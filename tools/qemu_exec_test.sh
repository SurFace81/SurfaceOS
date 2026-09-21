#!/bin/bash
# Full user-space regression test in QEMU. Everything is checked through the
# serial log: the kernel mirrors app output (SYS_WRITE) and logs every session
# start/end with its exit status.
#
#   1. hi, Enter                  -> normal exit, status 0
#   2. hi, Esc                    -> Esc while blocked in read(0), status SIGINT
#   3. proctest.bin spin, Esc     -> Esc while spinning in ring 3, status 130
#   4. memtest.bin                -> all memory checks pass
#   5. proctest.bin               -> all process/scheduler checks pass
#   6. uptime                     -> the console still works afterwards
#   7. umount /dev busy vs free   -> EBUSY while the cwd is inside it
#   8. meminfo around a session   -> no leaked frames (per-process kstacks)
set -u

cd "$(dirname "$0")/.."
IMG=test_disk.img
MON=/tmp/qmon_exec
LOG=uart.log
BOOT_WAIT=${BOOT_WAIT:-25}
APPS="hi hello memtest proctest argtest fstest"
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
wait_for "boot: root mounted" 10; result $? "root volume automounted at boot"

type_cmd "lsblk";  sleep 3
wait_for "lsblk: usb0" 10; result $? "lsblk lists usb0"
type_cmd "sync";   sleep 3
wait_for "sync: ok" 10; result $? "sync flushes the cache"

type_cmd "cd /bin"; sleep 3

# 1. normal exit
type_cmd "exec hi"
wait_for "Hello world!" 20; result $? "hi runs"
sleep 1; key ret
wait_session_end 1 15; result $? "hi exits on Enter"
[ "$(last_status)" = "0" ]; result $? "hi exit status 0"

# 2. Esc while blocked in a syscall
type_cmd "exec hi"
sleep 4; key esc
wait_session_end 2 15; result $? "Esc ends an app blocked in read_line"
[ "$(last_status)" = "2" ]; result $? "blocked app reports SIGINT (2)"

# 3. Esc while spinning in user mode
type_cmd "exec proctest spin"
wait_for "spinning without syscalls" 20; result $? "spinner started"
sleep 2; key esc
wait_session_end 3 15; result $? "Esc ends an app spinning in ring 3"
[ "$(last_status)" = "2" ]; result $? "spinning app reports SIGINT (2)"

# 4. memtest
type_cmd "exec memtest"
wait_for "memtest: " 240; result $? "memtest finished"
grep -E "\[FAIL\]|status [0-9-]+, expected" "$LOG" | sed 's/^/      /'
grep -q "memtest: [0-9]* passed, 0 failed" "$LOG"; result $? "memtest: no failed checks"
sleep 1; key ret
wait_session_end 4 20; result $? "memtest exits"

# 5. proctest
type_cmd "exec proctest"
wait_for "proctest: " 240; result $? "proctest finished"
grep -q "proctest: [0-9]* passed, 0 failed" "$LOG"; result $? "proctest: no failed checks"
sleep 1; key ret
wait_session_end 5 20; result $? "proctest exits"

# 6. argtest (SysV stack: argv/envp/auxv, execve with 1000 args, E2BIG)
type_cmd "exec argtest"
wait_for "argtest: " 240; result $? "argtest finished"
grep -q "argtest: [0-9]* passed, 0 failed" "$LOG"; result $? "argtest: no failed checks"
wait_session_end 6 20; result $? "argtest exits"

# 7. fstest (fd layer, VFS, FAT32: LFN, O_*, dup/fork, errors, /dev)
type_cmd "exec fstest"
wait_for "fstest: " 600; result $? "fstest finished"
grep -E "\[FAIL\]" "$LOG" | sed 's/^/      /'
grep -q "fstest: [0-9]* passed, 0 failed" "$LOG"; result $? "fstest: no failed checks"
wait_session_end 7 30; result $? "fstest exits"

# 8. umount refuses while the FS is in use, and succeeds once it is not.
#    Standing in /dev gives the console cwd a reference on the devfs root;
#    before the stage-3 cleanup umount freed those vnodes anyway.
type_cmd "cd /dev"; sleep 2
type_cmd "umount /dev"; sleep 3
wait_for "umount: busy /dev" 10; result $? "umount refuses a filesystem in use"
type_cmd "cd /"; sleep 2
type_cmd "umount /dev"; sleep 3
wait_for "umount: ok /dev" 10; result $? "umount succeeds once nothing holds it"

# 9. per-process kernel stacks are handed back when a session ends: run a
#    session between two meminfo samples and compare the free-frame counts.
type_cmd "meminfo"; sleep 3
FRAMES_BEFORE=$(grep "meminfo: frames_free=" "$LOG" | tail -1 | grep -oE 'frames_free=[0-9]+' | cut -d= -f2)
type_cmd "exec hi"
wait_for "Hello world!" 20
sleep 1; key ret
wait_session_end $(( $(sessions_ended) + 1 )) 15
type_cmd "meminfo"; sleep 3
FRAMES_AFTER=$(grep "meminfo: frames_free=" "$LOG" | tail -1 | grep -oE 'frames_free=[0-9]+' | cut -d= -f2)
echo "      frames free: $FRAMES_BEFORE -> $FRAMES_AFTER"
[ -n "$FRAMES_BEFORE" ] && [ "$FRAMES_BEFORE" = "$FRAMES_AFTER" ]
result $? "a session leaks no physical frames (kernel stacks freed)"

# 10. console still alive
monitor "screendump /tmp/scr_final.ppm"
type_cmd "uptime"; sleep 3
! grep -q "KERNEL PANIC\|kernel fault" "$LOG"; result $? "no kernel faults"

monitor "quit"
sleep 1

echo
echo "=== summary lines ==="
grep -E "^cpu:|^pmm:|memtest: |proctest: |argtest: |fstest: |session (start|end)|kernel fault|app fault" "$LOG"
echo
if [ $FAILS -eq 0 ]; then echo "ALL CHECKS PASSED"; else echo "$FAILS CHECK(S) FAILED"; fi
exit $FAILS
