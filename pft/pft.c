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
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>

#include <asm/trap_pf.h>

/* ----- wire-format event (binary ABI to userspace) ----- */

struct pft_event {
	u64  ts_ns;
	u32  pid;
	u32  tgid;
	char comm[TASK_COMM_LEN];
	u64  addr;
	u32  err_code;
	u32  _pad;
};

/* ----- ring buffer + reader wait queue ----- */

#define PFT_FIFO_LEN 1024
static DEFINE_KFIFO(pft_fifo, struct pft_event, PFT_FIFO_LEN);
static DEFINE_SPINLOCK(pft_fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(pft_wq);

/* ----- per-PID stats hash table ----- */

struct pft_pid_stat {
	u32  pid;
	u32  count;
	char comm[TASK_COMM_LEN];
	struct hlist_node node;
};

#define PFT_PID_HASH_BITS 8     /* 256 buckets */
static DEFINE_HASHTABLE(pft_pid_table, PFT_PID_HASH_BITS);
static DEFINE_SPINLOCK(pft_pid_lock);

/* ----- tracepoint pointers & counters ----- */

static struct tracepoint *tp_user;
static struct tracepoint *tp_kernel;

static atomic64_t pft_total;
static atomic64_t pft_user;
static atomic64_t pft_kernel;
static atomic64_t pft_write;
static atomic64_t pft_cow_hint;
static atomic64_t pft_dropped;       /* lost due to full fifo */
static atomic64_t pft_pid_alloc_fail; /* lost due to GFP_ATOMIC OOM in hash */

/* ----- debugfs / procfs handles ----- */

static struct dentry         *pft_debug_dir;
static struct proc_dir_entry *pft_proc_dir;

/* ----- hot path: bump per-PID counter, allocating an entry if first sight ----- */
static void account_pid(u32 pid, u32 tgid, const char *comm)
{
	struct pft_pid_stat *e;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&pft_pid_lock, flags);
	hash_for_each_possible(pft_pid_table, e, node, pid) {
		if (e->pid == pid) {
			e->count++;
			/* refresh comm in case the task did execve() */
			memcpy(e->comm, comm, TASK_COMM_LEN);
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&pft_pid_lock, flags);

	if (found)
		return;

	/*
	 * First-sight PID: allocate a new entry. We're in atomic context
	 * (probe), so GFP_ATOMIC. If it fails we just drop into a counter -
	 * the total/global stats are still correct, only per-PID granularity
	 * suffers.
	 */
	e = kmalloc(sizeof(*e), GFP_ATOMIC);
	if (!e) {
		atomic64_inc(&pft_pid_alloc_fail);
		return;
	}
	e->pid = pid;
	e->count = 1;
	memcpy(e->comm, comm, TASK_COMM_LEN);

	spin_lock_irqsave(&pft_pid_lock, flags);
	/* Re-check: another CPU may have inserted this PID between
	 * our lookup and now. If so, drop our fresh entry. */
	{
		struct pft_pid_stat *other;
		hash_for_each_possible(pft_pid_table, other, node, pid) {
			if (other->pid == pid) {
				other->count++;
				memcpy(other->comm, comm, TASK_COMM_LEN);
				spin_unlock_irqrestore(&pft_pid_lock, flags);
				kfree(e);
				return;
			}
		}
	}
	hash_add(pft_pid_table, &e->node, e->pid);
	spin_unlock_irqrestore(&pft_pid_lock, flags);
}

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

	if (!kfifo_in_spinlocked(&pft_fifo, &ev, 1, &pft_fifo_lock))
		atomic64_inc(&pft_dropped);
	else
		wake_up_interruptible(&pft_wq);

	account_pid(ev.pid, ev.tgid, ev.comm);
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

/* ===== /sys/kernel/debug/pft/events ===== */

static ssize_t pft_events_read(struct file *file, char __user *buf,
			       size_t count, loff_t *ppos)
{
	unsigned int copied = 0;
	int ret;

	if (kfifo_is_empty(&pft_fifo)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(pft_wq,
					       !kfifo_is_empty(&pft_fifo));
		if (ret)
			return ret;
	}

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
	.llseek  = noop_llseek,
};

/* ===== /proc/pft/stats (seq_file) ===== */

#define PFT_TOP_N 5

struct pft_top_entry {
	u32  pid;
	u32  count;
	char comm[TASK_COMM_LEN];
};

static int pft_stats_show(struct seq_file *m, void *v)
{
	struct pft_top_entry top[PFT_TOP_N] = {0};
	struct pft_pid_stat *e;
	unsigned long flags;
	int bkt, i, j;
	unsigned int unique = 0;

	seq_printf(m, "total            %lld\n",
		   (long long)atomic64_read(&pft_total));
	seq_printf(m, "user             %lld\n",
		   (long long)atomic64_read(&pft_user));
	seq_printf(m, "kernel           %lld\n",
		   (long long)atomic64_read(&pft_kernel));
	seq_printf(m, "write            %lld\n",
		   (long long)atomic64_read(&pft_write));
	seq_printf(m, "cow_hint         %lld\n",
		   (long long)atomic64_read(&pft_cow_hint));
	seq_printf(m, "dropped_fifo     %lld\n",
		   (long long)atomic64_read(&pft_dropped));
	seq_printf(m, "dropped_pid_oom  %lld\n",
		   (long long)atomic64_read(&pft_pid_alloc_fail));

	/*
	 * Collect top-N in a single pass through the hash table while
	 * holding the spinlock. We keep top[] as an unsorted small set
	 * with a running minimum index - O(N * top_n), fine for top_n=5.
	 */
	spin_lock_irqsave(&pft_pid_lock, flags);
	hash_for_each(pft_pid_table, bkt, e, node) {
		int min_i = 0;
		unique++;
		for (i = 1; i < PFT_TOP_N; i++)
			if (top[i].count < top[min_i].count)
				min_i = i;
		if (e->count > top[min_i].count) {
			top[min_i].pid   = e->pid;
			top[min_i].count = e->count;
			memcpy(top[min_i].comm, e->comm, TASK_COMM_LEN);
		}
	}
	spin_unlock_irqrestore(&pft_pid_lock, flags);

	/* Insertion sort by count desc - PFT_TOP_N is tiny. */
	for (i = 1; i < PFT_TOP_N; i++) {
		struct pft_top_entry key = top[i];
		j = i - 1;
		while (j >= 0 && top[j].count < key.count) {
			top[j + 1] = top[j];
			j--;
		}
		top[j + 1] = key;
	}

	seq_printf(m, "unique_pids      %u\n\n", unique);
	seq_puts(m, "top pids:\n");
	seq_puts(m, "   pid   count comm\n");
	for (i = 0; i < PFT_TOP_N; i++) {
		if (!top[i].count)
			continue;
		seq_printf(m, "%6u %7u %.*s\n",
			   top[i].pid, top[i].count,
			   TASK_COMM_LEN, top[i].comm);
	}

	return 0;
}

static int pft_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, pft_stats_show, NULL);
}

static const struct proc_ops pft_stats_proc_ops = {
	.proc_open    = pft_stats_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ===== init / exit ===== */

static void pft_hash_drain(void)
{
	struct pft_pid_stat *e;
	struct hlist_node *tmp;
	int bkt;

	/*
	 * Called only after tracepoint_synchronize_unregister(), so we
	 * know no probe is concurrently inserting. No lock needed, but
	 * we take it anyway for symmetry / future-proofing.
	 */
	spin_lock(&pft_pid_lock);
	hash_for_each_safe(pft_pid_table, bkt, tmp, e, node) {
		hash_del(&e->node);
		kfree(e);
	}
	spin_unlock(&pft_pid_lock);
}

static int __init pft_init(void)
{
	int ret;

	pft_debug_dir = debugfs_create_dir("pft", NULL);
	if (IS_ERR(pft_debug_dir)) {
		ret = PTR_ERR(pft_debug_dir);
		pr_err("pft: debugfs_create_dir failed: %d\n", ret);
		return ret;
	}
	debugfs_create_file("events", 0400, pft_debug_dir, NULL,
			    &pft_events_fops);

	pft_proc_dir = proc_mkdir("pft", NULL);
	if (!pft_proc_dir) {
		pr_err("pft: proc_mkdir failed\n");
		ret = -ENOMEM;
		goto err_debugfs;
	}
	if (!proc_create("stats", 0444, pft_proc_dir, &pft_stats_proc_ops)) {
		pr_err("pft: proc_create stats failed\n");
		ret = -ENOMEM;
		goto err_proc;
	}

	for_each_kernel_tracepoint(pft_tp_lookup, NULL);
	if (!tp_user || !tp_kernel) {
		pr_err("pft: page_fault tracepoints not found\n");
		ret = -ENODEV;
		goto err_proc;
	}

	ret = tracepoint_probe_register(tp_user, probe_page_fault_user, NULL);
	if (ret) {
		pr_err("pft: register(user) failed: %d\n", ret);
		goto err_proc;
	}
	ret = tracepoint_probe_register(tp_kernel, probe_page_fault_kernel, NULL);
	if (ret) {
		pr_err("pft: register(kernel) failed: %d\n", ret);
		tracepoint_probe_unregister(tp_user, probe_page_fault_user, NULL);
		tracepoint_synchronize_unregister();
		goto err_proc;
	}

	pr_info("pft: loaded - /proc/pft/stats, /sys/kernel/debug/pft/events (record=%zu B)\n",
		sizeof(struct pft_event));
	return 0;

err_proc:
	proc_remove(pft_proc_dir);
	pft_proc_dir = NULL;
err_debugfs:
	debugfs_remove_recursive(pft_debug_dir);
	pft_debug_dir = NULL;
	return ret;
}

static void __exit pft_exit(void)
{
	if (tp_user)
		tracepoint_probe_unregister(tp_user, probe_page_fault_user, NULL);
	if (tp_kernel)
		tracepoint_probe_unregister(tp_kernel, probe_page_fault_kernel, NULL);

	tracepoint_synchronize_unregister();

	proc_remove(pft_proc_dir);
	debugfs_remove_recursive(pft_debug_dir);

	pft_hash_drain();

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
MODULE_VERSION("0.3");
