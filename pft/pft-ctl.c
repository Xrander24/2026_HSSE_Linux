// SPDX-License-Identifier: GPL-2.0
/*
 * pft-ctl: userspace reader for the Page Fault Tracer.
 *
 * Reads fixed-size binary records from /sys/kernel/debug/pft/events
 * and prints a human-readable line per fault. Default mode blocks
 * until events are available. Ctrl-C to stop.
 *
 * Build (static so it runs in the busybox initramfs):
 *     gcc -Wall -O2 -static pft-ctl.c -o pft-ctl
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TASK_COMM_LEN 16
#define DEFAULT_PATH  "/sys/kernel/debug/pft/events"

/* MUST match struct pft_event in pft.c byte-for-byte. */
struct pft_event {
	uint64_t ts_ns;
	uint32_t pid;
	uint32_t tgid;
	char     comm[TASK_COMM_LEN];
	uint64_t addr;
	uint32_t err_code;
	uint32_t _pad;
} __attribute__((packed));

/* X86 page-fault error_code bits (see arch/x86/include/asm/trap_pf.h). */
#define X86_PF_PROT  (1u << 0)
#define X86_PF_WRITE (1u << 1)
#define X86_PF_USER  (1u << 2)
#define X86_PF_RSVD  (1u << 3)
#define X86_PF_INSTR (1u << 4)

static volatile sig_atomic_t stop;
static void on_sigint(int sig) { (void)sig; stop = 1; }

static const char *classify(uint32_t ec, char *out, size_t outsz)
{
	const char *mode = (ec & X86_PF_USER)  ? "U" : "K";
	const char *op   = (ec & X86_PF_INSTR) ? "X"
			 : (ec & X86_PF_WRITE) ? "W" : "R";
	const char *p    = (ec & X86_PF_PROT)  ? "PROT" : "NOTP";
	const char *cow  = ((ec & (X86_PF_WRITE | X86_PF_PROT))
			    == (X86_PF_WRITE | X86_PF_PROT)) ? " COW?" : "";
	snprintf(out, outsz, "%s/%s/%s%s", mode, op, p, cow);
	return out;
}

static int usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-n N] [-f PATH]\n"
		"  -n N     stop after N events (default: forever)\n"
		"  -f PATH  events file (default: %s)\n",
		prog, DEFAULT_PATH);
	return 2;
}

int main(int argc, char **argv)
{
	const char *path = DEFAULT_PATH;
	long limit = -1;
	int opt;

	while ((opt = getopt(argc, argv, "n:f:h")) != -1) {
		switch (opt) {
		case 'n': limit = strtol(optarg, NULL, 10); break;
		case 'f': path  = optarg;                    break;
		case 'h':
		default:  return usage(argv[0]);
		}
	}

	signal(SIGINT,  on_sigint);
	signal(SIGTERM, on_sigint);

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "pft-ctl: open(%s): %s\n", path, strerror(errno));
		return 1;
	}

	printf("%-13s %6s %6s %-16s %-18s %-4s %-12s\n",
	       "TIME(s.ns)", "PID", "TGID", "COMM", "ADDR", "EC", "KIND");

	long n = 0;
	while (!stop && (limit < 0 || n < limit)) {
		struct pft_event ev;
		ssize_t r = read(fd, &ev, sizeof ev);
		if (r == 0) {        /* EOF should not happen on this file */
			break;
		}
		if (r < 0) {
			if (errno == EINTR) continue;
			fprintf(stderr, "pft-ctl: read: %s\n", strerror(errno));
			break;
		}
		if (r != sizeof ev) {
			fprintf(stderr, "pft-ctl: short read (%zd of %zu)\n",
				r, sizeof ev);
			continue;
		}

		char kind[24];
		classify(ev.err_code, kind, sizeof kind);

		/* Make sure comm is NUL-terminated for printing. */
		char comm[TASK_COMM_LEN + 1];
		memcpy(comm, ev.comm, TASK_COMM_LEN);
		comm[TASK_COMM_LEN] = '\0';

		printf("%5" PRIu64 ".%09" PRIu64 " %6u %6u %-16s 0x%016" PRIx64
		       " 0x%02x %-12s\n",
		       ev.ts_ns / 1000000000ULL,
		       ev.ts_ns % 1000000000ULL,
		       ev.pid, ev.tgid, comm,
		       ev.addr, ev.err_code, kind);
		n++;
	}

	close(fd);
	return 0;
}
