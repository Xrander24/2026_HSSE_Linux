obj-m := pft.o

KDIR ?= /home/xrander24/HSSE/2026_linux/root/lib/modules/6.19.0/build
PWD  := $(shell pwd)
ROOTFS ?= $(PWD)/../root

USERAPPS   := pft-ctl pft-bench
USERCFLAGS := -Wall -Wextra -O2 -static

all: module $(USERAPPS)

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

pft-ctl: pft-ctl.c
	$(CC) $(USERCFLAGS) -o $@ $<

pft-bench: pft-bench.c
	$(CC) $(USERCFLAGS) -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(USERAPPS)

# Drop the .ko and the userspace tools into the busybox rootfs,
# ready for the next initramfs rebuild.
install: all
	cp pft.ko    $(ROOTFS)/
	cp pft-ctl   $(ROOTFS)/
	cp pft-bench $(ROOTFS)/
	cp demo.sh   $(ROOTFS)/
	chmod +x     $(ROOTFS)/demo.sh

.PHONY: all module clean install
