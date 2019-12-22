# Comment/uncomment the following line to disable/enable debugging
DEBUG = n

MKDIR= mkdir -p
CP= cp
RM= rm -f
MODULEDIR= /lib/modules/`uname -r`/misc/

# Add your debugging flag (or not) to CFLAGS
ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DSCULL_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif

EXTRA_CFLAGS += $(DEBFLAGS) -I..

ifneq ($(KERNELRELEASE),)
# call from kernel build system

obj-m	:= tty0tty.o 

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

endif


install: default rules
	${MKDIR} ${MODULEDIR}
	${CP} -v tty0tty.ko ${MODULEDIR}
	depmod
	modprobe tty0tty

rules:
	echo "tty0tty" > /etc/modules-load.d/tty0tty.conf
	${CP} 99-tty0tty.rules  /etc/udev/rules.d/
	service udev reload
	chown root:root /etc/udev/rules.d/99-tty0tty.rules


uninstall:
	$(RM) ${MODULEDIR}tty0tty.ko
	$(RM) /etc/modules-load.d/tty0tty.conf
	$(RM) /etc/udev/rules.d/99-tty0tty.rules


clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c *.mod .tmp_versions modules.order Module.symvers

depend .depend dep:
	$(CC) $(CFLAGS) -M *.c > .depend


ifeq (.depend,$(wildcard .depend))
include .depend
endif
