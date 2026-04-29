#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

#ifndef __NR_va_to_pa
#define __NR_va_to_pa 667
#endif

int main(void)
{
    unsigned long pa = 0;
    unsigned long va;
    char *buf;

    buf = (char *)malloc(4096);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    buf[0] = 42;

    va = (unsigned long)buf;

    long ret = syscall(__NR_va_to_pa, getpid(), va, &pa);

    if (ret < 0) {
        printf("syscall failed: ret=%ld errno=%d %s\n",
               ret, errno, strerror(errno));
        return 1;
    }

    printf("PID: %d\n", getpid());
    printf("VA : 0x%lx\n", va);
    printf("PA : 0x%lx\n", pa);

    free(buf);
    return 0;
}
