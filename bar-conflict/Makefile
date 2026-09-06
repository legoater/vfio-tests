obj-m := bar-conflict.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
ifeq ($(wildcard $(KDIR)),)
	@echo "Skipping bar-conflict: $(KDIR) not found"
else
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
endif

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
