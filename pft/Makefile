obj-m := pft.o

KDIR ?= /home/xrander24/HSSE/2026_linux/root/lib/modules/6.19.0/build
PWD  := $(shell pwd)
ROOTFS ?= $(PWD)/../root

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Convenience: drop the freshly built .ko into the busybox rootfs
# so the next initramfs rebuild picks it up.
install: all
	cp pft.ko $(ROOTFS)/

.PHONY: all clean install
