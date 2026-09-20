#!/bin/bash
# Full user-space regression test in QEMU. Everything is checked through the
# serial log: the kernel mirrors app output (SYS_WRITE) and logs every session
# start/end with its exit status.
#
#   1. hi.bin, Enter              -> normal exit, status 0
#   2. hi.bin, Esc                -> Esc while blocked in read_line, status 130
#   3. proctest.bin spin, Esc     -> Esc while spinning in ring 3, status 130
#   4. memtest.bin                -> all memory checks pass
#   5. proctest.bin               -> all process/scheduler checks pass
#   6. uptime                     -> the console still works afterwards
set -u

cd "$(dirname "$0")/.."
IMG=test_disk.img
MON=/tmp/qmon_exec
LOG=uart.log
BOOT_WAIT=${BOOT_WAIT:-25}
APPS="hi hello memtest proctest"

make bin/boot/efi/BOOTX64.EFI bin/kernel/kernel.bin bin/kernel/data/stdfont.fnt bin/boot/bios/stub.bin >/dev/null || exit 1
for a in $APPS; do make bin/apps/$a.bin >/dev/null || exit 1; done

rm -f "$IMG" "$LOG" /tmp/scr_*.ppm
dd if=/dev/zero of="$IMG" bs=1M count=32 status=none
mkfs.fat -F32 "$IMG" >/dev/null 2>&1
dd if=bin/boot/bios/stub.bin of="$IMG" conv=notrunc,fsync status=none
dd if=bin/boot/bios/stub.bin of="$IMG" conv=notrunc,fsync bs=512 seek=6 status=none

python3 tools/mkimg.py "$IMG" \
    bin/boot/efi/BOOTX64.EFI \
    bin/kernel/kernel.bin \
    bin/kernel/data/stdfont.fnt \
    $(for a in $APPS; do echo bin/apps/$a.bin; done) >/dev/null 2>&1

rm -f "$MON"
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

monitor() {  # monitor "<command>"
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

key()      { monitor "sendkey $1"; }
type_cmd() { python3 tools/send_keys.py "$MON" "$1"; key ret; }

# wait_for "<pattern>" <seconds>: poll the serial log (fixed-string match)
wait_for() {
    local deadline=$((SECONDS + $2))
    while [ $SECONDS -lt $deadline ]; do
        grep -qF -- "$1" "$LOG" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

sessions_ended() { grep -c "process: session end" "$LOG" 2>/dev/null || echo 0; }

# wait until the N-th session has ended
wait_session_end() {
    local deadline=$((SECONDS + $2))
    while [ $SECONDS -lt $deadline ]; do
        [ "$(sessions_ended)" -ge "$1" ] && return 0
        sleep 1
    done
    return 1
}

FAILS=0
result() {  # result <ok:0/1> "<description>"
    if [ "$1" -eq 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; FAILS=$((FAILS+1)); fi
}

last_status() { grep "process: session end" "$LOG" | tail -1 | grep -oE '[0-9]+$'; }

wait_for "boot: console ready" "$BOOT_WAIT"; result $? "kernel boots to the console"

type_cmd "mount";   sleep 8
type_cmd "cd APPS"; sleep 3

# 1. normal exit
type_cmd "exec hi.bin"
wait_for "Hello world!" 20; result $? "hi.bin runs"
sleep 1; key ret
wait_session_end 1 15; result $? "hi.bin exits on Enter"
[ "$(last_status)" = "0" ]; result $? "hi.bin exit status 0"

# 2. Esc while blocked in a syscall
type_cmd "exec hi.bin"
sleep 4; key esc
wait_session_end 2 15; result $? "Esc ends an app blocked in read_line"
[ "$(last_status)" = "2" ]; result $? "blocked app reports SIGINT (2)"

# 3. Esc while spinning in user mode
type_cmd "exec proctest.bin spin"
wait_for "spinning without syscalls" 20; result $? "spinner started"
sleep 2; key esc
wait_session_end 3 15; result $? "Esc ends an app spinning in ring 3"
[ "$(last_status)" = "2" ]; result $? "spinning app reports SIGINT (2)"

# 4. memtest
type_cmd "exec memtest.bin"
wait_for "memtest: " 240; result $? "memtest finished"
grep -E "\[FAIL\]|status [0-9-]+, expected" "$LOG" | sed 's/^/      /'
grep -q "memtest: [0-9]* passed, 0 failed" "$LOG"; result $? "memtest: no failed checks"
sleep 1; key ret
wait_session_end 4 20; result $? "memtest exits"

# 5. proctest
type_cmd "exec proctest.bin"
wait_for "proctest: " 240; result $? "proctest finished"
grep -q "proctest: [0-9]* passed, 0 failed" "$LOG"; result $? "proctest: no failed checks"
sleep 1; key ret
wait_session_end 5 20; result $? "proctest exits"

# 6. console still alive
monitor "screendump /tmp/scr_final.ppm"
type_cmd "uptime"; sleep 3
! grep -q "KERNEL PANIC\|kernel fault" "$LOG"; result $? "no kernel faults"

monitor "quit"
sleep 1

echo
echo "=== summary lines ==="
grep -E "^cpu:|^pmm:|memtest: |proctest: |session (start|end)|kernel fault|app fault" "$LOG"
echo
if [ $FAILS -eq 0 ]; then echo "ALL CHECKS PASSED"; else echo "$FAILS CHECK(S) FAILED"; fi
exit $FAILS
