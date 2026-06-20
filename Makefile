NASM		= nasm
NFLAGS		= -f bin -g

MINGW		= x86_64-w64-mingw32-gcc
MFLAGS		= -m64 -ffreestanding -Wall -Werror
LFLAGS		= -Wall -Werror -m64 -nostdlib -shared -Wl,-dll -Wl,--subsystem,10 -e efi_main

GCC			= x86_64-elf-gcc
GPP			= x86_64-elf-g++
CCFLAGS		= -c -m64 -g -ffreestanding -fno-exceptions -fno-rtti -nostdlib -I./src/kernel
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

SOURCES		=  	bin/kernel/kernel.o \
				bin/kernel/cpu/gdt.o \
				bin/kernel/cpu/gdt.asm.o \
				bin/kernel/mm/memory.o \
				bin/kernel/mm/heap.o \
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
				bin/kernel/drivers/commands.o \
				bin/kernel/drivers/pit.o \
				bin/kernel/drivers/rtc.o \
				bin/kernel/cpu/syscall.o \
				bin/kernel/cpu/program.o \

# SDK
SDK_FLAGS   = -c -m64 -ffreestanding -fno-exceptions -fno-rtti -nostdlib -Isrc/sdk/include
SDK_OBJ     = bin/sdk/entry.o bin/sdk/syscall.o bin/sdk/stdio.o bin/sdk/stdlib.o bin/sdk/string.o

# Apps: every .cpp in src/apps/ becomes a .bin
APP_SRC		= $(wildcard src/apps/*.cpp)
APP_BINS	= $(patsubst src/apps/%.cpp, bin/apps/%.bin, $(APP_SRC))

.PHONY: run clean create_disk version

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


# SDK (libc)
bin/sdk/%.o: src/sdk/libc/%.cpp
	mkdir -p $(dir $@)
	$(GPP) $(SDK_FLAGS) -o $@ $<


# Apps: compile app source + link with libc into flat binary
bin/apps/%.bin: src/apps/%.cpp $(SDK_OBJ)
	mkdir -p $(dir $@)
	$(GPP) $(SDK_FLAGS) -c -o bin/apps/$*.o $<
	$(LD) -m elf_x86_64 -T src/sdk/linker.ld -nostdlib -o $@ \
		bin/sdk/entry.o bin/sdk/syscall.o bin/sdk/stdio.o \
		bin/sdk/stdlib.o bin/sdk/string.o bin/apps/$*.o


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


# Disk and test
$(DISK_IMG): create_disk bin/boot/efi/BOOTX64.EFI bin/boot/bios/stub.bin bin/kernel/kernel.bin bin/kernel/data/stdfont.fnt $(APP_BINS)
	mkfs.fat -F32 $(DISK_IMG)

	mkdir -p ./disk/EFI/Boot
	cp bin/boot/efi/BOOTX64.EFI ./disk/EFI/Boot

	dd if=bin/boot/bios/stub.bin of=$(DISK_IMG) conv=notrunc,fsync
	dd if=bin/boot/bios/stub.bin of=$(DISK_IMG) conv=notrunc,fsync bs=512 seek=6		# copy of bootloader
#	dd if=bin/boot/bios/stage2.bin of=$(DISK_IMG) conv=notrunc,fsync bs=512 seek=16		# if you want support BIOS

	sudo mount $(DISK_IMG) ./tmp
	sudo cp -R ./disk/EFI ./tmp
	sudo cp ./bin/kernel/kernel.bin ./tmp/KERNEL.BIN
	sudo cp ./bin/kernel/data/stdfont.fnt ./tmp/FONT.FNT
	sudo sh -c 'echo "Hello from file!" > ./tmp/FILE.TXT'
	sudo mkdir -p ./tmp/APPS
	@for f in $(APP_BINS); do sudo cp $$f ./tmp/APPS/$$(basename $$f | tr 'a-z' 'A-Z'); done
	sleep 0.6
	sudo umount ./tmp

run: $(DISK_IMG)
#	$(QEMU_UEFI) -hda $(DISK_IMG)
	$(QEMU_UEFI) -drive id=usbstick,if=none,format=raw,file=$(DISK_IMG)
#	$(QEMU_BIOS) -hda $(DISK_IMG)


create_disk:
	@rm -rf $(DISK_IMG)
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=8

clean:
	@rm -rf bin/boot/bios/*.bin
	@rm -rf bin/boot/efi/*.o
	@rm -rf bin/boot/efi/*.EFI
	@rm -rf disk/EFI/Boot/*.EFI
	@rm -rf bin/kernel/*.o bin/kernel/*.bin
	@rm -rf bin/kernel/data/*.fnt
	@rm -rf bin/kernel/cpu/*.o bin/kernel/drivers/*.o bin/kernel/stdlib/*.o
	@rm -rf bin/sdk/*.o
	@rm -rf bin/apps/*
	@rm -f src/kernel/version.h