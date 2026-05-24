#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");

static int __init init_hello(void) {
    printk("Hello kernel\n");
    return 0;
}

static void __exit exit_hello(void) {
    printk("Bye kernel\n");
}

module_init(init_hello);
module_exit(exit_hello);