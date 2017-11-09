KVERS = /home/sun/fsl-release-bsp/imx6slevk/tmp/work/imx6slevk-poky-linux-gnueabi/linux-imx/3.14.38-r0/build

# Kernel modules
obj-m += sun.o

# Specify flags for the module compilation.
EXTRA_CFLAGS=-g -O0

build: kernel_modules

kernel_modules:
	make -C $(KVERS) M=$(CURDIR) modules

clean:
	make -C $(KVERS) M=$(CURDIR) clean
