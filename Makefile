KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m += etherip6.o

.PHONY: all module tools clean

all: module tools

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

tools: etherip6ctl

etherip6ctl: etherip6ctl.c etherip6_uapi.h
	$(CC) $(CFLAGS) -Wall -Wextra -O2 -o $@ etherip6ctl.c

clean:
	@if [ -d "$(KDIR)" ]; then \
		$(MAKE) -C "$(KDIR)" M="$(PWD)" clean; \
	else \
		$(RM) -r .tmp_versions; \
		$(RM) *.o .*.o *.ko *.mod *.mod.c Module.symvers modules.order .*.cmd; \
	fi
	$(RM) etherip6ctl
