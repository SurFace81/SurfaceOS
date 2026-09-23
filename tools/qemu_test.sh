#!/bin/bash
# Build + run SurfaceOS in QEMU (UEFI), capture UART output to uart.log.
# Non-interactive replacement for `make run` (no sudo, no monitor).
#
# Usage: tools/qemu_test.sh [timeout_seconds]
set -e

cd "$(dirname "$0")/.."
TIMEOUT=${1:-30}
IMG=test_disk.img
LAYOUT=${LAYOUT:-gpt}

rm -f uart.log
bash tools/make_test_image.sh "$IMG" "hello" "$LAYOUT"

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
