// SPDX-License-Identifier: GPL-2.0
/*
 * pft-bench: tiny page-fault generator for measuring tracer overhead.
 *
 *   pft-bench <pages> [iter]
 *
 * Allocates <pages> pages with mmap(MAP_ANON|MAP_PRIVATE), then touches
 * every page once - exactly <pages> minor faults per iteration. With
 * MAP_POPULATE removed this is the canonical lazy demand-paging workload.
 *
 * Run twice: once with /pft.ko loaded, once without (or with
 *   echo 'filter pid 1' > /proc/pft/control
 * to neutralize tracing for everything but PID 1). Compare timings.
 *
 * Build: gcc -Wall -O2 -static pft-bench.c -o pft-bench
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <pages> [iter]\n", argv[0]);
		return 2;
	}

	long pages = strtol(argv[1], NULL, 10);
	long iter  = argc > 2 ? strtol(argv[2], NULL, 10) : 1;
	if (pages <= 0 || iter <= 0) {
		fprintf(stderr, "pages and iter must be > 0\n");
		return 2;
	}

	long pgsz = sysconf(_SC_PAGESIZE);
	size_t sz = (size_t)pages * pgsz;

	double t0 = now_s();
	for (long it = 0; it < iter; it++) {
		char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
			       MAP_ANON | MAP_PRIVATE, -1, 0);
		if (p == MAP_FAILED) { perror("mmap"); return 1; }
		for (size_t i = 0; i < sz; i += pgsz)
			p[i] = (char)i;     /* one minor fault per page */
		munmap(p, sz);
	}
	double t1 = now_s();

	double total = t1 - t0;
	double per_fault_us = (total * 1e6) / ((double)pages * iter);
	printf("pid=%d pages=%ld iter=%ld total=%.3f s per-fault=%.3f us\n",
	       getpid(), pages, iter, total, per_fault_us);
	return 0;
}
