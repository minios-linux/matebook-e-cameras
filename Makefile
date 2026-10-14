# DKMS invokes this wrapper with KERNELRELEASE already set.
# The kernel module declarations live in drivers/Makefile.
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)/drivers" modules
clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)/drivers" clean
