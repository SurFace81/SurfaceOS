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
#     typed on screen 2 go to its own console, not to screen 1.
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
                                            # (screen 2's CMD runs it in 1g)
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck reader"
type_cmd "screens"
wait_for "sdkcheck input: child got screens" 10
result $? "keys typed on screen 2 did not reach screen 1"
wait_session_end $WANT 15

# 1b. Keys through the console protocol: in SF_CONSOLE_RAW every key comes
#     to ReadKey, Ctrl+C too; in a ReadLine, Ctrl+C ends it (SF_ABORTED).
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck keys"
wait_for "press keys, q ends" 20; result $? "sdkcheck keys switched to SF_CONSOLE_RAW"
key a
wait_for "sdkcheck key: code 30 mods 0 char 97" 10; result $? "ReadKey: a letter"
key ctrl-c
wait_for "sdkcheck key: code 46 mods 2 char 3" 10; result $? "ReadKey: Ctrl+C is a key in SF_CONSOLE_RAW"
key up
wait_for "sdkcheck key: code 200 mods 0 char 0" 10; result $? "ReadKey: an arrow"
key q
wait_session_end $WANT 15; result $? "sdkcheck keys ends on q"
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck reader"; sleep 2
key ctrl-c
wait_for "sdkcheck input: aborted" 10; result $? "Ctrl+C ends a ReadLine with SF_ABORTED"
wait_session_end $WANT 15
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck reader"; sleep 2
key ctrl-alt-c
wait_session_end $WANT 15; result $? "Ctrl+Alt+C ends a program waiting in ReadLine"
[ "$(last_status)" = "9" ]; result $? "  as SIGKILL (9)"

# 1c. Ctrl+Alt+Z pauses the programs on the screen, pressed again it lets
#     them go on; a paused program still ends on Ctrl+Alt+C.
ticks() { grep -ac "sdkcheck tick " "$LOG"; }
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck ticks"
wait_for "sdkcheck tick 3" 10; result $? "sdkcheck ticks is ticking"
key ctrl-alt-z; sleep 1
T1=$(ticks); sleep 2; T2=$(ticks)
[ "$T1" = "$T2" ]; result $? "Ctrl+Alt+Z pauses it ($T1 -> $T2 ticks)"
key ctrl-alt-z; sleep 2; T3=$(ticks)
[ "$T3" -gt "$T2" ]; result $? "pressed again, it goes on ($T2 -> $T3 ticks)"
key ctrl-alt-z; sleep 1
key ctrl-alt-c
wait_session_end $WANT 15; result $? "Ctrl+Alt+C ends a paused program"
[ "$(last_status)" = "9" ]; result $? "  as SIGKILL (9)"
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck reader"; sleep 2
key ctrl-alt-z; sleep 1; key ctrl-alt-z; sleep 1
type_cmd "after"
wait_for "sdkcheck input: child got after" 10; result $? "a pause does not cut a ReadLine short"
wait_session_end $WANT 15

# 1d. `&`: a program runs in the background on a hidden screen while the
#     console goes on; what it prints goes to a log in its data folder
#     (read back from the image at the end). Ctrl+Alt+C on screen 1 does
#     not reach it.
type_cmd "sdkcheck ticks &"
wait_for "console: background start, pid" 10; result $? "sdkcheck ticks & starts in the background"
T1=$(ticks)
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sdkcheck late"
wait_session_end $WANT 15; result $? "the console runs another program meanwhile"
key ctrl-alt-c; sleep 2
[ "$(ticks)" -gt "$T1" ]; result $? "the background program ticks on, Ctrl+Alt+C on screen 1 or not"
# Let it finish before the file tests: under QEMU's emulation a busy file
# test next to it slows down many times over (not under KVM).
wait_for "sdkcheck tick 150" 60; result $? "and runs to its end"

# 1e. Weights: on one CPU, a program on the shown screen gets twice the time
#     of one in the background (2:1), and once another screen is shown,
#     the same as it (1:1). With more CPUs the two do not share one.
if [ "${SMP:-1}" = "1" ]; then
    spin_avg() {  # spin_avg <label> <from> <to>: mean of its lines from..to
        grep -ao "sdkcheck spin $1: [0-9]*" "$LOG" | sed -n "$2,$3p" |
            awk '{ s += $4; n++ } END { print (n ? int(s / n) : 0) }'
    }
    type_cmd "sdkcheck spin bg 16 &"; sleep 1
    WANT=$(( $(sessions_ended) + 1 ))
    type_cmd "sdkcheck spin fg 9"
    wait_for "sdkcheck spin fg: " 10; sleep 4
    key alt-f2; sleep 5; key alt-f1
    wait_session_end $WANT 20
    # fg lines 2-4 ran with screen 1 shown, 7-9 with screen 2; the bg lines
    # at those times are the ones between them in the log.
    FG1=$(spin_avg fg 2 4); FG2=$(spin_avg fg 7 9)
    BG1=$(grep -ao "sdkcheck spin [a-z]*: [0-9]*" "$LOG" | awk '/fg:/{f++} /bg:/ && f>=2 && f<4 {s+=$4; n++} END {print (n?int(s/n):0)}')
    BG2=$(grep -ao "sdkcheck spin [a-z]*: [0-9]*" "$LOG" | awk '/fg:/{f++} /bg:/ && f>=7 && f<9 {s+=$4; n++} END {print (n?int(s/n):0)}')
    echo "      shown: fg $FG1 bg $BG1; screen 2 shown: fg $FG2 bg $BG2"
    [ "$BG1" -gt 0 ] && [ $((FG1 * 10 / BG1)) -ge 15 ] && [ $((FG1 * 10 / BG1)) -le 27 ]
    result $? "the program on the shown screen gets about twice the time"
    [ "$BG2" -gt 0 ] && [ $((FG2 * 10 / BG2)) -ge 7 ] && [ $((FG2 * 10 / BG2)) -le 14 ]
    result $? "with another screen shown, both get the same"
    # Let the background one finish before the file tests (under QEMU's
    # emulation a file test next to it slows down many times over).
    for i in $(seq 20); do
        [ "$(grep -ac "sdkcheck spin bg: " "$LOG")" -ge 16 ] && break; sleep 1
    done
    sleep 1
fi

# 1f. The admin right: `admin sdkcheck admin` gets Sys->Admin, disk:/ and
#     mount:/ (a plain sdkcheck checks it gets none of them).
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "admin sdkcheck admin"
wait_for "sdkcheck admin: " 30; result $? "admin sdkcheck admin finished"
grep -aE "\[FAIL\]" "$LOG" | sed 's/^/      /'
grep -aq "sdkcheck admin: [0-9]* passed, 0 failed" "$LOG"; result $? "sdkcheck admin: no failed checks"
wait_session_end $WANT 15; result $? "admin sdkcheck admin exits"

# 1g. CMD.BIN, the console of screens 2..9: the "zz" typed on screen 2
#     above waits in its line; it runs programs, handing them the keys,
#     and in the background; Ctrl+Alt+C ends its program but not it, and
#     on its own it ends too - and is started again.
key alt-f2; key ret
wait_for "zz: no such command or program" 10; result $? "CMD on screen 2 got what was typed there"
type_cmd "sdkcheck reader"; sleep 2; type_cmd "via cmd"
wait_for "sdkcheck input: child got via cmd" 10; result $? "CMD runs a program and hands it the keys"
T0=$(ticks); type_cmd "sdkcheck ticks"; sleep 2
key ctrl-alt-c; sleep 2
T1=$(ticks); sleep 1
[ "$T1" -gt "$T0" ] && [ "$(ticks)" = "$T1" ]
result $? "Ctrl+Alt+C ends CMD's program (after $((T1 - T0)) ticks)"
type_cmd "sdkcheck reader"; sleep 2; type_cmd "cmd lives"
wait_for "sdkcheck input: child got cmd lives" 10; result $? "and leaves CMD itself running"
CMDS=$(grep -ac "cmdkeeper: cmd pid" "$LOG")
key ctrl-alt-c; sleep 3
[ "$(grep -ac "cmdkeeper: cmd pid" "$LOG")" -gt "$CMDS" ]; result $? "CMD on its own ends on Ctrl+Alt+C and is started again"
type_cmd "sdkcheck reader"; sleep 2; type_cmd "new cmd"
wait_for "sdkcheck input: child got new cmd" 10; result $? "the new CMD works"
type_cmd "sdkcheck late &"
wait_for "started sdkcheck (pid" 10
grep -aq "in the background)" "$LOG"; result $? "CMD runs a program in the background"
sleep 2; key alt-f1

# 1h. Jobs: Ctrl+Alt+Z hands the keys back to the console, whose bg sends
#     the paused program to the background and fg brings it back; kill
#     ends a program by its number.
T0=$(ticks)
type_cmd "sdkcheck ticks"
sleep 3
key ctrl-alt-z; wait_for "console: screen 0 paused" 5
type_cmd "bg"
wait_for "console: bg pid" 10; result $? "bg sends the paused program to the background"
JOB=$(grep -a "console: bg pid" "$LOG" | tail -1 | grep -aoE 'pid [0-9]+' | grep -aoE '[0-9]+')
T1=$(ticks); sleep 2
[ "$(ticks)" -gt "$T1" ]; result $? "  where it goes on"
type_cmd "fg $JOB"
wait_for "console: fg pid $JOB" 10; result $? "fg brings it back"
key ctrl-alt-z; sleep 1
T1=$(ticks); sleep 2
[ "$(ticks)" = "$T1" ]; result $? "  paused again with Ctrl+Alt+Z"
type_cmd "kill $JOB"
wait_for "process: pid $JOB sdkcheck ended, status 9" 10; result $? "kill ends it by its number"

# 2. sfstest: files through the SDK (data:/, tmp:/, the sandbox). What it
#    leaves in data:/ is read back after a restart by qemu_verify.sh.
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "sfstest"
wait_for "sfstest: " 240; result $? "sfstest finished"
grep -aE "\[FAIL\]" "$LOG" | sed 's/^/      /'
grep -aq "sfstest: [0-9]* passed, 0 failed" "$LOG"; result $? "sfstest: no failed checks"
wait_session_end $WANT 15; result $? "sfstest exits"

# 3. threadtest: however a program with several threads ends, all of them
#    end - a fault in one (SIGSEGV = 11), Ctrl+Alt+C (SIGKILL = 9), or the
#    last thread leaving after the first (its status 42, wait format
#    42 << 8).
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "threadtest fault"
wait_session_end $WANT 15; result $? "threadtest fault ends"
[ "$(last_status)" = "11" ]; result $? "a fault in one thread ends the program (SIGSEGV)"
grep -aq "threadtest crashed: page fault at address 0x0, instruction at" "$LOG"
result $? "  and the kernel says so on its screen"

WANT=$(( $(sessions_ended) + 1 ))
type_cmd "threadtest spin"; sleep 3
key alt-f2; key ctrl-alt-c; key alt-f1; sleep 3    # another screen's
[ "$(sessions_ended)" -lt "$WANT" ]; result $? "Ctrl+Alt+C on screen 2 leaves screen 1's program alone"
monitor "sendkey ctrl-alt-c"
wait_session_end $WANT 15; result $? "threadtest spin ends on Ctrl+Alt+C"
[ "$(last_status)" = "9" ]; result $? "Ctrl+Alt+C ends every thread (SIGKILL)"

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

# Ctrl+Alt+C ends the programs a program started too: every program on the
# screen.
type_cmd "meminfo"; sleep 3
FRAMES_BEFORE=$(grep -a "meminfo: frames_free=" "$LOG" | tail -1 | grep -aoE 'frames_free=[0-9]+' | cut -d= -f2)
STARTED_BEFORE=$(grep -ac "started threadtest" "$LOG")
WANT=$(( $(sessions_ended) + 1 ))
type_cmd "threadtest group"; sleep 4
monitor "sendkey ctrl-alt-c"
wait_session_end $WANT 15; result $? "threadtest group ends on Ctrl+Alt+C"
# Three: the console started it, and it started two of its own.
[ "$(grep -ac "started threadtest" "$LOG")" = "$((STARTED_BEFORE + 3))" ]; result $? "it started two programs of its own"
sleep 2; type_cmd "meminfo"; sleep 3
FRAMES_AFTER=$(grep -a "meminfo: frames_free=" "$LOG" | tail -1 | grep -aoE 'frames_free=[0-9]+' | cut -d= -f2)
[ -n "$FRAMES_BEFORE" ] && [ "$FRAMES_BEFORE" = "$FRAMES_AFTER" ]
result $? "Ctrl+Alt+C ended them too (all their memory is back: $FRAMES_BEFORE -> $FRAMES_AFTER)"

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

type_cmd "sync"; sleep 3
monitor "quit"
sleep 1

# The background programs' logs, read from the image.
OFF=$(( $(sgdisk -i 1 "$IMG" | awk '/First sector/{print $3}') * 512 ))
python3 - "$IMG" "$OFF" > /tmp/exec_logs.txt 2>&1 <<'PY'
import sys
from pyfatfs.PyFatFS import PyFatFS
fs = PyFatFS(sys.argv[1], offset=int(sys.argv[2]), read_only=True)
# (Short names come back in capitals: SDKCHECK.)
d = '/files/' + [n for n in fs.listdir('/files') if n.lower() == 'sdkcheck'][0]
for name in sorted(fs.listdir(d)):
    if name.startswith('console_'):
        print(name, repr(fs.readtext(d + '/' + name)))
PY
grep -q "sdkcheck tick 150" /tmp/exec_logs.txt; result $? "the log of sdkcheck ticks & holds all it printed"

echo
echo "=== summary lines ==="
grep -aE "^cpu:|^pmm:|sdkcheck: |program (start|end)|kernel fault|app fault" "$LOG"
echo
if [ $FAILS -eq 0 ]; then echo "ALL CHECKS PASSED"; else echo "$FAILS CHECK(S) FAILED"; fi
exit $FAILS
