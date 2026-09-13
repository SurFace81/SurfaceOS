#!/usr/bin/env python3
"""Populate a FAT32 disk image with SurfaceOS boot files (no sudo required).

Usage: mkimg.py <image> <BOOTX64.EFI> <kernel.bin> <stdfont.fnt> [apps...]

The image must already exist, be formatted as FAT32 and contain the
BIOS boot stub written at sectors 0 and 6 (see tools/qemu_test.sh).
"""
import sys
from pyfatfs.PyFatFS import PyFatFS


def main(argv):
    if len(argv) < 5:
        print(__doc__)
        return 1

    image, efi, kernel, font = argv[1:5]
    apps = argv[5:]

    fs = PyFatFS(image, read_only=False)

    for d in ("/EFI", "/EFI/Boot", "/APPS"):
        try:
            fs.makedir(d)
        except Exception:
            pass  # already exists

    with open(efi, "rb") as f:
        fs.writebytes("/EFI/Boot/BOOTX64.EFI", f.read())
    with open(kernel, "rb") as f:
        fs.writebytes("/KERNEL.BIN", f.read())
    with open(font, "rb") as f:
        fs.writebytes("/FONT.FNT", f.read())
    fs.writebytes("/FILE.TXT", b"Hello from file!\n")

    for app in apps:
        name = app.rsplit("/", 1)[-1].upper()
        with open(app, "rb") as f:
            fs.writebytes("/APPS/" + name, f.read())

    fs.close()
    print("Image populated:", image)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
