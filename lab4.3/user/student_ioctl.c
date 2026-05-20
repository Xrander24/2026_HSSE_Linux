#include "student_ioctl.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void) {
  int fd = open("/dev/student_ioctl", O_RDWR);
  int value = 100;
  if (fd < 0) {
    perror("open");
    return 1;
  }
  if (ioctl(fd, IOCTL_HELLO, &value) < 0) {
    perror("ioctl");
    close(fd);
    return 1;
  }
  printf("result = %d\n", value);
  close(fd);
  return 0;
}