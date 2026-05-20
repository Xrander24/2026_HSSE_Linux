#include <linux/kernel.h>
#include <linux/syscalls.h>

SYSCALL_DEFINE1(say_hello, char *, msg)
{
    printk("Hello %s\n",msg);
    return 0;
}