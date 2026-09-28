#!/bin/bash
# fsck_clean.sh <fat volume> <log>: fsck.fat -n of an extracted FAT volume.
# Exit 0 when it finds nothing wrong but the dirty bit - which every test
# image has, since QEMU is stopped without an unmount (the verify pass even
# checks that the kernel reports it).
fsck.fat -n "$1" >"$2" 2>&1 && exit 0
! grep -vE "^fsck\.fat |^$|Dirty bit is set|Automatically removing dirty bit|Leaving filesystem unchanged|files, [0-9]+/[0-9]+ clusters" "$2" | grep -q .
