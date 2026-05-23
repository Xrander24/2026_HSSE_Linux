obj-m := pft.o

KDIR ?= /home/xrander24/HSSE/2026_linux/root/lib/modules/6.19.0/build
PWD  := $(shell pwd)
ROOTFS ?= $(PWD)/../root

USERAPP    := pft-ctl
USERCFLAGS := -Wall -Wextra -O2 -static

all: module $(USERAPP)

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

$(USERAPP): pft-ctl.c
	$(CC) $(USERCFLAGS) -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(USERAPP)

# Drop the .ko and the userspace tool into the busybox rootfs,
# ready for the next initramfs rebuild.
install: all
	cp pft.ko    $(ROOTFS)/
	cp $(USERAPP) $(ROOTFS)/

.PHONY: all module clean install
