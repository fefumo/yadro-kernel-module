KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m := dm-race.o

dm-race-y := \
	src/main.o \
	src/target.o \
	src/tracker.o

ccflags-y := -I$(src)/include

.PHONY: all clean reload unload load compile_commands clangd

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f compile_commands.json
	rm -f .kernel

load: all
	sudo insmod dm-race.ko

unload:
	-sudo rmmod dm_race

reload: unload load

clangd:
	ln -sfn $(KERNEL_DIR) .kernel

compile_commands: clangd
	compiledb make V=1
