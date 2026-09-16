#!/bin/bash
# Full exec regression test in QEMU:
#   1. boot, mount, cd APPS
#   2. exec hi.bin  -> check title bar + "Hello world!" on screen
#   3. Enter        -> check console prompt is back
#   4. type text    -> check characters render
set -e

cd "$(dirname "$0")/.."
IMG=test_disk.img
MON=/tmp/qmon_exec
BOOT_WAIT=${BOOT_WAIT:-25}

make bin/boot/efi/BOOTX64.EFI bin/kernel/kernel.bin bin/kernel/data/stdfont.fnt bin/boot/bios/stub.bin >/dev/null
make bin/apps/hi.bin bin/apps/hello.bin >/dev/null 2>&1

rm -f "$IMG" uart.log /tmp/scr_*.ppm
dd if=/dev/zero of="$IMG" bs=1M count=32 status=none
mkfs.fat -F32 "$IMG" >/dev/null 2>&1
dd if=bin/boot/bios/stub.bin of="$IMG" conv=notrunc,fsync status=none
dd if=bin/boot/bios/stub.bin of="$IMG" conv=notrunc,fsync bs=512 seek=6 status=none

python3 tools/mkimg.py "$IMG" \
    bin/boot/efi/BOOTX64.EFI \
    bin/kernel/kernel.bin \
    bin/kernel/data/stdfont.fnt \
    bin/apps/hi.bin bin/apps/hello.bin >/dev/null 2>&1

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

dump() {  # dump <name>
    python3 - "$MON" "/tmp/scr_$1.ppm" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
time.sleep(0.3); s.recv(65536)
s.sendall(('screendump %s\n' % sys.argv[2]).encode())
time.sleep(1.5)
s.close()
EOF
}

enter() {  # send Enter directly (bash command substitution eats trailing \n)
    python3 - "$MON" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
time.sleep(0.2); s.recv(65536)
s.sendall(b'sendkey ret\n')
time.sleep(0.4); s.recv(65536)
s.close()
PY
}

type_cmd() {  # type_cmd "<text>" then Enter
    python3 tools/send_keys.py "$MON" "$1"
    enter
}

sleep "$BOOT_WAIT"
dump 01_boot

type_cmd "mount";        sleep 8
type_cmd "cd APPS";      sleep 4
dump 02_cd_apps

type_cmd "exec hi.bin";  sleep 15
dump 03_hi_running

enter; sleep 3   # Enter -> exit app
dump 04_back_to_console

type_cmd "uptime";       sleep 3   # typing renders + command works
dump 05_typed_uptime

python3 - "$MON" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
time.sleep(0.3); s.recv(65536)
s.sendall(b'quit\n')
s.close()
EOF
sleep 1

echo "=== uart.log ==="
cat uart.log 2>/dev/null || echo "(empty)"

for f in /tmp/scr_0*.ppm; do
    echo "=== $f ==="
    python3 tools/render_screen.py "$f" | tail -30
done
