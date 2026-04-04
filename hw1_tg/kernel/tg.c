#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Xrander24");
MODULE_DESCRIPTION("Telegram via FIFO");

#define NUM_CHATS 3
#define MAX_MSG_LEN 512
#define MAX_RESP_LEN 8192
#define FIFO_REQ "/tmp/tg_request"
#define FIFO_RESP "/tmp/tg_response"

static int major;
static struct class *tg_class;
static struct cdev tg_cdev;

static DEFINE_MUTEX(fifo_mutex);

static ssize_t kwrite_file(const char *path, const void *buf, size_t len) {
  struct file *f = filp_open(path, O_WRONLY, 0);
  ssize_t ret;
  if (IS_ERR(f))
    return PTR_ERR(f);
  ret = kernel_write(f, buf, len, &f->f_pos);
  filp_close(f, NULL);
  return ret;
}

static ssize_t kread_file(const char *path, void *buf, size_t len) {
  struct file *f = filp_open(path, O_RDONLY, 0);
  ssize_t ret;
  if (IS_ERR(f))
    return PTR_ERR(f);
  ret = kernel_read(f, buf, len, &f->f_pos);
  filp_close(f, NULL);
  return ret;
}

static ssize_t ask_server(const char *req, char *resp, size_t resp_max) {
  ssize_t ret;

  if (mutex_lock_interruptible(&fifo_mutex))
    return -ERESTARTSYS;

  ret = kwrite_file(FIFO_REQ, req, strlen(req));
  if (ret < 0) {
    printk(KERN_ERR "telegram: fifo write error: %zd\n", ret);
    mutex_unlock(&fifo_mutex);
    return ret;
  }

  ret = kread_file(FIFO_RESP, resp, resp_max - 1);
  if (ret < 0) {
    printk(KERN_ERR "telegram: fifo read error: %zd\n", ret);
    mutex_unlock(&fifo_mutex);
    return ret;
  }

  resp[ret] = '\0';
  mutex_unlock(&fifo_mutex);

  if (strncmp(resp, "ERR:", 4) == 0) {
    printk(KERN_ERR "telegram: server error: %s\n", resp + 4);
    return -EIO;
  }

  return ret;
}

static int tg_open(struct inode *inode, struct file *file) {
  int minor = iminor(inode);
  if (minor >= NUM_CHATS) {
    printk(KERN_ERR "telegram: no such chat %d\n", minor);
    return -ENODEV;
  }
  file->private_data = (void *)(long)minor;
  printk(KERN_INFO "telegram: chat %d opened\n", minor);
  return 0;
}

static int tg_release(struct inode *inode, struct file *file) {
  printk(KERN_INFO "telegram: chat %d closed\n", (int)(long)file->private_data);
  return 0;
}

static ssize_t tg_read(struct file *file, char __user *ubuf, size_t ulen,
                       loff_t *offset) {
  int minor = (int)(long)file->private_data;
  char request[32];
  char *response;
  ssize_t resp_len, to_copy;

  if (*offset > 0)
    return 0;

  response = kmalloc(MAX_RESP_LEN, GFP_KERNEL);
  if (!response)
    return -ENOMEM;

  snprintf(request, sizeof(request), "READ:%d\n", minor);

  resp_len = ask_server(request, response, MAX_RESP_LEN);
  if (resp_len < 0) {
    kfree(response);
    return resp_len;
  }

  to_copy = min((size_t)resp_len, ulen);
  if (copy_to_user(ubuf, response, to_copy)) {
    kfree(response);
    return -EFAULT;
  }

  *offset += to_copy;
  kfree(response);
  return to_copy;
}

static ssize_t tg_write(struct file *file, const char __user *ubuf, size_t ulen,
                        loff_t *offset) {
  int minor = (int)(long)file->private_data;
  char *msg, *request, response[64];
  ssize_t ret;
  size_t copy_len;
  uid_t uid;
  char comm[TASK_COMM_LEN];

  if (ulen == 0)
    return 0;

  if (ulen > MAX_MSG_LEN) {
    printk(KERN_ERR "telegram: message too long (%zu)\n", ulen);
    return -EINVAL;
  }

  msg = kmalloc(ulen + 1, GFP_KERNEL);
  if (!msg)
    return -ENOMEM;

  if (copy_from_user(msg, ubuf, ulen)) {
    kfree(msg);
    return -EFAULT;
  }

  copy_len = ulen;
  if (copy_len > 0 && msg[copy_len - 1] == '\n')
    copy_len--;
  msg[copy_len] = '\0';

  if (copy_len == 0) {
    kfree(msg);
    return ulen;
  }

  uid = current_uid().val;
  get_task_comm(comm, current);

  request = kmalloc(MAX_MSG_LEN + 64, GFP_KERNEL);
  if (!request) {
    kfree(msg);
    return -ENOMEM;
  }

  // WRITE:<chat_id>:<uid>:<comm>:<msg>
  snprintf(request, MAX_MSG_LEN + 64, "WRITE:%d:%u:%s:%s\n", minor, uid, comm,
           msg);

  kfree(msg);

  printk(KERN_INFO "telegram: chat %d write from uid=%u comm=%s\n", minor, uid,
         comm);

  ret = ask_server(request, response, sizeof(response));
  kfree(request);

  if (ret < 0)
    return ret;

  return ulen;
}

static const struct file_operations tg_fops = {
    .owner = THIS_MODULE,
    .open = tg_open,
    .release = tg_release,
    .read = tg_read,
    .write = tg_write,
};

static int __init telegram_init(void) {
  dev_t dev;
  int i, ret;

  ret = alloc_chrdev_region(&dev, 0, NUM_CHATS, "telegram");
  if (ret < 0)
    return ret;
  major = MAJOR(dev);

  cdev_init(&tg_cdev, &tg_fops);
  ret = cdev_add(&tg_cdev, dev, NUM_CHATS);
  if (ret < 0) {
    unregister_chrdev_region(dev, NUM_CHATS);
    return ret;
  }

  tg_class = class_create("telegram");
  if (IS_ERR(tg_class)) {
    cdev_del(&tg_cdev);
    unregister_chrdev_region(dev, NUM_CHATS);
    return PTR_ERR(tg_class);
  }

  for (i = 0; i < NUM_CHATS; i++)
    device_create(tg_class, NULL, MKDEV(major, i), NULL, "telegram/chat_%d", i);

  printk(KERN_INFO "telegram: loaded, major=%d\n", major);
  return 0;
}

static void __exit telegram_exit(void) {
  int i;
  for (i = 0; i < NUM_CHATS; i++)
    device_destroy(tg_class, MKDEV(major, i));
  class_destroy(tg_class);
  cdev_del(&tg_cdev);
  unregister_chrdev_region(MKDEV(major, 0), NUM_CHATS);
  printk(KERN_INFO "telegram: unloaded\n");
}

module_init(telegram_init);
module_exit(telegram_exit);
