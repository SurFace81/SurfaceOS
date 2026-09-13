#!/bin/bash
# Run SurfaceOS in QEMU and type a command sequence via monitor `sendkey`.
# Captures UART output and a final screen dump.
#
# Usage: tools/qemu_interact.sh "<command text>" [boot_wait_s] [run_wait_s]
# Command text supports: letters, digits, space, \n (enter), . \ _ - etc.
set -e

cd "$(dirname "$0")/.."
CMD="$1"
BOOT_WAIT=${2:-20}
RUN_WAIT=${3:-20}
IMG=test_disk.img
MON=/tmp/qmon_int

make bin/boot/efi/BOOTX64.EFI bin/kernel/kernel.bin bin/kernel/data/stdfont.fnt bin/boot/bios/stub.bin >/dev/null 2>&1
make bin/apps/hello.bin >/dev/null 2>&1 || true

rm -f "$IMG" uart.log /tmp/screen_int.ppm
dd if=/dev/zero of="$IMG" bs=1M count=32 status=none
mkfs.fat -F32 "$IMG" >/dev/null 2>&1
dd if=bin/boot/bios/stub.bin of="$IMG" conv=notrunc,fsync status=none
dd if=bin/boot/bios/stub.bin of="$IMG" conv=notrunc,fsync bs=512 seek=6 status=none

python3 tools/mkimg.py "$IMG" \
    bin/boot/efi/BOOTX64.EFI \
    bin/kernel/kernel.bin \
    bin/kernel/data/stdfont.fnt \
    bin/apps/hello.bin >/dev/null 2>&1

rm -f "$MON"
qemu-system-x86_64 \
    -chardev file,id=uart0,path=uart.log \
    -m 128M \
    -bios uefi64.bin \
    -cpu qemu64 \
    -device qemu-xhci \
    -device pci-serial,chardev=uart0 \
    -drive id=usbstick,if=none,format=raw,file="$IMG" \
    -device usb-storage,drive=usbstick \
    -display none -no-reboot -no-shutdown \
    -monitor unix:$MON,server,nowait >/dev/null 2>&1 &
QEMU_PID=$!
trap "kill $QEMU_PID 2>/dev/null" EXIT

sleep "$BOOT_WAIT"

# Type the command
python3 tools/send_keys.py "$MON" "$CMD"

sleep "$RUN_WAIT"

# Screen dump before exit
python3 tools/send_keys.py "$MON" "" >/dev/null 2>&1 || true
python3 - "$MON" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
time.sleep(0.3); s.recv(65536)
s.sendall(b'screendump /tmp/screen_int.ppm\n')
time.sleep(1.5)
s.sendall(b'quit\n')
s.close()
EOF
sleep 1

echo "=== uart.log ==="
cat uart.log 2>/dev/null || echo "(empty)"
echo "=== screen dump: /tmp/screen_int.ppm ==="
python3 tools/render_screen.py /tmp/screen_int.ppm 2>/dev/null | tail -40
