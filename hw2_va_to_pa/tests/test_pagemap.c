#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#ifndef __NR_va_to_pa
#define __NR_va_to_pa 667
#endif

#define PAGEMAP_ENTRY_BYTES 8
#define PFN_MASK ((1ULL << 55) - 1)

static int syscall_va_to_pa(unsigned long va, unsigned long *pa)
{
    long ret;

    errno = 0;
    ret = syscall(__NR_va_to_pa, getpid(), va, pa);
    if (ret < 0) {
        printf("syscall va_to_pa(0x%lx) failed: errno=%d (%s)\n",
               va, errno, strerror(errno));
        return -1;
    }

    return 0;
}

static int pagemap_va_to_pa(unsigned long va, unsigned long *pa)
{
    int fd;
    uint64_t entry;
    ssize_t nread;
    long page_size;
    uint64_t vpn;
    off_t offset;
    uint64_t pfn;
    unsigned long page_offset;

    page_size = sysconf(_SC_PAGESIZE);
    vpn = va / page_size;
    offset = (off_t)(vpn * PAGEMAP_ENTRY_BYTES);

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        perror("open /proc/self/pagemap");
        return -1;
    }

    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        perror("lseek pagemap");
        close(fd);
        return -1;
    }

    nread = read(fd, &entry, sizeof(entry));
    close(fd);

    if (nread != sizeof(entry)) {
        perror("read pagemap");
        return -1;
    }

    printf("pagemap entry for VA 0x%lx: 0x%016llx\n",
           va, (unsigned long long)entry);

    if (!(entry & (1ULL << 63))) {
        printf("pagemap: page is not present\n");
        return -1;
    }

    pfn = entry & PFN_MASK;

    if (pfn == 0) {
        printf("pagemap: PFN is zero. Kernel may hide PFNs without proper permissions.\n");
        return -1;
    }

    page_offset = va % page_size;
    *pa = (unsigned long)((pfn * page_size) + page_offset);

    return 0;
}

static void wait_enter(const char *msg)
{
    printf("\n%s\n", msg);
    printf("Press ENTER to continue...\n");
    fflush(stdout);
    getchar();
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    char *buf;
    unsigned long va;
    unsigned long pa_syscall;
    unsigned long pa_pagemap;

    printf("=== test_pagemap: compare syscall with /proc/self/pagemap ===\n");
    printf("pid       : %d\n", getpid());
    printf("page_size : %ld\n", page_size);

    buf = mmap(NULL,
               page_size,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1,
               0);

    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    wait_enter("Page is mapped but not touched yet. You can inspect /proc/<pid>/maps.");

    printf("Touching memory at offset 123...\n");
    buf[123] = 42;

    va = (unsigned long)(buf + 123);

    wait_enter("Page was touched. Now comparing syscall and pagemap.");

    if (syscall_va_to_pa(va, &pa_syscall))
        return 1;

    if (pagemap_va_to_pa(va, &pa_pagemap))
        return 1;

    printf("\nVA              : 0x%lx\n", va);
    printf("PA from syscall : 0x%lx\n", pa_syscall);
    printf("PA from pagemap : 0x%lx\n", pa_pagemap);

    if (pa_syscall == pa_pagemap) {
        printf("[OK] syscall result matches /proc/self/pagemap\n");
    } else {
        printf("[FAIL] mismatch\n");
    }

    wait_enter("Test finished. You can inspect proc files before exit.");

    munmap(buf, page_size);
    return 0;
}
