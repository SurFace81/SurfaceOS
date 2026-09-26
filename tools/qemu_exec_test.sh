#!/bin/bash
# Full user-space regression test in QEMU. Everything is checked through the
# serial log: the kernel mirrors app output (SYS_WRITE) and the console logs
# every program start/end with its exit status.
#
#   1. sdkcheck args.txt word     -> the SDK tables and services, argN: roots,
#                                    SfStatus as exit status
#   2. sfstest                    -> files through the SDK, the roots' sandbox;
#                                    leaves data for qemu_verify.sh
#   3. threadtest                 -> a fault in one thread, ^C and the last
#                                    thread's exit end the whole program; ^C
#                                    ends the programs it started too; the
#                                    stress run (8 threads x 3, children)
#   4. mount/umount a second disk -> /mount/usb1pN, EBUSY while the cwd is inside
#   5. meminfo around a program   -> no leaked frames (kernel stacks, SDK pages)
#
# SMP=<n> runs QEMU with n CPUs (default 1).
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
APPS="sdkcheck sfstest threadtest"

bash tools/make_test_image.sh "$IMG" "$APPS"
bash tools/make_data_disk.sh "$DATA_IMG"

rm -f "$LOG" /tmp/scr_*.ppm
rm -f "$MON"
qemu-system-x86_64 \
    -chardev file,id=uart0,path=$LOG \
    -m ${QEMU_MEM:-128M} \
    -smp ${SMP:-1} \
    -bios uefi64.bin \
    -cpu ${QEMU_CPU:-qemu64} \
    -device qemu-xhci \
    -device pci-serial,chardev=uart0 \
    -drive id=usbstick,if=none,format=raw,file="$IMG" \
    -device usb-storage,drive=usbstick \
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

# 1a. Input owners: a child started with SF_START_GIVE_INPUT reads the
#     first line, the parent the next one once the child has ended; keys
#     typed on screen 2, which nobody owns, go nowhere.
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck input"
wait_for "the reader has the keys" 20; result $? "sdkcheck input started its reader"
type_cmd "first"
wait_for "sdkcheck input: child got first" 10; result $? "the child got the first line"
type_cmd "second"
wait_for "sdkcheck input: parent got second" 10; result $? "the parent got the next one"
wait_session_end $WANT 15; result $? "sdkcheck input exits"
key alt-f2
python3 tools/send_keys.py "$MON" "zz"     # no Enter: leaked, it would
key alt-f1                                  # prefix the next command
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck reader"
type_cmd "screens"
wait_for "sdkcheck input: child got screens" 10
result $? "keys typed on screen 2 did not reach screen 1"
wait_session_end $WANT 15

# 2. sfstest: files through the SDK (data:/, tmp:/, the sandbox). What it
#    leaves in data:/ is read back after a restart by qemu_verify.sh.
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sfstest"
wait_for "sfstest: " 240; result $? "sfstest finished"
grep -aE "\[FAIL\]" "$LOG" | sed 's/^/      /'
grep -aq "sfstest: [0-9]* passed, 0 failed" "$LOG"; result $? "sfstest: no failed checks"
wait_session_end $WANT 15; result $? "sfstest exits"

# 3. threadtest: however a program with several threads ends, all of them
#    end - a fault in one (SIGSEGV = 11), ^C (SIGINT = 2), or the last
#    thread leaving after the first (its status 42, wait format 42 << 8).
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "threadtest fault"
wait_session_end $WANT 15; result $? "threadtest fault ends"
[ "$(last_status)" = "11" ]; result $? "a fault in one thread ends the program (SIGSEGV)"

WANT=$(( $(sessions_ended) + 1 ))
type_cmd "threadtest spin"; sleep 3
monitor "sendkey ctrl-c"
wait_session_end $WANT 15; result $? "threadtest spin ends on ^C"
[ "$(last_status)" = "2" ]; result $? "^C ends every thread (SIGINT)"

WANT=$(( $(sessions_ended) + 1 ))
type_cmd "threadtest lastexit"
wait_session_end $WANT 15; result $? "threadtest lastexit ends"
[ "$(last_status)" = "$((42 << 8))" ]; result $? "the last thread's status is the program's"

# Many threads at once, over every CPU there is (run with SMP=4 too).
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "threadtest stress"
wait_for "threadtest stress: " 240; result $? "threadtest stress finished"
grep -aE "\[FAIL\]" "$LOG" | sed 's/^/      /'
grep -aq "threadtest stress: [0-9]* passed, 0 failed" "$LOG"; result $? "threadtest stress: no failed checks"
wait_session_end $WANT 15; result $? "threadtest stress exits"

# ^C reaches the programs a program started: they share its console.
type_cmd "meminfo"; sleep 3
FRAMES_BEFORE=$(grep -a "meminfo: frames_free=" "$LOG" | tail -1 | grep -aoE 'frames_free=[0-9]+' | cut -d= -f2)
STARTED_BEFORE=$(grep -ac "started threadtest" "$LOG")
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "threadtest group"; sleep 4
monitor "sendkey ctrl-c"
wait_session_end $WANT 15; result $? "threadtest group ends on ^C"
[ "$(grep -ac "started threadtest" "$LOG")" = "$((STARTED_BEFORE + 2))" ]; result $? "it started two programs of its own"
sleep 2; type_cmd "meminfo"; sleep 3
FRAMES_AFTER=$(grep -a "meminfo: frames_free=" "$LOG" | tail -1 | grep -aoE 'frames_free=[0-9]+' | cut -d= -f2)
[ -n "$FRAMES_BEFORE" ] && [ "$FRAMES_BEFORE" = "$FRAMES_AFTER" ]
result $? "^C ended them too (all their memory is back: $FRAMES_BEFORE -> $FRAMES_AFTER)"

# 4. mount puts every partition of the second disk under /mount; umount
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

# 5. per-process kernel stacks and SDK pages are handed back when a program
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

# 6. console still alive
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
