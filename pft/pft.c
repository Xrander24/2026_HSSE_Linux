// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/atomic.h>
#include <linux/sched.h>      /* current */
#include <linux/tracepoint.h>

/*
 * We CONSUME these tracepoints - we do NOT define them. So we include
 * the header WITHOUT defining CREATE_TRACE_POINTS (that macro is only
 * used in the single translation unit that owns the tracepoint).
 */
#include <trace/events/exceptions.h>

#include <asm/trap_pf.h>

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
	 * after a fork, or a write to a read-only mapping. It is a
	 * heuristic, not a guarantee - we'll explain that on defense.
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

static int __init pft_init(void)
{
	int ret;

	ret = register_trace_page_fault_user(probe_page_fault_user, NULL);
	if (ret) {
		pr_err("pft: register page_fault_user failed: %d\n", ret);
		return ret;
	}

	ret = register_trace_page_fault_kernel(probe_page_fault_kernel, NULL);
	if (ret) {
		pr_err("pft: register page_fault_kernel failed: %d\n", ret);
		unregister_trace_page_fault_user(probe_page_fault_user, NULL);
		return ret;
	}

	pr_info("pft: loaded - tracing page faults via tracepoints\n");
	return 0;
}

static void __exit pft_exit(void)
{
	unregister_trace_page_fault_user(probe_page_fault_user, NULL);
	unregister_trace_page_fault_kernel(probe_page_fault_kernel, NULL);

	/*
	 * unregister_trace_*() only removes the probe from the callback
	 * list. A probe call may already be in flight on another CPU.
	 * tracepoint_synchronize_unregister() waits an RCU grace period
	 * so we know no probe is running anymore - only then is it safe
	 * to free probe-touched data and let the module text be unloaded.
	 * Skipping this call is a classic source of "kernel BUG: unable to
	 * handle page fault" right after rmmod.
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
