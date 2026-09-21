#!/usr/bin/env python3
"""Build a bootable SurfaceOS disk image (no sudo required).

Usage:
  mkimg.py <image> <BOOTX64.EFI> <kernel.bin> <stdfont.fnt> [apps...]
      [--layout superfloppy|mbr|gpt] [--size MiB] [--bios-stub stub.bin]

The image is created from scratch:
  superfloppy  FAT32 over the whole disk; the optional BIOS stub is written
               to LBA 0 and 6 so a legacy BIOS boot still works. UEFI reaches
               the kernel through /EFI/Boot/BOOTX64.EFI. (The stub's BPB
               defines the geometry; pyfatfs follows it - this is the layout
               the kernel shipped with.)
  mbr          one FAT32 partition (type 0x0C) via sfdisk, formatted with
               mkfs.fat --offset, UEFI-only.
  gpt          one EFI System Partition (EF00) via sgdisk, formatted with
               mkfs.fat --offset, UEFI-only. This is the default and matches
               how a real USB stick is laid out.

Applications land in /bin/<lowercase name> without an extension, through
LFN; the boot files are /EFI/Boot/BOOTX64.EFI, /KERNEL.BIN and /FONT.FNT at
the volume root (the UEFI loader and the kernel expect them there). The
test tree /home, /tmp and /test/long directory name/ is created too.

Requires: mkfs.fat, and sfdisk (mbr) / sgdisk (gpt) on PATH; pyfatfs.
"""
import os
import subprocess
import sys

from pyfatfs.PyFatFS import PyFatFS

# Where the FS starts, per layout, in 512-byte sectors. Filled during build so
# callers (and a final report) know the offset pyfatfs used.
FS_OFFSET_SECTORS = 0


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                          stderr=subprocess.STDOUT, **kw)


def first_partition_start(image):
    """Byte offset of the first partition, read back from the table we made."""
    out = subprocess.run(["sfdisk", "-J", image], check=True,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    import json
    parts = json.loads(out.stdout)["partitiontable"]["partitions"]
    if not parts:
        raise SystemExit("mkimg: no partition created")
    # sfdisk reports 'start' in sectors of the disk's sector size (512).
    return parts[0]["start"] * 512


def build_superfloppy(image, size_mib, stub, sector_size=512):
    global FS_OFFSET_SECTORS
    run(["dd", "if=/dev/zero", "of=" + image, "bs=1M",
         "count=%d" % size_mib, "status=none"])
    if sector_size != 512:
        run(["mkfs.fat", "-F32", "-S", "%d" % sector_size, image])
    else:
        run(["mkfs.fat", "-F32", image])

    # The legacy BIOS stub is deliberately NOT written. It only prints "use
    # UEFI" and halts (there is no stage2), and overwriting LBA 0 with it
    # replaced the mkfs.fat BPB with a hardcoded geometry whose bogus
    # TotalSectors32 (~8 GiB) did not match the image: the kernel coped by
    # clamping, but fsck.fat on the host walked off the end. UEFI (QEMU and
    # real hardware) boots /EFI/Boot/BOOTX64.EFI through the firmware's own
    # FAT driver and needs no stub, so the volume stays a clean FAT32.
    FS_OFFSET_SECTORS = 0
    return 0


def build_mbr(image, size_mib):
    global FS_OFFSET_SECTORS
    run(["dd", "if=/dev/zero", "of=" + image, "bs=1M",
         "count=%d" % size_mib, "status=none"])
    # One partition, FAT32 LBA (0x0c), spanning the disk from the default
    # 2048-sector start. `,,c` = start default, end-of-disk, type c.
    subprocess.run(["sfdisk", image], input=b",,c\n", check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    off_bytes = first_partition_start(image)
    off_sectors = off_bytes // 512
    run(["mkfs.fat", "-F32", "--offset=%d" % off_sectors, image])
    FS_OFFSET_SECTORS = off_sectors
    return off_bytes


def build_gpt(image, size_mib):
    global FS_OFFSET_SECTORS
    run(["dd", "if=/dev/zero", "of=" + image, "bs=1M",
         "count=%d" % size_mib, "status=none"])
    # sgdisk: new GPT, one partition filling the disk, type EF00 (ESP).
    run(["sgdisk", "-o", "-n", "1:0:0", "-t", "1:ef00", image])
    off_bytes = first_partition_start(image)
    off_sectors = off_bytes // 512
    run(["mkfs.fat", "-F32", "--offset=%d" % off_sectors, image])
    FS_OFFSET_SECTORS = off_sectors
    return off_bytes


def populate(image, off_bytes, efi, kernel, font, apps):
    fs = PyFatFS(image, offset=off_bytes, read_only=False)

    for d in ("/EFI", "/EFI/Boot", "/bin", "/dev", "/home", "/tmp",
              "/test", "/test/long directory name"):
        try:
            fs.makedir(d)
        except Exception:
            pass  # already exists

    def put(path, data):
        fs.writebytes(path, data)

    with open(efi, "rb") as f:
        put("/EFI/Boot/BOOTX64.EFI", f.read())
    with open(kernel, "rb") as f:
        put("/KERNEL.BIN", f.read())
    with open(font, "rb") as f:
        put("/FONT.FNT", f.read())

    # Applications: /bin/<lowercase basename>, no extension (LFN).
    for app in apps:
        base = app.rsplit("/", 1)[-1]
        if base.endswith(".bin"):
            base = base[:-4]
        with open(app, "rb") as f:
            put("/bin/" + base.lower(), f.read())

    # A test tree exercising LFN, spaces and nesting (fstest reads these).
    put("/test/long directory name/file with spaces.txt",
        b"a file inside a long directory name, with spaces\n")
    put("/test/hello.txt", b"hello from the test tree\n")

    fs.close()


def fsck_repair(image, off_bytes):
    """Fix what fsck.fat considers wrong (pyfatfs leaves an inaccurate free
    cluster count in FSInfo). The stage-3 acceptance criterion is a clean
    `fsck.fat -n` on the host, so repair it once, at build time.

    For a partitioned image the volume is extracted, repaired and written
    back - fsck.fat has no offset option and losetup needs sudo.
    """
    if off_bytes == 0:
        subprocess.run(["fsck.fat", "-a", "-w", image],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return

    tmp = image + ".part.fat"
    with open(image, "rb") as src, open(tmp, "wb") as dst:
        src.seek(off_bytes)
        while True:
            chunk = src.read(1 << 20)
            if not chunk:
                break
            dst.write(chunk)
    subprocess.run(["fsck.fat", "-a", "-w", tmp],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(image, "r+b") as dst, open(tmp, "rb") as src:
        dst.seek(off_bytes)
        while True:
            chunk = src.read(1 << 20)
            if not chunk:
                break
            dst.write(chunk)
    os.remove(tmp)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = {}
    for a in argv[1:]:
        if a.startswith("--"):
            if "=" in a:
                k, v = a[2:].split("=", 1)
                opts[k] = v
            else:
                opts[a[2:]] = True

    if len(args) < 4:
        print(__doc__)
        return 1

    image, efi, kernel, font = args[0:4]
    apps = args[4:]

    layout = opts.get("layout", "gpt").lower()
    size_mib = int(opts.get("size", "64"))
    stub = opts.get("bios-stub")
    sector_size = int(opts.get("sector-size", "512"))

    if sector_size not in (512, 4096):
        print("mkimg: --sector-size must be 512 or 4096")
        return 1
    if sector_size == 4096 and layout != "superfloppy":
        # sfdisk/sgdisk write 512-based tables; a partitioned 4K image needs
        # a 4K-aware partitioner. The kernel-side 4K path is tested with a
        # superfloppy (mkfs.fat -S 4096, UEFI-only).
        print("mkimg: --sector-size 4096 is only supported with --layout superfloppy")
        return 1

    if os.path.exists(image):
        os.remove(image)

    if layout == "superfloppy":
        off = build_superfloppy(image, size_mib, stub, sector_size)
    elif layout == "mbr":
        off = build_mbr(image, size_mib)
    elif layout == "gpt":
        off = build_gpt(image, size_mib)
    else:
        print("mkimg: unknown layout %r (superfloppy|mbr|gpt)" % layout)
        return 1

    populate(image, off, efi, kernel, font, apps)
    fsck_repair(image, off)

    print("Image built: %s (layout=%s, fs offset=%d sectors / %d bytes)"
          % (image, layout, FS_OFFSET_SECTORS, off))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
