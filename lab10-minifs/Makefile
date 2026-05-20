obj-m += minifs.o

KDIR ?= /home/xrander24/HSSE/2026_linux/root/lib/modules/6.19.0/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean