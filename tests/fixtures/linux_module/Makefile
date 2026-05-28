obj-m += kobox_probe.o
obj-m += kobox_empty.o
obj-m += kobox_driverish.o
obj-m += kobox_pci_minimal.o

CFLAGS_REMOVE_kobox_empty.o += -pg -mfentry -mrecord-mcount
CFLAGS_kobox_driverish.o += -fno-stack-protector
CFLAGS_kobox_pci_minimal.o += -fno-stack-protector

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
