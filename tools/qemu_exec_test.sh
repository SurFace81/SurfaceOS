#!/bin/bash
# Full user-space regression test in QEMU. Everything is checked through the
# serial log: the kernel mirrors app output (SYS_WRITE) and the console logs
# every program start/end with its exit status.
#
#   1. sdkcheck args.txt word     -> the SDK tables and services, argN: roots,
#                                    SfStatus as exit status
#   2. mount/umount a second disk -> /mount/usb1pN, EBUSY while the cwd is inside
#   3. meminfo around a program   -> no leaked frames (kernel stacks, SDK pages)
set -u

# Programs may write any bytes to the serial log: byte semantics everywhere,
# and -a on every grep, keep the log parsing independent of what they print.
export LC_ALL=C

cd "$(dirname "$0")/.."
IMG=test_disk.img
# Second disk (two FAT32 partitions) for mount/umount: usb1.
DATA_IMG=test_data.img
MON=/tmp/qmon_exec
LOG=uart.log
BOOT_WAIT=${BOOT_WAIT:-25}
APPS="sdkcheck"
# LAYOUT: superfloppy | mbr | gpt (default gpt - what a real stick looks like)
LAYOUT=${LAYOUT:-gpt}
# SECTOR: 512 | 4096 (4096 only with LAYOUT=superfloppy, see mkimg.py)
SECTOR=${SECTOR:-512}
# A 4K-sector FAT32 needs >= 65536 sectors to get a 32-bit TotalSectors:
# at least 256 MiB.
if [ "$SECTOR" != "512" ]; then
    IMG_SIZE=${IMG_SIZE:-512}
else
    IMG_SIZE=${IMG_SIZE:-64}
fi

bash tools/make_test_image.sh "$IMG" "$APPS" "$LAYOUT" "$IMG_SIZE" "$SECTOR"
bash tools/make_data_disk.sh "$DATA_IMG"

# QEMU device for a non-512 sector size: usb-storage does not forward
# logical_block_size to its child scsi-hd, so the 4K device is built as
# usb-bot + explicit scsi-hd. Note: the 4K image must be >= 256 MiB for
# mkfs.fat to produce a valid FAT32 (64 MiB fits in TotalSectors16).
if [ "$SECTOR" = "512" ]; then
    USB_DEV="-device usb-storage,drive=usbstick"
else
    USB_DEV="-device usb-bot,id=msd -device scsi-hd,bus=msd.0,drive=usbstick,logical_block_size=$SECTOR,physical_block_size=$SECTOR"
fi

rm -f "$LOG" /tmp/scr_*.ppm
rm -f "$MON"
qemu-system-x86_64 \
    -chardev file,id=uart0,path=$LOG \
    -m ${QEMU_MEM:-128M} \
    -bios uefi64.bin \
    -cpu ${QEMU_CPU:-qemu64} \
    -device qemu-xhci \
    -device pci-serial,chardev=uart0 \
    -drive id=usbstick,if=none,format=raw,file="$IMG" \
    $USB_DEV \
    -drive id=data,if=none,format=raw,file="$DATA_IMG" \
    -device usb-storage,drive=data \
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
        grep -aqF -- "$1" "$LOG" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

# Occurrences, not lines: an app that ends its output without a newline
# shares a line with the kernel message that follows, and grep -c would
# count the pair once.
sessions_ended() { grep -ao "console: program end" "$LOG" 2>/dev/null | wc -l; }

# wait until the N-th session has ended
wait_session_end() {
    local deadline=$((SECONDS + $2))
    while [ $SECONDS -lt $deadline ]; do
        [ "$(sessions_ended)" -ge "$1" ] && return 0
        sleep 1
    done
    return 1
}

# key_await <key> "<pattern>" [seconds]: press a key and wait for the output
# it should produce. Fixed sleeps made these checks fail on a loaded host
# even though nothing was wrong - polling just takes longer instead.
key_await() {
    local k="$1" pat="$2" secs="${3:-10}"
    key "$k"
    local deadline=$((SECONDS + secs))
    while [ $SECONDS -lt $deadline ]; do
        grep -aqF -- "$pat" "$LOG" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

FAILS=0
result() {  # result <ok:0/1> "<description>"
    if [ "$1" -eq 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; FAILS=$((FAILS+1)); fi
}

last_status() { grep -a "console: program end" "$LOG" | tail -1 | grep -aoE '[0-9]+$'; }

wait_for "boot: console ready" "$BOOT_WAIT"; result $? "kernel boots to the console"
wait_for "boot: root mounted" 10; result $? "root volume automounted at boot"

type_cmd "lsblk";  sleep 3
wait_for "lsblk: usb0" 10; result $? "lsblk lists usb0"
type_cmd "sync";   sleep 3
wait_for "sync: ok" 10; result $? "sync flushes the cache"

type_cmd "cd /apps"; sleep 3

# 1. sdkcheck: a program on the SurfaceOS SDK - the tables and services it
#    starts with, and args.txt (created by the console) as arg1:; its
#    SfStatus becomes the exit status.
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck args.txt word"
wait_for "sdkcheck: " 20; result $? "sdkcheck finished"
grep -aq "sdkcheck: [0-9]* passed, 0 failed" "$LOG"; result $? "sdkcheck: no failed checks"
wait_session_end $WANT 15; result $? "sdkcheck exits"
[ "$(last_status)" = "0" ]; result $? "its SfStatus comes back as exit status 0"

# 2. mount puts every partition of the second disk under /mount; umount
#    refuses while the FS is in use (the console cwd holds its root), and
#    succeeds once it is not, removing the mount point.
type_cmd "mount usb1"; sleep 3
wait_for "mount: usb1p1 on /mount/usb1p1" 10; result $? "mount usb1 mounts usb1p1"
wait_for "mount: usb1p2 on /mount/usb1p2" 10; result $? "mount usb1 mounts usb1p2"
type_cmd "cd /mount/usb1p1"; sleep 2
type_cmd "umount usb1"; sleep 3
wait_for "umount: busy usb1p1" 10; result $? "umount refuses a filesystem in use"
wait_for "umount: ok usb1p2" 10; result $? "umount takes down the idle partition"
type_cmd "cd /"; sleep 2
type_cmd "umount usb1"; sleep 3
wait_for "umount: ok usb1p1" 10; result $? "umount succeeds once nothing holds it"

# 3. per-process kernel stacks and SDK pages are handed back when a program
#    ends: run one between two meminfo samples and compare free frames.
type_cmd "meminfo"; sleep 3
FRAMES_BEFORE=$(grep -a "meminfo: frames_free=" "$LOG" | tail -1 | grep -aoE 'frames_free=[0-9]+' | cut -d= -f2)
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck"
wait_session_end $WANT 15; result $? "program between meminfo samples ended"
type_cmd "meminfo"; sleep 3
FRAMES_AFTER=$(grep -a "meminfo: frames_free=" "$LOG" | tail -1 | grep -aoE 'frames_free=[0-9]+' | cut -d= -f2)
echo "      frames free: $FRAMES_BEFORE -> $FRAMES_AFTER"
[ -n "$FRAMES_BEFORE" ] && [ "$FRAMES_BEFORE" = "$FRAMES_AFTER" ]
result $? "a program leaks no physical frames"

# 4. console still alive
monitor "screendump /tmp/scr_final.ppm"
type_cmd "uptime"; sleep 3
! grep -aq "KERNEL PANIC\|kernel fault" "$LOG"; result $? "no kernel faults"

monitor "quit"
sleep 1

echo
echo "=== summary lines ==="
grep -aE "^cpu:|^pmm:|sdkcheck: |program (start|end)|kernel fault|app fault" "$LOG"
echo
if [ $FAILS -eq 0 ]; then echo "ALL CHECKS PASSED"; else echo "$FAILS CHECK(S) FAILED"; fi
exit $FAILS
