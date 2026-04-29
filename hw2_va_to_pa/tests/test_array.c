#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef __NR_va_to_pa
#define __NR_va_to_pa 667
#endif

static int va_to_pa(unsigned long va, unsigned long *pa)
{
    long ret;

    errno = 0;
    ret = syscall(__NR_va_to_pa, getpid(), va, pa);
    if (ret < 0) {
        printf("va_to_pa(0x%lx) failed: errno=%d (%s)\n",
               va, errno, strerror(errno));
        return -1;
    }

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
    size_t pages = 4;
    size_t len = pages * page_size;
    char *buf;
    unsigned long pa_base;
    unsigned long pa_plus_1;
    unsigned long pa_plus_100;
    unsigned long pa_last;
    int i;

    printf("=== test_array: VA -> PA for mmap'ed array ===\n");
    printf("pid       : %d\n", getpid());
    printf("page_size : %ld\n", page_size);

    buf = mmap(NULL,
               len,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1,
               0);

    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    printf("mapped virtual range: %p - %p (%zu bytes)\n",
           buf, buf + len, len);

    wait_enter("Memory is mapped, but pages may not be physically allocated yet. "
               "You can inspect /proc/<pid>/maps now.");

    printf("\nTouching every page...\n");
    for (i = 0; i < (int)pages; i++) {
        buf[i * page_size] = (char)(i + 1);
    }

    wait_enter("Pages were touched. Now physical pages should exist.");

    printf("\n--- Per-page translation ---\n");
    for (i = 0; i < (int)pages; i++) {
        unsigned long va = (unsigned long)(buf + i * page_size);
        unsigned long pa = 0;

        if (va_to_pa(va, &pa) == 0) {
            printf("page %d: VA=0x%lx -> PA=0x%lx\n", i, va, pa);
        }
    }

    printf("\n--- Offsets inside the first page ---\n");

    if (va_to_pa((unsigned long)(buf), &pa_base))
        goto out;

    if (va_to_pa((unsigned long)(buf + 1), &pa_plus_1))
        goto out;

    if (va_to_pa((unsigned long)(buf + 100), &pa_plus_100))
        goto out;

    if (va_to_pa((unsigned long)(buf + page_size - 1), &pa_last))
        goto out;

    printf("VA buf             = %p -> PA 0x%lx\n", buf, pa_base);
    printf("VA buf + 1         = %p -> PA 0x%lx, diff=%ld\n",
           buf + 1, pa_plus_1, (long)(pa_plus_1 - pa_base));
    printf("VA buf + 100       = %p -> PA 0x%lx, diff=%ld\n",
           buf + 100, pa_plus_100, (long)(pa_plus_100 - pa_base));
    printf("VA buf + page-1    = %p -> PA 0x%lx, diff=%ld\n",
           buf + page_size - 1, pa_last, (long)(pa_last - pa_base));

    if (pa_plus_1 == pa_base + 1 &&
        pa_plus_100 == pa_base + 100 &&
        pa_last == pa_base + page_size - 1) {
        printf("[OK] offsets inside one page are correct\n");
    } else {
        printf("[FAIL] offsets inside one page are not as expected\n");
    }

    wait_enter("Test finished. You can inspect /proc/<pid>/maps or press ENTER to exit.");

out:
    munmap(buf, len);
    return 0;
}
