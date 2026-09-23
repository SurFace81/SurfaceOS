# SurfaceOS
 
A hobby x86_64 operating system written in C++ (freestanding, no OOP).
 
# Features
 
- UEFI bootloader (custom EFI loader via MinGW; passes the boot volume's
  partition start and disk signature to the kernel for automount)
- x86_64 kernel: GDT/IDT/PIC, paging (4 KiB, per-process address spaces,
  W^X), TSS, ring-3 user mode
- Preemptive scheduler (switches only at ring-3 boundaries), fork/execve/
  wait4/kill, SysV ABI process startup (argv/envp/auxv on the stack)
- Linux x86_64-compatible syscall ABI: same numbers, structures and
  -errno results (int 0x80 entry until stage 6)
- Block layer: blkdev registry (USB MSD today, AHCI/NVMe-shaped),
  MBR/GPT/superfloppy partition parsing, LRU sector cache with dirty
  tracking and flush, 512 and 4096-byte sectors
- VFS: vnode cache, mount table (FAT32 root + devfs), POSIX namei
  (`/`-separated paths, `.`, `..` across mount points, LFN)
- FAT32 with long file names (UTF-8, case preserved), FSInfo-based
  allocator, incremental read/write by byte offset, truncate, rename,
  unlink of open files, volume dirty bit
- Per-process fd table (POSIX dup/fork/exec semantics, O_CLOEXEC),
  open file descriptions with shared offsets
- devfs: /dev/null, /dev/zero, /dev/tty, /dev/console; canonical-mode
  terminal with echo; stdin/stdout/stderr are fds 0/1/2
- Userspace SDK heading towards musl: POSIX fd API (open/read/write/
  lseek/stat/getdents64/opendir...), environ/getenv, heap over brk
- Built-in shell: ls, cat, xxd, write, cp, mv, rm, mkdir, rmdir, cd, pwd,
  mount/umount, sync, lsblk, exec /bin/<name>, hardware info commands
- Test apps: memtest, proctest, argtest, fstest (fd layer + VFS + FAT32,
  122 checks incl. 3 MiB random-offset I/O and 100 LFN files)
 
# Project Structure
 
```
src/
  boot/       — UEFI bootloader (C, MinGW) and BIOS stub (NASM)
  kernel/     — kernel source (C++, freestanding)
    cpu/      — GDT, IDT, IRQ, paging, PCI, syscall dispatch, process,
                ELF loader, uaccess, sys_fs (fd syscalls)
    dev/      — blkdev registry, partition parsing, block cache
    drivers/  — screen, keyboard, console, commands, tty, UART, USB/xHCI,
                PIT, RTC
    fs/       — VFS core (vnode/mount/namei), file/fd layer, devfs,
                fat32/ (fat.cpp, dir.cpp, vnode.cpp)
    mm/       — physical memory, heap
    stdlib/   — stdio, string
  include/    — kernel-side headers (cpu/, dev/, fs/, drivers/, mm/)
  sdk/        — userspace C library: abi/ headers (Linux-compatible),
                libc/ (crt0.S, syscall, fd, dirent, stdio, stdlib, ...)
  apps/       — userspace applications (hi, hello, memtest, proctest,
                argtest, fstest)
tools/
  mkimg.py            — image builder: superfloppy|mbr|gpt, 512/4K sectors
  qemu_exec_test.sh   — full QEMU regression suite (LAYOUT=..., SECTOR=...)
  qemu_verify.sh      — reboot + persistence + host fsck.fat
  qemu_matrix.sh      — all layouts x all checks
```

# Build & Run

1. Install Linux (Ubuntu)
2. Enable i386 (if you have a 64-bit system):

    `sudo dpkg --add-architecture i386`

    `sudo apt update`

3. Install dependencies:

    `sudo apt install qemu-system-x86 nasm gparted okteta make git libc6:i386 libncurses6:i386 libstdc++6:i386 gcc-mingw-w64-x86-64 dosfstools sfdisk gdisk python3-pip ovmf`

    `pip3 install pyfatfs`

4. Install Cross Compiler (https://wiki.osdev.org/GCC_Cross-Compiler)
    - Extract opt.tar.xz in $HOME
    - Add to PATH permanently: `echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc`
    - Reload: `source ~/.bashrc`

5. Open SurfaceOS folder: 
    - Create folder `./tmp`
    - Run in terminal: `make run`  (GPT image by default; `make run LAYOUT=superfloppy`)

# Tests

- `bash tools/qemu_exec_test.sh` — boots QEMU, runs hi/memtest/proctest/
  argtest/fstest, asserts on the serial log. `LAYOUT=superfloppy|mbr|gpt`,
  `SECTOR=512|4096` (4096 implies a 512 MiB superfloppy image).
- `bash tools/qemu_verify.sh` — reboots the fstest image and checks
  persistence + host `fsck.fat -n`.
- `bash tools/qemu_matrix.sh` — all of the above across every layout.

# Writing to a real stick

`make usb DEV=/dev/sdX` (default LAYOUT=gpt; the whole device is overwritten)
