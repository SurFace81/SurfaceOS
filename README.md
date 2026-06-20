# SurfaceOS
 
A hobby x86_64 operating system written in C++ (freestanding, no OOP).
 
# Features
 
- UEFI bootloader (custom EFI loader via MinGW)
- x86_64 kernel
- GDT and IDT setup
- CPU exception handling
- Hardware interrupts (PIC 8259)
- PS/2 keyboard driver
- Framebuffer text console (UEFI GOP)
- Serial output (UART 16550 over PCIe)
- Paging with 2 MiB pages
- Physical memory manager
- Heap allocator
- PCI bus enumeration
- xHCI USB host controller driver
- USB mass storage (read/write)
- FAT32 filesystem (read/write, directories, mount/umount)
- RTC clock (read/write)
- PIT timer and uptime tracking
- CPUID (CPU name, topology, cache info, frequencies)
- Userspace libc (syscall interface, stdio, heap, string)
- Userspace app loader (flat binary via `exec`)
- Built-in shell with commands
 
# Project Structure
 
```
src/
  boot/       — UEFI bootloader (C, MinGW) and BIOS stub (NASM)
  kernel/     — kernel source (C++, freestanding)
    cpu/      — GDT, IDT, IRQ, paging, PCI, syscall, program loader
    drivers/  — screen, keyboard, console, UART, USB/xHCI, FAT32, PIT, RTC
    mm/       — physical memory, heap
    stdlib/   — stdio, string
  include/    — kernel-side headers
  sdk/        — userspace C library (syscall wrapper, stdio, heap, string)
  apps/       — userspace applications
```

# Build & Run

1. Install Linux (Ubuntu)
2. Enable i386 (if you have 64-bit system):

    `sudo dpkg --add-architecture i386`

    `sudo apt update`

3. Install dependencies:

    `sudo apt install qemu-system-x86 nasm gparted okteta make git libc6:i386 libncurses6:i386 libstdc++6:i386`

4. Install Cross Compiler (https://wiki.osdev.org/GCC_Cross-Compiler)
    - Extract opt.tar.xz in $HOME
    - Add to PATH: `export PATH="$HOME/opt/cross/bin:$PATH`
    - Reload bashrc: `source ~/.bashrc`

5. Open SurfaceOS folder: 
    - Create folder `./tmp`
    - Run in terminal: `make run`