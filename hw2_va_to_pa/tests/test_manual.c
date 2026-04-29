#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

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

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    char *buf;
    unsigned long va;
    unsigned long pa;
    char line[128];

    printf("=== test_manual ===\n");
    printf("pid       : %d\n", getpid());
    printf("page_size : %ld\n", page_size);

    buf = mmap(NULL,
               page_size * 2,
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1,
               0);

    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    buf[0] = 1;
    buf[100] = 2;
    buf[page_size] = 3;

    printf("allocated range:\n");
    printf("  buf             = %p\n", buf);
    printf("  buf + 100       = %p\n", buf + 100);
    printf("  buf + page_size = %p\n", buf + page_size);
    printf("\nYou can also inspect:\n");
    printf("  cat /proc/%d/maps\n", getpid());
    printf("\nEnter virtual address in hex, for example: %lx\n", (unsigned long)buf);
    printf("Type q to exit.\n");

    while (1) {
        printf("\nva> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;

        if (line[0] == 'q' || line[0] == 'Q')
            break;

        va = strtoul(line, NULL, 16);

        if (va_to_pa(va, &pa) == 0) {
            printf("VA 0x%lx -> PA 0x%lx\n", va, pa);
        }
    }

    munmap(buf, page_size * 2);
    return 0;
}
