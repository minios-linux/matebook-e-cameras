ifneq ($(KERNELRELEASE),)
obj-m += hi846.o s5k3l6-matebook.o ipu-bridge.o dw9714.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
