#!/bin/bash
# Full user-space regression test in QEMU. Everything is checked through the
# serial log: the kernel mirrors app output (SYS_WRITE) and logs every session
# start/end with its exit status.
#
#   1. hi, Enter                  -> normal exit, status 0
#   2. hi, Ctrl+C                 -> interrupt while blocked in read(0)
#   3. proctest.bin spin, Ctrl+C  -> interrupt while spinning in ring 3
#   4. memtest.bin                -> all memory checks pass
#   5. proctest.bin               -> all process/scheduler checks pass
#   6. uptime                     -> the console still works afterwards
#   7. umount /dev busy vs free   -> EBUSY while the cwd is inside it
#   8. meminfo around a session   -> no leaked frames (per-process kstacks)
#   9. hi, Ctrl+D                 -> EOF ends a canonical read
#  10. termtest                   -> CP437 upper half renders
#  11. keys                      -> every key reaches the app with its code
#  12. keys decode               -> raw mode + CSI u: Ctrl+1, Ctrl+Shift+S
set -u

# termtest writes raw CP437 bytes and escape sequences to the serial log, so
# in a UTF-8 locale grep starts seeing NEL line terminators and stray
# encodings, and anchors stop matching. Byte semantics everywhere, and -a on
# every grep, keeps the log parsing independent of what the apps printed.
export LC_ALL=C

cd "$(dirname "$0")/.."
IMG=test_disk.img
MON=/tmp/qmon_exec
LOG=uart.log
BOOT_WAIT=${BOOT_WAIT:-25}
APPS="hi hello memtest proctest argtest fstest termtest keys"
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
        grep -aqF -- "$1" "$LOG" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

sessions_ended() { grep -ac "process: session end" "$LOG" 2>/dev/null || echo 0; }

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

last_status() { grep -a "process: session end" "$LOG" | tail -1 | grep -aoE '[0-9]+$'; }

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

# 2. Ctrl+C while blocked in a syscall. Esc used to be the kill key, which
#    meant no application could ever see Esc or an escape sequence.
type_cmd "exec hi"
sleep 4; key ctrl-c
wait_session_end 2 15; result $? "Ctrl+C ends an app blocked in read"
[ "$(last_status)" = "2" ]; result $? "blocked app reports SIGINT (2)"

# 3. Ctrl+C while spinning in user mode
type_cmd "exec proctest spin"
wait_for "spinning without syscalls" 20; result $? "spinner started"
sleep 2; key ctrl-c
wait_session_end 3 15; result $? "Ctrl+C ends an app spinning in ring 3"
[ "$(last_status)" = "2" ]; result $? "spinning app reports SIGINT (2)"

# 4. memtest
type_cmd "exec memtest"
wait_for "memtest: " 240; result $? "memtest finished"
grep -aE "\[FAIL\]|status [0-9-]+, expected" "$LOG" | sed 's/^/      /'
grep -aq "memtest: [0-9]* passed, 0 failed" "$LOG"; result $? "memtest: no failed checks"
sleep 1; key ret
wait_session_end 4 20; result $? "memtest exits"

# 5. proctest
type_cmd "exec proctest"
wait_for "proctest: " 240; result $? "proctest finished"
grep -aq "proctest: [0-9]* passed, 0 failed" "$LOG"; result $? "proctest: no failed checks"
sleep 1; key ret
wait_session_end 5 20; result $? "proctest exits"

# 6. argtest (SysV stack: argv/envp/auxv, execve with 1000 args, E2BIG)
type_cmd "exec argtest"
wait_for "argtest: " 240; result $? "argtest finished"
grep -aq "argtest: [0-9]* passed, 0 failed" "$LOG"; result $? "argtest: no failed checks"
wait_session_end 6 20; result $? "argtest exits"

# 7. fstest (fd layer, VFS, FAT32: LFN, O_*, dup/fork, errors, /dev)
type_cmd "exec fstest"
wait_for "fstest: " 600; result $? "fstest finished"
grep -aE "\[FAIL\]" "$LOG" | sed 's/^/      /'
grep -aq "fstest: [0-9]* passed, 0 failed" "$LOG"; result $? "fstest: no failed checks"
wait_session_end 7 30; result $? "fstest exits"

# 8. Ctrl+D on an empty line is EOF. Until the keyboard learned to fold Ctrl,
#    Ctrl+D arrived as plain 'd' and the canonical read blocked forever.
type_cmd "exec hi"
wait_for "Hello world!" 20; result $? "hi runs (Ctrl+D scenario)"
# Snapshot the count *before* the key: the session can end between the
# substitution and the wait, and we would then sit waiting for one too many.
WANT=$(( $(sessions_ended) + 1 ))
sleep 1; key ctrl-d
wait_session_end $WANT 15
result $? "Ctrl+D ends a canonical read (EOF)"
[ "$(last_status)" = "0" ]; result $? "app reading EOF exits cleanly"

# 9. termtest: the CP437 upper half renders (box drawing, blocks, symbols).
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "exec termtest"
wait_for "termtest: done" 60; result $? "termtest finished"
monitor "screendump /tmp/scr_term.ppm"
sleep 1; key ret
wait_session_end $WANT 20; result $? "termtest exits"

# 10. keys: every key reaches an application with a distinct code. Function
#     keys, the navigation cluster and the keypad used to be dropped by a
#     whitelist in the driver, or arrive with no character at all.
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "exec keys"
wait_for "keys: press any key" 30; result $? "keys started"
for k in f5 up home delete ctrl-a; do key $k; sleep 1; done
sleep 2
grep -aq "key code=63 .*name=F5"        "$LOG"; result $? "F5 reaches the app"
grep -aq "key code=200 .*name=Up"       "$LOG"; result $? "arrow keys reach the app"
grep -aq "key code=199 .*name=Home"     "$LOG"; result $? "Home reaches the app"
grep -aq "key code=211 .*name=Delete"   "$LOG"; result $? "Delete reaches the app"
grep -aq "key code=30 char=0x0*1 mods=CTRL" "$LOG"; result $? "Ctrl+A folds to 0x01"
key esc
wait_session_end $WANT 20; result $? "keys exits"

# 11. keys decode: the same keyboard through raw mode and the SDK decoder.
#     Ctrl+1 and Ctrl+Shift+S are the point - classic xterm sequences cannot
#     express either, so they only arrive because CSI u is on. Note Ctrl+C
#     does not interrupt here: raw mode clears ISIG, so Esc is the way out.
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "exec keys decode"
wait_for "keys decode: raw mode" 30; result $? "keys decode entered raw mode"
for k in up f5 ctrl-1 ctrl-shift-s alt-x; do key $k; sleep 1; done
sleep 2
grep -aq "dec code=200 mods=0 name=Up"          "$LOG"; result $? "arrows decode in raw mode"
grep -aq "dec code=63 mods=0 name=F5"           "$LOG"; result $? "function keys decode"
grep -aq "dec code=49 mods=2 name=Ctrl+1"       "$LOG"; result $? "Ctrl+digit survives (CSI u)"
grep -aq "dec code=115 mods=3 name=Ctrl+Shift+s" "$LOG"; result $? "Ctrl+Shift+letter survives"
grep -aq "dec code=120 mods=4 name=Alt+x"       "$LOG"; result $? "Alt+key decodes"
key esc
wait_for "keys decode: done" 20; result $? "raw mode restored on exit"
wait_session_end $WANT 20; result $? "keys decode exits"

# 12. umount refuses while the FS is in use, and succeeds once it is not.
#    Standing in /dev gives the console cwd a reference on the devfs root;
#    before the stage-3 cleanup umount freed those vnodes anyway.
type_cmd "cd /dev"; sleep 2
type_cmd "umount /dev"; sleep 3
wait_for "umount: busy /dev" 10; result $? "umount refuses a filesystem in use"
type_cmd "cd /"; sleep 2
type_cmd "umount /dev"; sleep 3
wait_for "umount: ok /dev" 10; result $? "umount succeeds once nothing holds it"

# 13. per-process kernel stacks are handed back when a session ends: run a
#    session between two meminfo samples and compare the free-frame counts.
type_cmd "meminfo"; sleep 3
FRAMES_BEFORE=$(grep -a "meminfo: frames_free=" "$LOG" | tail -1 | grep -aoE 'frames_free=[0-9]+' | cut -d= -f2)
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "exec hi"
wait_for "Hello world!" 20
sleep 1; key ret
wait_session_end $WANT 15; result $? "session between meminfo samples ended"
type_cmd "meminfo"; sleep 3
FRAMES_AFTER=$(grep -a "meminfo: frames_free=" "$LOG" | tail -1 | grep -aoE 'frames_free=[0-9]+' | cut -d= -f2)
echo "      frames free: $FRAMES_BEFORE -> $FRAMES_AFTER"
[ -n "$FRAMES_BEFORE" ] && [ "$FRAMES_BEFORE" = "$FRAMES_AFTER" ]
result $? "a session leaks no physical frames (kernel stacks freed)"

# 14. console still alive
monitor "screendump /tmp/scr_final.ppm"
type_cmd "uptime"; sleep 3
! grep -aq "KERNEL PANIC\|kernel fault" "$LOG"; result $? "no kernel faults"

monitor "quit"
sleep 1

echo
echo "=== summary lines ==="
grep -aE "^cpu:|^pmm:|memtest: |proctest: |argtest: |fstest: |session (start|end)|kernel fault|app fault" "$LOG"
echo
if [ $FAILS -eq 0 ]; then echo "ALL CHECKS PASSED"; else echo "$FAILS CHECK(S) FAILED"; fi
exit $FAILS
