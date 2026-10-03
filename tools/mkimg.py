#!/usr/bin/env python3
"""Build a bootable SurfaceOS disk image (no sudo required).

Usage:
  mkimg.py <image> <BOOTX64.EFI> <kernel.bin> <stdfont.fnt> <cmd.bin> [apps...] [--size MiB]

The image is GPT with one FAT32 partition filling the disk, formatted with
mkfs.fat --offset. Its type is Microsoft Basic Data (0700), not an EFI
System Partition (EF00): Windows gives an ESP no drive letter, so the
stick's files could not be reached there. Firmware boots
/EFI/Boot/BOOTX64.EFI from a removable disk's FAT partition either way.
The firmware boots /EFI/Boot/BOOTX64.EFI; the loader and the kernel expect
/sfos/KERNEL.BIN and /sfos/FONT.FNT, and the kernel starts the console of
every screen from /sfos/CMD.BIN. Programs land in /apps/<lowercase
name> without an extension, through LFN. The top-level /files, /tmp and
/mount are created too.

Requires: mkfs.fat, sgdisk and sfdisk on PATH; pyfatfs.
"""
import json
import os
import subprocess
import sys

from pyfatfs.PyFatFS import PyFatFS


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                          stderr=subprocess.STDOUT, **kw)


def build_gpt(image, size_mib):
    """Create the disk and its FAT32 partition; the partition's byte offset."""
    run(["dd", "if=/dev/zero", "of=" + image, "bs=1M",
         "count=%d" % size_mib, "status=none"])
    run(["sgdisk", "-o", "-n", "1:0:0", "-t", "1:0700", image])

    out = subprocess.run(["sfdisk", "-J", image], check=True,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    parts = json.loads(out.stdout)["partitiontable"]["partitions"]
    if not parts:
        raise SystemExit("mkimg: no partition created")
    start = parts[0]["start"]                   # in 512-byte sectors

    run(["mkfs.fat", "-F32", "--offset=%d" % start, image])
    return start * 512


def populate(image, off_bytes, efi, kernel, font, cmd, apps):
    fs = PyFatFS(image, offset=off_bytes, read_only=False)

    for d in ("/EFI", "/EFI/Boot", "/sfos", "/apps", "/files", "/tmp", "/mount"):
        fs.makedir(d)

    def put(path, src):
        with open(src, "rb") as f:
            fs.writebytes(path, f.read())

    put("/EFI/Boot/BOOTX64.EFI", efi)
    put("/sfos/KERNEL.BIN", kernel)
    put("/sfos/FONT.FNT", font)
    put("/sfos/CMD.BIN", cmd)

    for app in apps:
        base = app.rsplit("/", 1)[-1]
        if base.endswith(".bin"):
            base = base[:-4]
        put("/apps/" + base.lower(), app)

    fs.close()


def fsck_repair(image, off_bytes):
    """pyfatfs leaves an inaccurate free-cluster count in FSInfo; fix it once,
    at build time, so the image starts out clean for fsck.fat. fsck.fat has
    no offset option and losetup needs sudo, so the volume is extracted,
    repaired and written back."""
    tmp = image + ".part.fat"
    with open(image, "rb") as src, open(tmp, "wb") as dst:
        src.seek(off_bytes)
        while chunk := src.read(1 << 20):
            dst.write(chunk)
    subprocess.run(["fsck.fat", "-a", "-w", tmp],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(image, "r+b") as dst, open(tmp, "rb") as src:
        dst.seek(off_bytes)
        while chunk := src.read(1 << 20):
            dst.write(chunk)
    os.remove(tmp)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    size_mib = 64
    for a in argv[1:]:
        if a.startswith("--size="):
            size_mib = int(a.split("=", 1)[1])
        elif a.startswith("--"):
            print("mkimg: unknown option %s" % a)
            return 1

    if len(args) < 5:
        print(__doc__)
        return 1

    image, efi, kernel, font, cmd = args[0:5]
    apps = args[5:]

    if os.path.exists(image):
        os.remove(image)

    off = build_gpt(image, size_mib)
    populate(image, off, efi, kernel, font, cmd, apps)
    fsck_repair(image, off)

    print("Image built: %s (GPT, FAT32 at %d bytes)" % (image, off))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
