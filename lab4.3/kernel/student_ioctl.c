#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#define STUDENT_IOCTL_MAGIC 's'
#define IOCTL_HELLO _IOWR(STUDENT_IOCTL_MAGIC, 1, int)

static long student_unlocked_ioctl(struct file *file, unsigned int cmd,
                                   unsigned long arg) {
  int value;
  switch (cmd) {
  case IOCTL_HELLO:
    if (copy_from_user(&value, (int __user *)arg, sizeof(value)))
      return -EFAULT;

    pr_info("student_ioctl: IOCTL_HELLO called, value=%d\n", value);

    value += 42;
    if (copy_to_user((int __user *)arg, &value, sizeof(value)))
      return -EFAULT;
    return 0;

  default:
    return -EINVAL;
  }
}

static const struct file_operations student_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = student_unlocked_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = student_unlocked_ioctl,
#endif
};

static struct miscdevice student_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "student_ioctl",
    .fops = &student_fops,
    .mode = 0666,
};

static int __init student_init(void) {
  int ret = misc_register(&student_miscdev);
  if (ret)
    return ret;
  pr_info("student_ioctl: module loaded\n");
  return 0;
}

static void __exit student_exit(void) {
  misc_deregister(&student_miscdev);
  pr_info("student_ioctl: module unloaded\n");
}

module_init(student_init);
module_exit(student_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux Kernel Course");
MODULE_DESCRIPTION("Educational ioctl-based kernel interface");