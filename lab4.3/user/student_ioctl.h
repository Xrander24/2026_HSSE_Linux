#ifndef STUDENT_IOCTL_H
#define STUDENT_IOCTL_H

#include <linux/ioctl.h>

#define STUDENT_IOCTL_MAGIC 's'
#define IOCTL_HELLO _IOWR(STUDENT_IOCTL_MAGIC, 1, int)

#endif