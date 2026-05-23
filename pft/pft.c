// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/sched.h>      /* current */
#include <linux/tracepoint.h>

#include <asm/trap_pf.h>

/*
 * Why we do NOT use register_trace_page_fault_user() / the
 * <trace/events/exceptions.h> macros here:
 *
 * The tracepoints page_fault_user / page_fault_kernel are defined
 * (CREATE_TRACE_POINTS) in arch/x86/mm/fault.c, but the kernel does
 * not call EXPORT_TRACEPOINT_SYMBOL_GPL() for them. So the symbol
 * __tracepoint_page_fault_user exists in vmlinux but is invisible to
 * out-of-tree modules - the linker rejects our .ko with
 * "modpost: __tracepoint_page_fault_user undefined".
 *
 * Workaround (the standard out-of-tree pattern, also used by bpftrace
 * and SystemTap): iterate the kernel-side list of all tracepoints with
 * for_each_kernel_tracepoint(), match by name, and attach a probe
 * via tracepoint_probe_register(). Both helpers ARE exported GPL.
 */

static struct tracepoint *tp_user;
static struct tracepoint *tp_kernel;

static atomic64_t pft_total;
static atomic64_t pft_user;
static atomic64_t pft_kernel;
static atomic64_t pft_write;
static atomic64_t pft_cow_hint;

/*
 * Probe context: called from the page-fault handler with preemption
 * disabled and inside an RCU read-side critical section. That means:
 *   - MUST NOT sleep (no mutexes, no GFP_KERNEL allocations).
 *   - MUST be fast - every userspace fault on the system goes through
 *     here, so even a few microseconds of overhead per call adds up.
 * Atomics + printk_ratelimited are both safe in this context.
 */
static void probe_page_fault_user(void *data,
				  unsigned long address,
				  struct pt_regs *regs,
				  unsigned long error_code)
{
	atomic64_inc(&pft_total);
	atomic64_inc(&pft_user);

	if (error_code & X86_PF_WRITE)
		atomic64_inc(&pft_write);

	/*
	 * CoW heuristic: a write that took a protection fault on a page
	 * that IS present (X86_PF_PROT set) is almost always copy-on-write
	 * after a fork, or a write to a read-only mapping. Heuristic, not
	 * a guarantee - explained on defense.
	 */
	if ((error_code & (X86_PF_WRITE | X86_PF_PROT)) ==
	    (X86_PF_WRITE | X86_PF_PROT))
		atomic64_inc(&pft_cow_hint);

	printk_ratelimited(KERN_DEBUG
		"pft: user pid=%d comm=%s addr=0x%lx ec=0x%lx\n",
		current->pid, current->comm, address, error_code);
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
}

/*
 * Callback for for_each_kernel_tracepoint(). Called once per registered
 * kernel tracepoint with its struct tracepoint*. We just stash pointers
 * to the two we care about.
 */
static void pft_tp_lookup(struct tracepoint *tp, void *priv)
{
	if (!strcmp(tp->name, "page_fault_user"))
		tp_user = tp;
	else if (!strcmp(tp->name, "page_fault_kernel"))
		tp_kernel = tp;
}

static int __init pft_init(void)
{
	int ret;

	for_each_kernel_tracepoint(pft_tp_lookup, NULL);

	if (!tp_user || !tp_kernel) {
		pr_err("pft: could not find page_fault tracepoints (user=%p kernel=%p)\n",
		       tp_user, tp_kernel);
		return -ENODEV;
	}

	ret = tracepoint_probe_register(tp_user, probe_page_fault_user, NULL);
	if (ret) {
		pr_err("pft: tracepoint_probe_register(user) failed: %d\n", ret);
		return ret;
	}

	ret = tracepoint_probe_register(tp_kernel, probe_page_fault_kernel, NULL);
	if (ret) {
		pr_err("pft: tracepoint_probe_register(kernel) failed: %d\n", ret);
		tracepoint_probe_unregister(tp_user, probe_page_fault_user, NULL);
		tracepoint_synchronize_unregister();
		return ret;
	}

	pr_info("pft: loaded - tracing page faults via tracepoints\n");
	return 0;
}

static void __exit pft_exit(void)
{
	if (tp_user)
		tracepoint_probe_unregister(tp_user, probe_page_fault_user, NULL);
	if (tp_kernel)
		tracepoint_probe_unregister(tp_kernel, probe_page_fault_kernel, NULL);

	/*
	 * tracepoint_probe_unregister() only removes the probe from the
	 * RCU list. A probe call may already be in flight on another CPU.
	 * tracepoint_synchronize_unregister() waits an RCU grace period
	 * so we know no probe is running anymore - only then is it safe
	 * to free probe-touched data and let the module text be unloaded.
	 * Skipping this is a classic source of "BUG: unable to handle page
	 * fault" right after rmmod.
	 */
	tracepoint_synchronize_unregister();

	pr_info("pft: unloaded - total=%lld user=%lld kernel=%lld write=%lld cow_hint=%lld\n",
		(long long)atomic64_read(&pft_total),
		(long long)atomic64_read(&pft_user),
		(long long)atomic64_read(&pft_kernel),
		(long long)atomic64_read(&pft_write),
		(long long)atomic64_read(&pft_cow_hint));
}

module_init(pft_init);
module_exit(pft_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Xrander24");
MODULE_DESCRIPTION("Page Fault Tracer (educational)");
MODULE_VERSION("0.1");
