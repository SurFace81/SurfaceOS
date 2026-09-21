#!/bin/bash
# Persistence check (stage 3.7): reboot QEMU with the image the fstest run
# left behind, run `fstest verify` and confirm everything survived -
# including the host-side fsck.fat of the volume.
#
# Usage: tools/qemu_verify.sh [layout]
#   Expects test_disk.img to contain a finished fstest run (qemu_exec_test.sh).
set -u
cd "$(dirname "$0")/.."

LAYOUT="${1:-gpt}"
IMG=test_disk.img
MON=/tmp/qmon_verify
LOG=/tmp/uart_verify.log

if [ ! -f "$IMG" ]; then
    echo "FAIL: $IMG missing - run qemu_exec_test.sh first"
    exit 1
fi

rm -f "$LOG" "$MON"
qemu-system-x86_64 \
    -chardev file,id=uart0,path=$LOG \
    -m ${QEMU_MEM:-128M} \
    -bios uefi64.bin \
    -cpu ${QEMU_CPU:-qemu64} \
    -device qemu-xhci \
    -device pci-serial,chardev=uart0 \
    -drive id=usbstick,if=none,format=raw,file="$IMG" \
    -device usb-storage,drive=usbstick \
    -display none -no-reboot -no-shutdown \
    -monitor unix:$MON,server,nowait >/dev/null 2>&1 &
QEMU_PID=$!
trap "kill $QEMU_PID 2>/dev/null" EXIT

monitor() {
    python3 - "$MON" "$1" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
time.sleep(0.2); s.recv(65536)
s.sendall((sys.argv[2] + '\n').encode())
time.sleep(0.4)
s.close()
PY
}
type_cmd() { python3 tools/send_keys.py "$MON" "$1"; monitor "sendkey ret"; }

wait_for() {
    local deadline=$((SECONDS + $2))
    while [ $SECONDS -lt $deadline ]; do
        grep -qF -- "$1" "$LOG" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

FAILS=0
result() {
    if [ "$1" -eq 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; FAILS=$((FAILS+1)); fi
}

wait_for "boot: console ready" 40; result $? "kernel boots to the console"
wait_for "boot: root mounted" 10; result $? "root automounted again"

# The image was not cleanly unmounted (QEMU was killed): the dirty-bit
# warning must appear, proving the flag round-trips.
wait_for "volume is dirty" 5; result $? "dirty volume detected and reported"

type_cmd "exec /bin/fstest verify"
wait_for "fstest verify: " 240; result $? "fstest verify finished"
grep -E "\[FAIL\]" "$LOG" | sed 's/^/      /'
grep -q "fstest verify: [0-9]* passed, 0 failed" "$LOG"
result $? "all data survived the restart"

monitor "quit"
sleep 1

# Host-side FAT validation.
PART=/tmp/verify_part.fat
if [ "$LAYOUT" = "superfloppy" ]; then
    cp "$IMG" "$PART"
else
    TOTAL=$(stat -c %s "$IMG")
    dd if="$IMG" of="$PART" bs=512 skip=2048 count=$(( (TOTAL - 2048*512) / 512 )) status=none
fi
if fsck.fat -n "$PART" >/tmp/verify_fsck.log 2>&1; then
    echo "PASS  host fsck.fat -n is clean"
else
    echo "FAIL  host fsck.fat -n:"
    tail -6 /tmp/verify_fsck.log | sed 's/^/      /'
    FAILS=$((FAILS+1))
fi
rm -f "$PART"

echo
if [ $FAILS -eq 0 ]; then echo "VERIFY: ALL PASSED"; else echo "VERIFY: $FAILS FAILURE(S)"; fi
exit $FAILS
