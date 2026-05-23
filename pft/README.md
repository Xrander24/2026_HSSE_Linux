# Page Fault Tracer (pft)

Educational Linux kernel module that traces x86 page faults and exposes
statistics to userspace. Built against Linux 6.19, runs in the course
QEMU + busybox initramfs setup.

## What it does

For every page fault on the system the module:

1. Bumps global atomic counters (total, user, kernel, write, CoW hint).
2. Records a fixed-size binary event into a `kfifo` ring buffer.
3. Updates a per-PID hash table (count + last `comm`).

Userspace can:

- read raw events from `/sys/kernel/debug/pft/events` (binary stream),
- read aggregated stats and top-PIDs from `/proc/pft/stats` (text),
- send commands to `/proc/pft/control` (reset, set/clear PID filter).

A small reader (`pft-ctl`) and a micro-bench (`pft-bench`) are shipped
alongside the module.

## Architecture

```
   userspace
   ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
   │ pft-ctl      │     │ cat /proc/   │     │ echo cmd >   │
   │ (reader)     │     │ pft/stats    │     │ /proc/pft/   │
   │              │     │              │     │ control      │
   └──────┬───────┘     └──────┬───────┘     └──────┬───────┘
          │ read()             │ read()             │ write()
   ─ ─ ─ ─┼─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┼ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─ ─ ─
          ▼                    ▼                    ▼
   ┌────────────────────────────────────────────────────────┐
   │  pft.ko                                                │
   │                                                        │
   │  /sys/kernel/debug/pft/events  -> kfifo<pft_event>     │
   │  /proc/pft/stats               -> seq_file show        │
   │  /proc/pft/control             -> command parser       │
   │                                                        │
   │  per-PID hash (DEFINE_HASHTABLE, spinlock)             │
   │  atomic64 counters + atomic filter_pid                 │
   │                                                        │
   │           ▲                                            │
   │           │ called from probe context (atomic)         │
   │  ┌────────┴─────────┐                                  │
   │  │ probe functions  │  registered via                  │
   │  │  user / kernel   │  tracepoint_probe_register()     │
   │  └────────▲─────────┘  on tracepoints discovered by    │
   │           │            for_each_kernel_tracepoint()    │
   └───────────┼────────────────────────────────────────────┘
               │
   ────────────┼──────────── kernel core ────────────────────
               │
        exceptions:page_fault_user
        exceptions:page_fault_kernel
        (defined in arch/x86/mm/fault.c)
```

## Why these design choices

**Tracepoints, not kprobes / not syscall hooks.**
`exceptions:page_fault_user` / `page_fault_kernel` are static tracepoints
with a stable signature: `(unsigned long address, struct pt_regs *regs,
unsigned long error_code)`. They are part of the kernel ABI for
observability tooling - they don't move between versions the way kprobe
targets do, and they cost nothing when no probe is attached (a single
static-branch check).

**Lookup-by-name registration, not `register_trace_*()`.**
`arch/x86/mm/fault.c` creates `__tracepoint_page_fault_user` (via
`CREATE_TRACE_POINTS`) but does *not* call
`EXPORT_TRACEPOINT_SYMBOL_GPL()` for it. An out-of-tree module that
expands the `register_trace_page_fault_user()` macro fails to link with
`modpost: __tracepoint_page_fault_user undefined`. The standard
workaround - also used by bpftrace and SystemTap - is to walk the
kernel-wide list with `for_each_kernel_tracepoint()`, match by name,
and call `tracepoint_probe_register(tp, probe, NULL)`. Both helpers are
exported `GPL`.

**Atomic context discipline.**
Probes run with preemption disabled and inside an RCU read-side critical
section. The code therefore:

- never sleeps - no mutex, no `down_*`, no `GFP_KERNEL`,
- only uses `atomic64_inc`, `kfifo_in_spinlocked`, `kmalloc(GFP_ATOMIC)`,
  spinlocks (`spin_lock_irqsave`),
- never copies to userspace from the probe path - all userspace transfer
  happens in process context inside `pft_events_read`.

**Per-PID hash with double-checked insertion.**
First-sight PID allocation must happen outside the spinlock (we can't
`kmalloc(GFP_ATOMIC)` under a held spinlock with IRQ-save in all configs).
The probe does:

1. Lookup under lock; if found, bump and return.
2. Drop lock; `kmalloc(GFP_ATOMIC)`.
3. Reacquire lock; *re-check* whether someone else inserted the PID
   in the meantime; if so, drop our entry. Otherwise, insert.

This is the classical double-checked-locking pattern; without the
re-check, a parallel probe on another CPU can introduce a duplicate.

**Two filesystems, two roles.**
`procfs` carries the stable, human-readable surfaces (`stats`,
`control`) - the kind of thing a sysadmin would `cat`. `debugfs` carries
the raw binary event stream (`events`) - the kind of thing a debug or
profiling tool consumes. This split is idiomatic in modern kernel code.

**Strict teardown order in `pft_exit`.**

```
   unregister probes
   tracepoint_synchronize_unregister()   <-- wait for in-flight probes
   proc_remove(...)
   debugfs_remove_recursive(...)
   pft_hash_drain()
```

If we remove the debugfs file before in-flight probes finish, a probe
that just woke up could still call `wake_up_interruptible(&pft_wq)`
while the wait queue is being torn down by another concurrent reader.
If we drain the hash before the synchronize, a probe could still insert
into a half-freed table. The pattern - "stop the producer, wait for the
producer to quiesce, then dismantle the rest" - is the only race-free
order.

## Interfaces

### `/sys/kernel/debug/pft/events` (mode 0400, read-only)

A stream of fixed-size `struct pft_event` records, 48 bytes each:

| offset | size | field    | type     |
|--------|------|----------|----------|
|  0     |  8   | ts_ns    | u64 (CLOCK_MONOTONIC ns) |
|  8     |  4   | pid      | u32      |
| 12     |  4   | tgid     | u32      |
| 16     | 16   | comm     | char[TASK_COMM_LEN] |
| 32     |  8   | addr     | u64 (faulting VA) |
| 40     |  4   | err_code | u32 (X86_PF_* bits) |
| 44     |  4   | _pad     | u32      |

Read is blocking by default; supports `O_NONBLOCK` and `poll(POLLIN)`.

### `/proc/pft/stats` (mode 0444, text)

```
total            <total>
user             <user>
kernel           <kernel>
write            <writes>
cow_hint         <heuristic CoW count>
dropped_fifo     <events lost because the ring was full>
dropped_pid_oom  <per-PID inserts dropped due to GFP_ATOMIC failure>
filter_pid       <N>   (0 = disabled)
unique_pids      <N>

top pids:
   pid   count comm
   ...
```

### `/proc/pft/control` (mode 0200, write-only)

Accepts one command per write:

- `reset` — zero all counters and drop the per-PID hash.
- `filter pid N` — keep only events with `current->pid == N`.
- `filter clear` (or `filter off`) — disable the PID filter.

```sh
echo "filter pid 42" > /proc/pft/control
echo reset           > /proc/pft/control
echo "filter clear"  > /proc/pft/control
```

## Decoding `err_code`

| bit | name        | 0 means         | 1 means              |
|-----|-------------|-----------------|----------------------|
|  0  | `X86_PF_PROT`  | not-present     | present, protection violation |
|  1  | `X86_PF_WRITE` | read            | write                |
|  2  | `X86_PF_USER`  | kernel-mode     | user-mode (CPL=3)    |
|  4  | `X86_PF_INSTR` | data access     | instruction fetch    |

Common values seen on busybox:

| `ec`  | meaning                                                     |
|-------|-------------------------------------------------------------|
| 0x04  | user, read, page not present → lazy demand paging           |
| 0x06  | user, write, not present → first write after `malloc`/`mmap`|
| 0x07  | user, write, present, not writable → **copy-on-write**       |
| 0x14  | user, instruction fetch, not present → `.text` page load    |

## Building & running

On the Linux build host:

```sh
cd pft
make            # builds pft.ko, pft-ctl, pft-bench
make install    # copies them + demo.sh into ../root/
# rebuild initramfs.gz and boot QEMU as usual
```

In the QEMU shell:

```sh
mount -t debugfs none /sys/kernel/debug 2>/dev/null
insmod /pft.ko
/demo.sh                    # quick end-to-end demonstration
cat /proc/pft/stats         # aggregated view
/pft-ctl                    # tail the event stream
```

## Defense scenario (suggested)

1. `insmod /pft.ko` → show `dmesg | tail`.
2. `/demo.sh` → end-to-end picture: stats, top PIDs, sample events.
3. Demonstrate CoW: start `/pft-bench 32` in one process, fork in shell,
   show `cow_hint` rising.
4. Demonstrate filtering:
   ```sh
   /pft-bench 100 &
   echo "filter pid $!" > /proc/pft/control
   wait
   cat /proc/pft/stats           # only that PID is in stats
   echo "filter clear" > /proc/pft/control
   ```
5. Measure overhead with `/pft-bench`:
   ```sh
   rmmod pft                     # baseline
   /pft-bench 10000 5
   insmod /pft.ko
   /pft-bench 10000 5            # with tracer
   ```
   Compare per-fault microseconds.
6. `rmmod pft` → `dmesg | tail` shows final counters; demonstrate safe
   unload via `tracepoint_synchronize_unregister()`.

## Known limitations

- **x86-only.** The page-fault tracepoints we consume are defined in
  `arch/x86/mm/fault.c`. Other architectures expose page faults
  differently (e.g. `handle_mm_fault` kprobe, perf SW events). Picking
  one stable interface was deliberate.
- **minor vs major faults are heuristic.** The page-fault tracepoint
  fires before the kernel decides whether to read from disk
  (`do_swap_page`, `filemap_fault`). To get a precise minor/major split
  we would also need to hook one of those points, or use the perf SW
  counters `PERF_COUNT_SW_PAGE_FAULTS_{MIN,MAJ}`.
- **CoW detection is a heuristic.** The combination `X86_PF_WRITE |
  X86_PF_PROT` covers CoW after `fork()` *and* write-after-`mprotect`.
  We label this `cow_hint`, not `cow_count`, to be honest.
- **PID reuse is not tracked.** When PID is recycled after a task exits,
  the old hash entry survives until module reset. For an educational
  module that is fine; a production tool would hook `sched_process_exit`.
- **Ring buffer drops under burst load.** The kfifo holds 1024 events;
  bursts past that increment `dropped_fifo`. Sizing is a trade-off
  between memory and capture fidelity.

## File layout

```
pft/
├── Makefile         build module + userspace tools
├── pft.c            kernel module
├── pft-ctl.c        userspace event reader
├── pft-bench.c      synthetic page-fault generator
├── demo.sh          one-shot demonstration
└── README.md        this file
```
