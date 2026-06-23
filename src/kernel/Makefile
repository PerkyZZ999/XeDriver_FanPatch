obj-m := xe_fan_probe.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Kernel was built with clang on CachyOS, so we must use clang for modules too
# Use override to ensure CC/LD are set regardless of environment
override CC = clang
override LD = ld.lld

all:
	$(MAKE) -C $(KDIR) M=$(PWD) CC=$(CC) LD=$(LD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	sudo insmod xe_fan_probe.ko

uninstall:
	sudo rmmod xe_fan_probe

.PHONY: all clean install uninstall
