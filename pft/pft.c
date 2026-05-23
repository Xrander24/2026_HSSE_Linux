// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/tracepoint.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>

#include <asm/trap_pf.h>

/*
 * Wire-format record exposed to userspace via /sys/kernel/debug/pft/events.
 * Userspace just reads sizeof(struct pft_event) bytes at a time.
 * Keep the layout stable: this is effectively a binary ABI between the
 * module and the userspace reader (added on day 3).
 */
struct pft_event {
	u64  ts_ns;          /* monotonic timestamp */
	u32  pid;            /* task->pid */
	u32  tgid;           /* task->tgid (the "process" id in userspace) */
	char comm[TASK_COMM_LEN]; /* current->comm; 16 bytes */
	u64  addr;           /* faulting virtual address */
	u32  err_code;       /* X86_PF_* bits */
	u32  _pad;           /* explicit pad so sizeof is stable across compilers */
};

/*
 * Why a kfifo:
 *  - Producer (probe) runs in atomic context on potentially many CPUs.
 *  - Consumer (read syscall) runs in process context and can sleep.
 *  - kfifo gives us a lock-friendly SPSC/MPSC ring buffer and a typed
 *    API: kfifo_in_spinlocked() / kfifo_to_user().
 *
 * Size must be a power of 2 for the typed kfifo macros. 1024 * 48 bytes
 * ~= 48 KiB in the module's .bss. Enough for bursts; older events get
 * dropped (we count drops separately).
 */
#define PFT_FIFO_LEN 1024
static DEFINE_KFIFO(pft_fifo, struct pft_event, PFT_FIFO_LEN);
static DEFINE_SPINLOCK(pft_lock);
static DECLARE_WAIT_QUEUE_HEAD(pft_wq);

static struct tracepoint *tp_user;
static struct tracepoint *tp_kernel;

static atomic64_t pft_total;
static atomic64_t pft_user;
static atomic64_t pft_kernel;
static atomic64_t pft_write;
static atomic64_t pft_cow_hint;
static atomic64_t pft_dropped;   /* events lost because the fifo was full */

static struct dentry *pft_debug_dir;

/*
 * record_event() is the hot path. Keep it small and allocation-free.
 * Called from probe context (atomic, RCU read-side, preemption off).
 */
static __always_inline void record_event(unsigned long address,
					 unsigned long error_code)
{
	struct pft_event ev;

	ev.ts_ns    = ktime_get_ns();
	ev.pid      = current->pid;
	ev.tgid     = current->tgid;
	ev.addr     = address;
	ev.err_code = (u32)error_code;
	ev._pad     = 0;
	memcpy(ev.comm, current->comm, sizeof(ev.comm));

	/*
	 * kfifo_in_spinlocked: producer side, takes pft_lock. Returns the
	 * number of elements actually written (0 if the fifo was full).
	 * We never block here - dropping is by design.
	 */
	if (!kfifo_in_spinlocked(&pft_fifo, &ev, 1, &pft_lock))
		atomic64_inc(&pft_dropped);
	else
		wake_up_interruptible(&pft_wq);
}

static void probe_page_fault_user(void *data,
				  unsigned long address,
				  struct pt_regs *regs,
				  unsigned long error_code)
{
	atomic64_inc(&pft_total);
	atomic64_inc(&pft_user);

	if (error_code & X86_PF_WRITE)
		atomic64_inc(&pft_write);
	if ((error_code & (X86_PF_WRITE | X86_PF_PROT)) ==
	    (X86_PF_WRITE | X86_PF_PROT))
		atomic64_inc(&pft_cow_hint);

	record_event(address, error_code);
}

static void probe_page_fault_kernel(void *data,
				    unsigned long address,
				    struct pt_regs *regs,
				    unsigned long error_code)
{
	atomic64_inc(&pft_total);
	atomic64_inc(&pft_kernel);

	if (error_code & X86_PF_WRITE)
		atomic64_inc(&pft_write);

	record_event(address, error_code);
}

static void pft_tp_lookup(struct tracepoint *tp, void *priv)
{
	if (!strcmp(tp->name, "page_fault_user"))
		tp_user = tp;
	else if (!strcmp(tp->name, "page_fault_kernel"))
		tp_kernel = tp;
}

/* ----- /sys/kernel/debug/pft/events ----- */

static ssize_t pft_events_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	unsigned int copied = 0;
	int ret;

	/*
	 * Block until at least one event is available, unless O_NONBLOCK.
	 * wait_event_interruptible re-checks the condition under the
	 * wait-queue lock to avoid the lost-wakeup race with wake_up().
	 */
	if (kfifo_is_empty(&pft_fifo)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(pft_wq,
					       !kfifo_is_empty(&pft_fifo));
		if (ret)               /* -ERESTARTSYS on signal */
			return ret;
	}

	/*
	 * kfifo_to_user copies up to (count / sizeof(elem)) elements out
	 * to the user buffer. It takes care of the wrap and short copies.
	 * We pass &pft_lock implicitly via the spinlocked variant.
	 */
	ret = kfifo_to_user(&pft_fifo, buf, count, &copied);
	if (ret)
		return ret;
	return copied;
}

static __poll_t pft_events_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &pft_wq, wait);
	if (!kfifo_is_empty(&pft_fifo))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations pft_events_fops = {
	.owner   = THIS_MODULE,
	.open    = simple_open,
	.read    = pft_events_read,
	.poll    = pft_events_poll,
	.llseek  = no_llseek,
};

/* ----- init / exit ----- */

static int __init pft_init(void)
{
	int ret;

	/*
	 * Create debugfs BEFORE registering probes: if creation fails we
	 * don't have probes firing into a half-built world.
	 */
	pft_debug_dir = debugfs_create_dir("pft", NULL);
	if (IS_ERR(pft_debug_dir)) {
		ret = PTR_ERR(pft_debug_dir);
		pr_err("pft: debugfs_create_dir failed: %d\n", ret);
		return ret;
	}
	debugfs_create_file("events", 0400, pft_debug_dir, NULL,
			    &pft_events_fops);

	for_each_kernel_tracepoint(pft_tp_lookup, NULL);
	if (!tp_user || !tp_kernel) {
		pr_err("pft: page_fault tracepoints not found\n");
		ret = -ENODEV;
		goto err_debugfs;
	}

	ret = tracepoint_probe_register(tp_user, probe_page_fault_user, NULL);
	if (ret) {
		pr_err("pft: register(user) failed: %d\n", ret);
		goto err_debugfs;
	}
	ret = tracepoint_probe_register(tp_kernel, probe_page_fault_kernel, NULL);
	if (ret) {
		pr_err("pft: register(kernel) failed: %d\n", ret);
		tracepoint_probe_unregister(tp_user, probe_page_fault_user, NULL);
		tracepoint_synchronize_unregister();
		goto err_debugfs;
	}

	pr_info("pft: loaded - events at /sys/kernel/debug/pft/events (record=%zu bytes)\n",
		sizeof(struct pft_event));
	return 0;

err_debugfs:
	debugfs_remove_recursive(pft_debug_dir);
	pft_debug_dir = NULL;
	return ret;
}

static void __exit pft_exit(void)
{
	/*
	 * Strict teardown order:
	 *   1) unregister probes  -> no NEW probe calls will start
	 *   2) synchronize        -> wait for IN-FLIGHT probes to finish
	 *   3) remove debugfs     -> safe: no probe can wake_up_interruptible
	 *                           on the wait queue anymore
	 * Doing it in any other order is racy.
	 */
	if (tp_user)
		tracepoint_probe_unregister(tp_user, probe_page_fault_user, NULL);
	if (tp_kernel)
		tracepoint_probe_unregister(tp_kernel, probe_page_fault_kernel, NULL);

	tracepoint_synchronize_unregister();

	debugfs_remove_recursive(pft_debug_dir);

	pr_info("pft: unloaded - total=%lld user=%lld kernel=%lld write=%lld cow_hint=%lld dropped=%lld\n",
		(long long)atomic64_read(&pft_total),
		(long long)atomic64_read(&pft_user),
		(long long)atomic64_read(&pft_kernel),
		(long long)atomic64_read(&pft_write),
		(long long)atomic64_read(&pft_cow_hint),
		(long long)atomic64_read(&pft_dropped));
}

module_init(pft_init);
module_exit(pft_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Xrander24");
MODULE_DESCRIPTION("Page Fault Tracer (educational)");
MODULE_VERSION("0.2");
