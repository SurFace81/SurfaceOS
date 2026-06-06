# Features

- UEFI bootloader (custom EFI loader)
- x86_64 kernel
- GDT and IDT setup
- CPU exception handling
- Hardware interrupts (using PIC 8259)
- PS/2 keyboard driver
- Framebuffer text console (UEFI GOP)
- Serial output (using UART 16550, ports/PCIe)
- Paging with 2 MiB pages
- Buddy-based memory allocator
- Basic shell with built-in commands
- PCI bus enumeration
- xHCI USB controller support
- Basic USB device listing

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