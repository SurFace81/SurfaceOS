#!/bin/bash
# Build a test disk image (GPT, see tools/mkimg.py). Shared by the qemu_*
# scripts.
#
# Usage: make_test_image.sh <image> "<app1> <app2> ..." [size_mib]
#
# An app is src/apps/<name>; "tcc" is the port, with its files
# (/files/tcc) and the /demo project.
#
# The kernel and its dependencies are built first; the image is populated
# via tools/mkimg.py (pyfatfs), no sudo.
set -e

cd "$(dirname "$0")/.."

IMG="$1"
APPS="$2"
SIZE="${3:-64}"

make bin/boot/efi/BOOTX64.EFI bin/kernel/kernel.bin bin/kernel/data/stdfont.fnt bin/sfos/cmd.bin >/dev/null
APP_BINS=""
TREES=""
for a in $APPS; do
    if [ "$a" = tcc ]; then
        make bin/ports/tcc.bin bin/ports/tcc/files/.stamp >/dev/null
        APP_BINS="$APP_BINS bin/ports/tcc.bin"
        TREES="--tree=bin/ports/tcc/files:/files/tcc --tree=ports/tcc/demo:/demo"
    else
        make bin/apps/$a.bin >/dev/null
        APP_BINS="$APP_BINS bin/apps/$a.bin"
    fi
done

rm -f "$IMG"

python3 tools/mkimg.py "$IMG" \
    bin/boot/efi/BOOTX64.EFI \
    bin/kernel/kernel.bin \
    bin/kernel/data/stdfont.fnt \
    bin/sfos/cmd.bin \
    $APP_BINS \
    $TREES \
    --size="$SIZE" >/dev/null
