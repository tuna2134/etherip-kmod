KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m += etherip6.o

.PHONY: all module tools clean

all: module tools

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C "$(KDIR)" M="$(PWD)" clean; \
	else \
		$(RM) -r .tmp_versions; \
		$(RM) *.o .*.o *.ko *.mod *.mod.c Module.symvers modules.order .*.cmd; \
	fi
	$(RM) etherip6ctl
