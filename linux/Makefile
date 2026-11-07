# Out-of-tree build on the Raspberry Pi:
#   sudo apt install raspberrypi-kernel-headers   (or linux-headers-$(uname -r))
#   make && sudo insmod pmc-hwmon.ko
#   sudo dtoverlay pmc-overlay.dtbo     (or the echo below without DT)
#   echo pmc-hwmon 0x42 | sudo tee /sys/bus/i2c/devices/i2c-1/new_device
#   sensors
obj-m := pmc-hwmon.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
	dtc -@ -I dts -O dtb -o pmc-overlay.dtbo pmc-overlay.dts

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f pmc-overlay.dtbo
