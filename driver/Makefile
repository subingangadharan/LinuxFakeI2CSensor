obj-m := fakesensor.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all: module dtbo

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

dtbo: fakesensor.dtbo fakesensor-opi5.dtbo

fakesensor.dtbo: fakesensor-overlay.dts
	dtc -@ -I dts -O dtb -o $@ $<

fakesensor-opi5.dtbo: fakesensor-opi5-overlay.dts
	dtc -@ -I dts -O dtb -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f fakesensor.dtbo
