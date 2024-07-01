NASM		= nasm
NFLAGS		= -f bin -g

MINGW		= x86_64-w64-mingw32-gcc
MFLAGS		= -m64 -ffreestanding -Wall -Werror
LFLAGS		= -Wall -Werror -m64 -nostdlib -shared -Wl,-dll -Wl,--subsystem,10 -e efi_main

GCC			= x86_64-elf-gcc
CCFLAGS		= -c -m64 -ffreestanding -nostdlib -I./src/kernel
LD			= x86_64-elf-ld
LDFLAGS		= -m elf_x86_64 -T src/kernel/linker.ld -nostdlib

# pmemsave XXXX - YYYY mem.dmp 	-	dump of phys memory
QEMU_UEFI	= qemu-system-x86_64 -monitor stdio -m 64M -bios uefi64.bin -cpu qemu64 -no-reboot -no-shutdown # -d int,cpu_reset
QEMU_BIOS	= qemu-system-x86_64 -monitor stdio -m 128M -cpu qemu64 # -no-reboot -no-shutdown
DISK_IMG	= surfaceos.img

SOURCES		=  	bin/kernel/kernel.o \
				bin/kernel/drivers/screen.o \
				bin/kernel/stdlib/stdio.o \
				bin/kernel/stdlib/string.o \
				bin/kernel/cpu/gdt.o \
				bin/kernel/cpu/gdt_asm.o \
				bin/kernel/cpu/memory.o \
				bin/kernel/cpu/paging.o \


# Bootloader
bin/boot/bios/%.bin: src/boot/bios/%.asm
	$(NASM) $(NFLAGS) -o $@ $<

bin/boot/efi/%.o: src/boot/efi/%.c
	$(MINGW) $(MFLAGS) -c $< -o $@

bin/boot/efi/BOOTX64.EFI: bin/boot/efi/main_efi.o
	$(MINGW) $^ $(LFLAGS) -o $@


# Data files
bin/kernel/data/stdfont.fnt: src/kernel/data/stdfont.asm
	$(NASM) $(NFLAGS) -o $@ $<


# Other files
bin/kernel/drivers/%.o: src/kernel/drivers/%.c
	$(GCC) $(CCFLAGS) -o $@ $^

bin/kernel/stdlib/%.o: src/kernel/stdlib/%.c
	$(GCC) $(CCFLAGS) -o $@ $^

bin/kernel/cpu/%.o: src/kernel/cpu/%.c
	$(GCC) $(CCFLAGS) -o $@ $^

bin/kernel/cpu/gdt_asm.o: src/kernel/cpu/gdt.asm
	$(NASM) -f elf64 -g -o $@ $<


# Kernel
bin/kernel/kentry.o: src/kernel/kentry.asm
	$(NASM) -f elf64 -g -o $@ $<

bin/kernel/kernel.o: src/kernel/kernel.c
	$(GCC) $(CCFLAGS) -o $@ $^

bin/kernel/kernel.bin: bin/kernel/kentry.o $(SOURCES)
	$(LD) $(LDFLAGS) -o $@ $^


# Disk and test
$(DISK_IMG): create_disk bin/boot/efi/BOOTX64.EFI bin/boot/bios/stub.bin bin/kernel/kernel.bin bin/kernel/data/stdfont.fnt
	mkfs.fat -F32 $(DISK_IMG)

	cp bin/boot/efi/BOOTX64.EFI ./disk/EFI/Boot

	dd if=bin/boot/bios/stub.bin of=$(DISK_IMG) conv=notrunc,fsync
	dd if=bin/boot/bios/stub.bin of=$(DISK_IMG) conv=notrunc,fsync bs=512 seek=6		# copy of bootloader
#	dd if=bin/boot/bios/stage2.bin of=$(DISK_IMG) conv=notrunc,fsync bs=512 seek=16		# if you want support BIOS

	sudo mount $(DISK_IMG) ./tmp
	sudo cp -R ./disk/EFI ./tmp
	sudo cp ./bin/kernel/kernel.bin ./tmp/KERNEL.BIN
	sudo cp ./bin/kernel/data/stdfont.fnt ./tmp/FONT.FNT
	sleep 0.3
	sudo umount ./tmp

run: $(DISK_IMG)
	$(QEMU_UEFI) -hda $(DISK_IMG)
#	$(QEMU_BIOS) -hda $(DISK_IMG)


create_disk:
	@rm -rf $(DISK_IMG)
	dd if=/dev/zero of=$(DISK_IMG) bs=1M count=60

clean:
	@rm -rf bin/boot/bios/*.bin
	@rm -rf bin/boot/efi/*.o
	@rm -rf bin/boot/efi/*.EFI
	@rm -rf disk/EFI/Boot/*.EFI
	@rm -rf bin/kernel/*.o bin/kernel/*.bin