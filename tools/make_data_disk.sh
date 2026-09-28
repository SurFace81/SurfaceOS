#!/bin/bash
# Build a second, non-bootable test disk: GPT with two FAT32 partitions,
# each holding /hello.txt with its partition number. `mount` tests attach
# it next to the boot image. No sudo (mkfs.fat --offset).
#
# Usage: make_data_disk.sh <image>
set -e

IMG="$1"
# FAT32 needs >= 65525 clusters: 36 MiB per partition with 512-byte clusters.
P1_MIB=40
SIZE_MIB=84

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=$SIZE_MIB status=none
sgdisk -o -n 1:0:+${P1_MIB}M -t 1:0700 -n 2:0:0 -t 2:0700 "$IMG" >/dev/null

# Start and size (in sectors) of partition N, from sfdisk's JSON dump.
part() {
    sfdisk -J "$IMG" | python3 -c "
import json, sys
p = json.load(sys.stdin)['partitiontable']['partitions'][$1 - 1]
print(p['start'], p['size'])"
}

for n in 1 2; do
    read -r start size < <(part $n)
    mkfs.fat -F32 -s 1 -n "DATA$n" --offset=$start "$IMG" $((size / 2)) >/dev/null 2>&1
    python3 - "$IMG" $((start * 512)) $n <<'PY'
import sys
from pyfatfs.PyFatFS import PyFatFS
fs = PyFatFS(sys.argv[1], offset=int(sys.argv[2]), read_only=False)
fs.writebytes("/hello.txt", ("partition %s\n" % sys.argv[3]).encode())
fs.close()
PY
done
