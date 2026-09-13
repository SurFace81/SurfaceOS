#!/bin/bash
# Build + run SurfaceOS in QEMU (UEFI), capture UART output to uart.log.
# Non-interactive replacement for `make run` (no sudo, no monitor).
#
# Usage: tools/qemu_test.sh [timeout_seconds]
set -e

cd "$(dirname "$0")/.."
TIMEOUT=${1:-30}
IMG=test_disk.img

# Build everything
make bin/boot/efi/BOOTX64.EFI bin/kernel/kernel.bin bin/kernel/data/stdfont.fnt >/dev/null
make bin/apps/hello.bin >/dev/null 2>&1 || true

# Fresh image: FAT32 + BIOS stub at sectors 0 and 6 (UEFI boot uses ESP)
rm -f "$IMG" uart.log
dd if=/dev/zero of="$IMG" bs=1M count=32 status=none
mkfs.fat -F32 "$IMG" >/dev/null 2>&1
dd if=bin/boot/bios/stub.bin of="$IMG" conv=notrunc,fsync status=none
dd if=bin/boot/bios/stub.bin of="$IMG" conv=notrunc,fsync bs=512 seek=6 status=none

python3 tools/mkimg.py "$IMG" \
    bin/boot/efi/BOOTX64.EFI \
    bin/kernel/kernel.bin \
    bin/kernel/data/stdfont.fnt \
    bin/apps/hello.bin >/dev/null

timeout "$TIMEOUT" qemu-system-x86_64 \
    -chardev file,id=uart0,path=uart.log \
    -m 128M \
    -bios uefi64.bin \
    -cpu qemu64 \
    -device qemu-xhci \
    -device pci-serial,chardev=uart0 \
    -drive id=usbstick,if=none,format=raw,file="$IMG" \
    -device usb-storage,drive=usbstick \
    -display none -no-reboot -no-shutdown >/dev/null 2>&1 || true

echo "=== uart.log ==="
cat uart.log 2>/dev/null || echo "(empty)"
