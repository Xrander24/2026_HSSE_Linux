#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef __NR_va_to_pa
#define __NR_va_to_pa 667
#endif

static int va_to_pa_pid(pid_t pid, unsigned long va, unsigned long *pa)
{
    long ret;

    errno = 0;
    ret = syscall(__NR_va_to_pa, pid, va, pa);
    if (ret < 0) {
        printf("va_to_pa(pid=%d, va=0x%lx) failed: errno=%d (%s)\n",
               pid, va, errno, strerror(errno));
        return -1;
    }

    return 0;
}

static int va_to_pa_self(unsigned long va, unsigned long *pa)
{
    return va_to_pa_pid(getpid(), va, pa);
}

static void wait_enter(const char *who, const char *msg)
{
    printf("\n[%s] %s\n", who, msg);
    printf("[%s] Press ENTER to continue...\n", who);
    fflush(stdout);
    getchar();
}

int main(void)
{
    long page_size = sysconf(_SC_PAGESIZE);
    char *buf;
    unsigned long va;
    unsigned long parent_pa_before;
    unsigned long parent_pa_after;
    pid_t child;

    printf("=== test_fork_cow: fork and copy-on-write ===\n");
    printf("parent pid: %d\n", getpid());
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

    buf[0] = 42;
    va = (unsigned long)buf;

    if (va_to_pa_self(va, &parent_pa_before))
        return 1;

    printf("[parent] VA=%p -> PA before fork=0x%lx\n",
           buf, parent_pa_before);

    wait_enter("parent", "Before fork. You can inspect /proc/<parent_pid>/maps.");

    child = fork();

    if (child < 0) {
        perror("fork");
        return 1;
    }

    if (child == 0) {
        unsigned long child_pa_before_write;
        unsigned long child_pa_after_write;

        printf("\n[child] child pid: %d, parent pid: %d\n",
               getpid(), getppid());
        printf("[child] inherited same VA: %p\n", buf);

        if (va_to_pa_self(va, &child_pa_before_write))
            return 1;

        printf("[child] VA=%p -> PA before write=0x%lx\n",
               buf, child_pa_before_write);

        wait_enter("child", "Before write. Usually PA is same as parent due to shared COW page.");

        printf("[child] Writing to buf[0]. This should trigger COW...\n");
        buf[0] = 99;

        if (va_to_pa_self(va, &child_pa_after_write))
            return 1;

        printf("[child] VA=%p -> PA after write =0x%lx\n",
               buf, child_pa_after_write);

        if (child_pa_after_write != child_pa_before_write) {
            printf("[child] [OK] PA changed after write: COW happened\n");
        } else {
            printf("[child] [NOTE] PA did not change. This can happen depending on kernel/config/timing, but usually COW changes it.\n");
        }

        wait_enter("child", "Child finished. Inspect /proc/<child_pid>/maps if needed.");
        return 0;
    } else {
        int status;

        printf("[parent] forked child pid: %d\n", child);

        wait_enter("parent", "After fork, before parent writes. Child may be waiting too.");

        if (va_to_pa_self(va, &parent_pa_after))
            return 1;

        printf("[parent] VA=%p -> PA after fork=0x%lx\n",
               buf, parent_pa_after);

        printf("[parent] parent value buf[0]=%d\n", buf[0]);

        wait(&status);
        printf("[parent] child exited, status=%d\n", status);

        printf("[parent] final buf[0]=%d\n", buf[0]);
        printf("[parent] If child wrote 99, parent still sees 42 because of COW isolation.\n");

        munmap(buf, page_size);
    }

    return 0;
}
