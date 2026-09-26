# SurfaceOS
 
A hobby x86_64 operating system written in C++ (freestanding, no OOP).
 
# Features
 
- UEFI bootloader (custom EFI loader via MinGW; passes the boot volume's
  partition start and disk signature to the kernel for automount)
- x86_64 kernel: GDT/IDT/PIC, paging (4 KiB, per-process address spaces,
  W^X), TSS, ring-3 user mode
- Preemptive scheduler (switches only at ring-3 boundaries), fork/execve/
  wait4/kill, SysV ABI process startup (argv/envp/auxv on the stack)
- Old POSIX-shaped syscall ABI (int 0x80, -errno results): frozen - no
  new calls - and removed once nothing needs it; programs use the SDK
- Block layer: blkdev registry (USB MSD today, AHCI/NVMe-shaped),
  GPT/MBR/superfloppy partition parsing (SurfaceOS itself lives on GPT;
  the others are for mounting ordinary sticks), LRU sector cache with
  dirty tracking and flush, 512 and 4096-byte sectors
- VFS: vnode cache, mount table (FAT32 root + devfs), POSIX namei
  (`/`-separated paths, `.`, `..` across mount points, LFN)
- FAT32 with long file names (UTF-8, case preserved), FSInfo-based
  allocator, incremental read/write by byte offset, truncate, rename,
  unlink of open files, volume dirty bit
- Per-process fd table (POSIX dup/fork/exec semantics, O_CLOEXEC),
  open file descriptions with shared offsets
- devfs (outside the directory tree, no /dev): null, zero, tty, console;
  canonical-mode terminal with echo; stdin/stdout/stderr are fds 0/1/2
- SurfaceOS SDK (<sfos.h>): a program implements SfMain(SfApp*, SfSystem*)
  and reaches the system through tables of the SDK runtime
  (src/sdk/runtime), which the kernel maps into every program and which
  enters the kernel with the syscall instruction. Sys->Memory gives pages
  and a heap, Sys->Time the clock, the uptime and sleeping,
  Sys->Process the command line of a process. App->Args is the command
  line; a path in it is opened by the console and reaches the program as
  argN:. Files go through roots: data:/
  (the program's own /files/<name>, created on first start) and tmp:/
  (/tmp, unique names from CreateUnique); no path leads above its root.
  The old POSIX layer (int 0x80, libc) is still in the tree until it is
  removed.
- Built-in shell: ls, cat, xxd, write, cp, mv, rm, mkdir, rmdir, cd, pwd,
  mount <dev> (partitions go to /mount/<dev>pN), umount <dev|dir>, sync,
  lsblk, hardware info commands; a program runs by its name (looked up
  in /apps) or by its path
- Programs: hello (Console Print and ReadLine), sdkcheck (the SDK
  tables, memory, time, arguments), sfstest (files through the SDK and
  the roots' sandbox; `sfstest verify` after a restart)
 
# Project Structure
 
```
src/
  boot/       — UEFI bootloader (C, MinGW)
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
  sdk/        — include/sfos.h + sfos/ (the SDK), runtime/ (the code
                behind the SDK tables), sfos.ld (program link script);
                the old POSIX layer: abi/, libc/, linker.ld
  apps/       — programs: <name>.cpp is one program, <name>/ is one
                program of all the .cpp files in it
tools/
  mkimg.py            — image builder: GPT, one FAT32 EFI System Partition
  qemu_exec_test.sh   — full QEMU regression suite
  qemu_verify.sh      — reboot + persistence + host fsck.fat
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
    - Run in terminal: `make run`

# Tests

- `bash tools/qemu_exec_test.sh` — boots QEMU, runs sdkcheck, sfstest,
  mount/umount on a second disk and a leak check, asserts on the serial
  log.
- `bash tools/qemu_verify.sh` — after the suite: reboots its image, runs
  `sfstest verify`, host `fsck.fat -n`.

# Writing to a real stick

`make usb DEV=/dev/sdX` (GPT; the whole device is overwritten)
