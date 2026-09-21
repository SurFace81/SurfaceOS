NASM		= nasm
NFLAGS		= -f bin -g

MINGW		= x86_64-w64-mingw32-gcc
MFLAGS		= -m64 -ffreestanding -Wall -Werror
LFLAGS		= -Wall -Werror -m64 -nostdlib -shared -Wl,-dll -Wl,--subsystem,10 -e efi_main

GCC			= x86_64-elf-gcc
GPP			= x86_64-elf-g++
AR			= x86_64-elf-ar
CCFLAGS		= -c -m64 -g -ffreestanding -fno-exceptions -fno-rtti -nostdlib \
			  -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone \
			  -mgeneral-regs-only \
			  -I./src/kernel
LD			= x86_64-elf-ld
LDFLAGS		= -m elf_x86_64 -T src/kernel/linker.ld -nostdlib

# pmemsave XXXX - YYYY mem.dmp 	-	dump of phys memory
# -d int,cpu_reset
QEMU_UEFI	= 	qemu-system-x86_64 \
				-monitor stdio \
				-chardev file,id=uart0,path=uart.log \
				-trace usb_xhci* -D xhci.log \
				-m 128M \
				-bios uefi64.bin \
				-cpu qemu64 \
				-device qemu-xhci \
				-device usb-storage,drive=usbstick \
				-device pci-serial,chardev=uart0 \
				-no-reboot -no-shutdown
QEMU_BIOS	= qemu-system-x86_64 -monitor stdio -serial file:uart.log -m 64M -cpu qemu64 # -no-reboot -no-shutdown
DISK_IMG	= surfaceos.img
# Disk layout: superfloppy (legacy, BIOS stub), mbr or gpt (default - how a
# real USB stick looks; UEFI boot). Passed to tools/mkimg.py.
LAYOUT		?= gpt
IMG_SIZE_MIB	?= 64

SOURCES		=  	bin/kernel/kernel.o \
				bin/kernel/cpu/gdt.o \
				bin/kernel/cpu/gdt.asm.o \
				bin/kernel/mm/memory.o \
				bin/kernel/mm/heap.o \
				bin/kernel/mm/pmm.o \
				bin/kernel/cpu/paging.o \
				bin/kernel/cpu/ports.o \
				bin/kernel/drivers/uart.o \
				bin/kernel/drivers/screen.o \
				bin/kernel/stdlib/stdio.o \
				bin/kernel/stdlib/string.o \
				bin/kernel/cpu/idt.o \
				bin/kernel/cpu/irq.o \
				bin/kernel/cpu/interrupts.asm.o \
				bin/kernel/drivers/keyboard.o \
				bin/kernel/drivers/console.o \
				bin/kernel/cpu/cpuid.o \
				bin/kernel/cpu/pci.o \
				bin/kernel/drivers/usb/xhci.o \
				bin/kernel/drivers/fs/fat32.o \
				bin/kernel/dev/blkdev.o \
				bin/kernel/dev/part.o \
				bin/kernel/dev/bcache.o \
				bin/kernel/fs/vfs.o \
				bin/kernel/fs/file.o \
				bin/kernel/fs/devfs.o \
				bin/kernel/fs/fat32/fat.o \
				bin/kernel/fs/fat32/dir.o \
				bin/kernel/fs/fat32/vnode.o \
				bin/kernel/drivers/commands.o \
				bin/kernel/drivers/pit.o \
				bin/kernel/drivers/tty.o \
				bin/kernel/drivers/rtc.o \
				bin/kernel/cpu/syscall.o \
				bin/kernel/cpu/tss.o \
				bin/kernel/cpu/process.o \
				bin/kernel/cpu/process.asm.o \
				bin/kernel/cpu/elf.o \
				bin/kernel/cpu/features.o \
				bin/kernel/cpu/uaccess.o \

# SDK: crt0.o is always linked first (contains _start, must be at PROGRAM_BASE)
# everything else goes into a static library so link order doesn't matter
SDK_FLAGS   = -c -m64 -mcmodel=large -ffreestanding -fno-exceptions -fno-rtti -nostdlib \
			  -fno-asynchronous-unwind-tables -Isrc/sdk/include
SDK_SRC     = $(wildcard src/sdk/libc/*.cpp)
SDK_ALL_OBJ = $(patsubst src/sdk/libc/%.cpp, bin/sdk/%.o, $(SDK_SRC))
SDK_ENTRY   = bin/sdk/crt0.o
SDK_LIB_OBJ = $(filter-out $(SDK_ENTRY), $(SDK_ALL_OBJ))
SDK_LIB     = bin/sdk/libsfos.a

# crt0.S: the _start stub (SysV initial stack -> __libc_start)
bin/sdk/crt0.o: src/sdk/libc/crt0.S
	mkdir -p $(dir $@)
	x86_64-elf-gcc -c -m64 -ffreestanding -nostdlib -o $@ $<

# Apps: every .cpp in src/apps/ becomes a .bin
APP_SRC		= $(wildcard src/apps/*.cpp)
APP_BINS	= $(patsubst src/apps/%.cpp, bin/apps/%.bin, $(APP_SRC))

.PHONY: run clean create_disk version usb

# Bootloader
bin/boot/bios/%.bin: src/boot/bios/%.asm
	mkdir -p $(dir $@)
	$(NASM) $(NFLAGS) -o $@ $<

bin/boot/efi/%.o: src/boot/efi/%.c
	mkdir -p $(dir $@)
	$(MINGW) $(MFLAGS) -c $< -o $@

bin/boot/efi/BOOTX64.EFI: bin/boot/efi/main_efi.o
	$(MINGW) $^ $(LFLAGS) -o $@


# Data files
bin/kernel/data/stdfont.fnt: src/kernel/data/stdfont.asm
	mkdir -p $(dir $@)
	$(NASM) $(NFLAGS) -o $@ $<


# Kernel object files
bin/kernel/drivers/%.o: src/kernel/drivers/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) -o $@ $^

bin/kernel/drivers/usb/%.o: src/kernel/drivers/usb/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) -o $@ $^

bin/kernel/drivers/fs/%.o: src/kernel/drivers/fs/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) -o $@ $^

bin/kernel/dev/%.o: src/kernel/dev/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) -o $@ $^

bin/kernel/fs/%.o: src/kernel/fs/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) -o $@ $^

bin/kernel/fs/fat32/%.o: src/kernel/fs/fat32/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) -o $@ $^

bin/kernel/stdlib/%.o: src/kernel/stdlib/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) -o $@ $^

bin/kernel/cpu/%.o: src/kernel/cpu/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) -o $@ $^

bin/kernel/mm/%.o: src/kernel/mm/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) -o $@ $^

bin/kernel/cpu/%.asm.o: src/kernel/cpu/%.asm
	mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<


# SDK objects
bin/sdk/%.o: src/sdk/libc/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(SDK_FLAGS) -o $@ $<

# SDK static library (everything except entry.o)
$(SDK_LIB): $(SDK_LIB_OBJ)
	$(AR) rcs $@ $^


# Apps: compile + link entry.o first, then app object, then pull the rest from libsfos.a
# -z max-page-size=0x1000: keep the ELF compact. The x86_64-elf default is a
# 2 MB segment alignment, which pads a 10 KB app to ~1 MB of zeros on disk.
# That made read_file pull hundreds of clusters over USB and froze the shell.
bin/apps/%.bin: src/apps/%.cpp src/sdk/linker.ld $(SDK_ENTRY) $(SDK_LIB)
	mkdir -p $(dir $@)
	$(GPP) $(SDK_FLAGS) -c -o bin/apps/$*.o $<
	$(LD) -m elf_x86_64 -z max-page-size=0x1000 -T src/sdk/linker.ld -nostdlib -o $@ \
		$(SDK_ENTRY) bin/apps/$*.o -Lbin/sdk -lsfos


# Generating version
version:
	@bash ./version.sh


# Kernel
bin/kernel/kentry.o: src/kernel/kentry.asm
	mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<

bin/kernel/kernel.o: src/kernel/kernel.cpp version
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) -o $@ $<

bin/kernel/kernel.bin: bin/kernel/kentry.o $(SOURCES)
	mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^


# Disk image: built by tools/mkimg.py (pyfatfs, no sudo). LAYOUT=gpt|mbr|
# superfloppy, IMG_SIZE_MIB=64. Apps land in /APPS (8.3) until stage 3.7
# moves them to /bin with LFN names.
$(DISK_IMG): bin/boot/efi/BOOTX64.EFI bin/boot/bios/stub.bin bin/kernel/kernel.bin bin/kernel/data/stdfont.fnt $(APP_BINS)
	python3 tools/mkimg.py $(DISK_IMG) \
		bin/boot/efi/BOOTX64.EFI \
		bin/kernel/kernel.bin \
		bin/kernel/data/stdfont.fnt \
		$(APP_BINS) \
		--layout=$(LAYOUT) --size=$(IMG_SIZE_MIB) \
		--bios-stub=bin/boot/bios/stub.bin

run: $(DISK_IMG)
	$(QEMU_UEFI) -drive id=usbstick,if=none,format=raw,file=$(DISK_IMG)

# Write the image to a real stick: make usb DEV=/dev/sdX [LAYOUT=gpt]
usb: $(DISK_IMG)
	@test -n "$(DEV)" || { echo "usage: make usb DEV=/dev/sdX"; false; }
	@ls -l $(DEV)
	@echo "WARNING: $(DEV) will be overwritten. Ctrl-C now to abort."; sleep 3
	sudo dd if=$(DISK_IMG) of=$(DEV) bs=4M conv=fsync status=progress
	@echo "Done. The stick can be removed."

create_disk:
	@rm -rf $(DISK_IMG)

clean:
	@rm -rf bin/boot/bios/*.bin
	@rm -rf bin/boot/efi/*.o
	@rm -rf bin/boot/efi/*.EFI
	@rm -rf disk/EFI/Boot/*.EFI
	@rm -rf bin/kernel/*.o bin/kernel/*.bin
	@rm -rf bin/kernel/data/*.fnt
	@rm -rf bin/kernel/cpu/*.o bin/kernel/drivers/*.o bin/kernel/stdlib/*.o
	@rm -rf bin/kernel/mm/*.o bin/kernel/drivers/usb/*.o bin/kernel/drivers/fs/*.o bin/kernel/dev/*.o bin/kernel/fs/*.o bin/kernel/fs/fat32/*.o
	@rm -rf bin/sdk/*.o bin/sdk/*.a
	@rm -rf bin/apps/*
	@rm -f src/kernel/version.h