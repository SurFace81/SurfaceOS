#!/bin/bash
# Build a test disk image. Shared by qemu_exec_test.sh / qemu_interact.sh /
# qemu_matrix.sh so the layout logic exists exactly once.
#
# Usage: make_test_image.sh <image> "<app1> <app2> ..." [layout] [size_mib] [sector]
#   layout: superfloppy | mbr | gpt (default gpt)
#   sector: 512 | 4096 (4096 only with layout=superfloppy)
#
# The kernel and its dependencies are built first; the image is populated
# via tools/mkimg.py (pyfatfs), no sudo.
set -e

cd "$(dirname "$0")/.."

IMG="$1"
APPS="$2"
LAYOUT="${3:-gpt}"
SIZE="${4:-64}"
SECTOR="${5:-512}"

make bin/boot/efi/BOOTX64.EFI bin/kernel/kernel.bin \
     bin/kernel/data/stdfont.fnt bin/boot/bios/stub.bin >/dev/null
for a in $APPS; do make bin/apps/$a.bin >/dev/null; done

rm -f "$IMG"

APP_BINS=""
for a in $APPS; do APP_BINS="$APP_BINS bin/apps/$a.bin"; done

python3 tools/mkimg.py "$IMG" \
    bin/boot/efi/BOOTX64.EFI \
    bin/kernel/kernel.bin \
    bin/kernel/data/stdfont.fnt \
    $APP_BINS \
    --layout="$LAYOUT" --size="$SIZE" --sector-size="$SECTOR" \
    --bios-stub=bin/boot/bios/stub.bin >/dev/null
