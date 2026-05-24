#include <stdio.h>
#include <sys/syscall.h>

int main(void) {
    long sr = syscall(666, "xrander24");

    printf("Retuen code %d\n", sr);
    return 0;
}