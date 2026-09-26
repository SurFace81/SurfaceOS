#!/bin/bash
# Build a test disk image (GPT, see tools/mkimg.py). Shared by the qemu_*
# scripts.
#
# Usage: make_test_image.sh <image> "<app1> <app2> ..." [size_mib]
#
# The kernel and its dependencies are built first; the image is populated
# via tools/mkimg.py (pyfatfs), no sudo.
set -e

cd "$(dirname "$0")/.."

IMG="$1"
APPS="$2"
SIZE="${3:-64}"

make bin/boot/efi/BOOTX64.EFI bin/kernel/kernel.bin bin/kernel/data/stdfont.fnt >/dev/null
for a in $APPS; do make bin/apps/$a.bin >/dev/null; done

rm -f "$IMG"

APP_BINS=""
for a in $APPS; do APP_BINS="$APP_BINS bin/apps/$a.bin"; done

python3 tools/mkimg.py "$IMG" \
    bin/boot/efi/BOOTX64.EFI \
    bin/kernel/kernel.bin \
    bin/kernel/data/stdfont.fnt \
    $APP_BINS \
    --size="$SIZE" >/dev/null
