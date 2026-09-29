NASM		= nasm
NFLAGS		= -f bin -g

MINGW		= x86_64-w64-mingw32-gcc
MFLAGS		= -m64 -ffreestanding -Wall -Werror
LFLAGS		= -Wall -Werror -m64 -nostdlib -shared -Wl,-dll -Wl,--subsystem,10 -e efi_main

GCC			= x86_64-elf-gcc
GPP			= x86_64-elf-g++
CCFLAGS		= -c -m64 -g -ffreestanding -fno-exceptions -fno-rtti -nostdlib \
			  -fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone \
			  -mgeneral-regs-only -mcmodel=kernel -fno-pic \
			  -I./src/kernel
LD			= x86_64-elf-ld
OBJCOPY		= x86_64-elf-objcopy
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
DISK_IMG	= surfaceos.img

# `make` alone builds the image.
.DEFAULT_GOAL := $(DISK_IMG)
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
				bin/kernel/drivers/kbd.o \
				bin/kernel/drivers/reports.o \
				bin/kernel/cpu/cpuid.o \
				bin/kernel/cpu/pci.o \
				bin/kernel/drivers/usb/xhci.o \
				bin/kernel/drivers/usb/usb.o \
				bin/kernel/drivers/usb/msc.o \
				bin/kernel/drivers/usb/hid_kbd.o \
				bin/kernel/drivers/usb/hub.o \
				bin/kernel/dev/blkdev.o \
				bin/kernel/dev/part.o \
				bin/kernel/dev/bcache.o \
				bin/kernel/fs/vfs.o \
				bin/kernel/fs/file.o \
				bin/kernel/fs/mounts.o \
				bin/kernel/fs/fat32/fat.o \
				bin/kernel/fs/fat32/dir.o \
				bin/kernel/fs/fat32/vnode.o \
				bin/kernel/drivers/pit.o \
				bin/kernel/drivers/tty.o \
				bin/kernel/drivers/term.o \
				bin/kernel/drivers/rtc.o \
				bin/kernel/cpu/sfcall.o \
				bin/kernel/cpu/sdkpage.o \
				bin/kernel/cpu/sdkpage.asm.o \
				bin/kernel/cpu/sffile.o \
				bin/kernel/cpu/sftime.o \
				bin/kernel/cpu/sfconsole.o \
				bin/kernel/cpu/sfadmin.o \
				bin/kernel/cpu/sfsync.o \
				bin/kernel/cpu/apic.o \
				bin/kernel/fs/fileio.o \
				bin/kernel/cpu/percpu.o \
				bin/kernel/cpu/spinlock.o \
				bin/kernel/cpu/smp.o \
				bin/kernel/cpu/smp.asm.o \
				bin/kernel/cpu/process.o \
				bin/kernel/cpu/task.o \
				bin/kernel/cpu/task.asm.o \
				bin/kernel/cpu/elf.o \
				bin/kernel/cpu/features.o \
				bin/kernel/cpu/uaccess.o \
				bin/kernel/acpi/acpi.o \
				bin/kernel/obj/object.o \
				bin/kernel/obj/event.o \

# SDK: crt0.o is always linked first (contains _start, must be at PROGRAM_BASE)
# everything else goes into a static library so link order doesn't matter
SDK_FLAGS   = -c -m64 -ffreestanding -fno-exceptions -fno-rtti -nostdlib \
			  -fno-asynchronous-unwind-tables -Isrc/sdk/include

# Header dependencies: every compile also writes a .d next to its object
# (-MMD), with a phony target per header (-MP) so a deleted header does not
# break the build. Without them a changed struct in a header left stale
# objects behind that only failed at link time - or not at all.
DEPFLAGS    = -MMD -MP
# Programs, all built against <sfos.h> (no libc, no start-up code: the
# kernel starts them in the SDK runtime, which calls SfMain):
#   src/apps/<name>.cpp   a program of one file        -> bin/apps/<name>.bin
#   src/apps/<name>/      one program of all its .cpp  -> bin/apps/<name>.bin
APP_ONE_SRC	= $(wildcard src/apps/*.cpp)
APP_DIR_SRC	= $(wildcard src/apps/*/*.cpp)
APP_DIRS	= $(sort $(patsubst src/apps/%/,%,$(dir $(APP_DIR_SRC))))
APP_BINS	= $(patsubst src/apps/%.cpp,bin/apps/%.bin,$(APP_ONE_SRC)) \
		  $(patsubst %,bin/apps/%.bin,$(APP_DIRS))
APP_OBJS	= $(patsubst src/apps/%.cpp,bin/apps/%.o,$(APP_ONE_SRC) $(APP_DIR_SRC))
APP_LDFLAGS	= -m elf_x86_64 -z max-page-size=0x1000 -T src/sdk/sfos.ld -nostdlib

.PHONY: run clean version usb

# Bootloader

bin/boot/efi/%.o: src/boot/efi/%.c
	mkdir -p $(dir $@)
	$(MINGW) $(MFLAGS) $(DEPFLAGS) -c $< -o $@

bin/boot/efi/BOOTX64.EFI: bin/boot/efi/main_efi.o
	$(MINGW) $^ $(LFLAGS) -o $@


# Data files
bin/kernel/data/stdfont.fnt: src/kernel/data/stdfont.asm
	mkdir -p $(dir $@)
	$(NASM) $(NFLAGS) -o $@ $<


# Kernel object files
bin/kernel/drivers/%.o: src/kernel/drivers/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/drivers/usb/%.o: src/kernel/drivers/usb/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/drivers/fs/%.o: src/kernel/drivers/fs/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/dev/%.o: src/kernel/dev/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/fs/%.o: src/kernel/fs/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/fs/fat32/%.o: src/kernel/fs/fat32/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/stdlib/%.o: src/kernel/stdlib/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/cpu/%.o: src/kernel/cpu/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/mm/%.o: src/kernel/mm/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/acpi/%.o: src/kernel/acpi/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/obj/%.o: src/kernel/obj/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/cpu/%.asm.o: src/kernel/cpu/%.asm
	mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<


# SDK runtime: the code behind the SDK tables, linked at the SDK's address
# (abi/sdkimage.h) and built into the kernel (sdkpage.asm).
RUNTIME_SRC   = $(wildcard src/sdk/runtime/*.cpp)
RUNTIME_OBJS  = $(patsubst src/sdk/runtime/%.cpp,bin/sdk/runtime/%.o,$(RUNTIME_SRC))
RUNTIME_FLAGS = $(SDK_FLAGS) -mcmodel=large -fno-pic -fno-stack-protector

bin/sdk/runtime/%.o: src/sdk/runtime/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(RUNTIME_FLAGS) $(DEPFLAGS) -o $@ $<

bin/sdk/runtime.elf: $(RUNTIME_OBJS) src/sdk/runtime/runtime.ld
	$(LD) -m elf_x86_64 -nostdlib -T src/sdk/runtime/runtime.ld -o $@ $(RUNTIME_OBJS)

bin/sdk/runtime.bin: bin/sdk/runtime.elf
	$(OBJCOPY) -O binary -j .text $< $@

bin/kernel/cpu/sdkpage.asm.o: bin/sdk/runtime.bin

# The other CPUs' first code: a flat binary for its low page, built into
# the kernel (smp.asm).
bin/kernel/cpu/ap_trampoline.bin: src/kernel/cpu/ap_trampoline.asm
	mkdir -p $(dir $@)
	$(NASM) -f bin -o $@ $<

bin/kernel/cpu/smp.asm.o: bin/kernel/cpu/ap_trampoline.bin


# Programs. -z max-page-size=0x1000 keeps the ELF compact: the x86_64-elf
# default is a 2 MB segment alignment, which pads a 10 KB program to ~1 MB
# of zeros on disk.
bin/apps/%.o: src/apps/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(SDK_FLAGS) $(DEPFLAGS) -o $@ $<

bin/apps/%.bin: bin/apps/%.o src/sdk/sfos.ld
	$(LD) $(APP_LDFLAGS) -o $@ $<

define APP_DIR_RULE
bin/apps/$(1).bin: $$(patsubst src/apps/%.cpp,bin/apps/%.o,$$(wildcard src/apps/$(1)/*.cpp)) src/sdk/sfos.ld
	$$(LD) $$(APP_LDFLAGS) -o $$@ $$(filter %.o,$$^)
endef
$(foreach d,$(APP_DIRS),$(eval $(call APP_DIR_RULE,$(d))))

# The console of screens 2..9, /sfos/CMD.BIN: built like a program, kept
# out of /apps.
CMD_BIN = bin/sfos/cmd.bin

bin/sfos/cmd.o: src/sfos/cmd.cpp
	mkdir -p $(dir $@)
	$(GPP) $(SDK_FLAGS) $(DEPFLAGS) -o $@ $<

$(CMD_BIN): bin/sfos/cmd.o src/sdk/sfos.ld
	$(LD) $(APP_LDFLAGS) -o $@ $<

# Keep the objects: they are intermediate files of the pattern rule, which
# make would otherwise delete and rebuild every time.
.SECONDARY: $(APP_OBJS) bin/sfos/cmd.o


# Generating version
version:
	@bash ./version.sh


# Kernel
bin/kernel/kentry.o: src/kernel/kentry.asm
	mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<

bin/kernel/kernel.o: src/kernel/kernel.cpp version
	mkdir -p $(dir $@)
	$(GPP) $(CCFLAGS) $(DEPFLAGS) -o $@ $<

bin/kernel/kernel.bin: bin/kernel/kentry.o $(SOURCES)
	mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^
	$(LD) $(LDFLAGS) --oformat elf64-x86-64 -o bin/kernel/kernel.elf $^


# Disk image: GPT with one FAT32 EFI System Partition, built by
# tools/mkimg.py (pyfatfs, no sudo). IMG_SIZE_MIB=64. Apps land in
# /apps/<name> (LFN).
$(DISK_IMG): bin/boot/efi/BOOTX64.EFI bin/kernel/kernel.bin bin/kernel/data/stdfont.fnt $(CMD_BIN) $(APP_BINS)
	python3 tools/mkimg.py $(DISK_IMG) \
		bin/boot/efi/BOOTX64.EFI \
		bin/kernel/kernel.bin \
		bin/kernel/data/stdfont.fnt \
		$(CMD_BIN) \
		$(APP_BINS) \
		--size=$(IMG_SIZE_MIB)

run: $(DISK_IMG)
	$(QEMU_UEFI) -drive id=usbstick,if=none,format=raw,file=$(DISK_IMG)

# Write the image to a real stick: make usb DEV=/dev/sdX
usb: $(DISK_IMG)
	@test -n "$(DEV)" || { echo "usage: make usb DEV=/dev/sdX"; false; }
	@ls -l $(DEV)
	@echo "WARNING: $(DEV) will be overwritten. Ctrl-C now to abort."; sleep 3
	sudo dd if=$(DISK_IMG) of=$(DEV) bs=4M conv=fsync status=progress
	@echo "Done. The stick can be removed."

clean:
	@rm -rf bin/boot/efi/*.o
	@rm -rf bin/boot/efi/*.EFI
	@rm -rf bin/kernel/*.o bin/kernel/*.bin
	@rm -rf bin/kernel/data/*.fnt
	@rm -rf bin/kernel/cpu/*.o bin/kernel/drivers/*.o bin/kernel/stdlib/*.o
	@rm -rf bin/kernel/mm/*.o bin/kernel/drivers/usb/*.o bin/kernel/drivers/fs/*.o bin/kernel/dev/*.o bin/kernel/fs/*.o bin/kernel/fs/fat32/*.o
	@rm -rf bin/kernel/acpi/*.o bin/kernel/obj/*.o
	@find bin -name '*.d' -delete 2>/dev/null || true
	@rm -rf bin/sdk/runtime bin/sdk/runtime.elf bin/sdk/runtime.bin
	@rm -rf bin/apps/*
	@rm -f src/kernel/version.h

# Header dependencies written by -MMD (see DEPFLAGS).
-include $(patsubst %.o,%.d,$(filter %.o,$(SOURCES))) bin/kernel/kernel.d \
         $(RUNTIME_OBJS:.o=.d) $(APP_OBJS:.o=.d) bin/boot/efi/main_efi.d
