# Features

- UEFI bootloader (custom EFI loader)
- x86_64 kernel
- GDT and IDT setup
- CPU exception handling
- Hardware interrupts (using PIC 8259)
- PS/2 keyboard driver
- Framebuffer text console (UEFI GOP)
- Serial output (using UART 16550)
- Paging with 2 MiB pages
- Buddy-based memory allocator
- Basic shell with built-in commands
- PCI bus enumeration
- xHCI USB controller support
- Basic USB device listing

# Build & Run

1. Install Linux (Ubuntu)
2. Install dependencies:

    `sudo apt install qemu-system-x86 nasm gparted okteta make libc6:i386 libncurses5:i386 libstdc++6:i386`

3. Install Cross Compiler: https://wiki.osdev.org/GCC_Cross-Compiler
4. Open SurfaceOS folder: 
    - Create folder `./tmp`
    - Run in terminal: `make run`