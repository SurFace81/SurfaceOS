#!/bin/bash
# Run the full QEMU regression suite across every disk layout the kernel
# must handle: superfloppy, MBR, GPT (512-byte sectors) and a superfloppy
# with 4096-byte sectors. After each run the host validates the volume with
# fsck.fat where possible (partitioned images are extracted with dd first).
#
# Usage: tools/qemu_matrix.sh
set -u
cd "$(dirname "$0")/.."

FAILS=0

run_layout() {  # run_layout <label> <layout> [VAR=value...]
    local label="$1"; local layout="$2"; shift 2
    echo "==================== $label ($layout) ===================="
    if env LAYOUT="$layout" "$@" bash tools/qemu_exec_test.sh >/tmp/matrix_$label.log 2>&1; then
        echo "PASS  suite [$label]"
        # The suite leaves test_disk.img behind: validate it while fresh.
        fsck_image "$label" "$layout"
    else
        echo "FAIL  suite [$label] (see /tmp/matrix_$label.log)"
        grep -E "^(PASS|FAIL)" /tmp/matrix_$label.log | tail -25
        FAILS=$((FAILS+1))
    fi
}

fsck_image() {  # fsck_image <label> <layout>
    local label="$1"
    local layout="$2"
    local img=test_disk.img
    local part=/tmp/fsck_$label.fat

    # Extract the FAT volume: GPT/MBR partition 1 starts at sector 2048.
    if [ "$layout" = "superfloppy" ]; then
        cp "$img" "$part"
    else
        local total=$(stat -c %s "$img")
        dd if="$img" of="$part" bs=512 skip=2048 \
           count=$(( (total - 2048 * 512) / 512 )) status=none
    fi

    if fsck.fat -n "$part" >/tmp/fsck_$label.log 2>&1; then
        echo "PASS  fsck.fat clean [$label]"
    else
        echo "FAIL  fsck.fat [$label]:"
        tail -8 /tmp/fsck_$label.log | sed 's/^/      /'
        FAILS=$((FAILS+1))
    fi
    rm -f "$part"
}

run_layout sf  superfloppy
run_layout mbr mbr
run_layout gpt gpt
# 4K sectors: superfloppy only (partitioned 4K needs a 4K-aware partitioner;
# QEMU exposes it as usb-bot + scsi-hd, see qemu_exec_test.sh). The 4K FAT32
# needs >= 256 MiB to get a 32-bit TotalSectors.
run_layout 4k  superfloppy SECTOR=4096 IMG_SIZE=512

echo
if [ $FAILS -eq 0 ]; then
    echo "MATRIX: ALL LAYOUTS PASSED"
else
    echo "MATRIX: $FAILS FAILURE(S)"
fi
exit $FAILS
