#!/usr/bin/env python3
"""Write an Android boot image (header v0), as lk/lk2nd boot it.

    mkbootimg.py --kernel zImage-dtb --ramdisk initramfs.gz --cmdline "..." \
        --base 0x80000000 --kernel-offset 0x8000 --ramdisk-offset 0x2700000 \
        --second-offset 0xf00000 --tags-offset 0x2500000 --pagesize 2048 -o boot.img
"""
import argparse
import hashlib
import struct


def pad(data, page):
    return data + b"\0" * ((page - len(data) % page) % page)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--kernel", required=True)
    p.add_argument("--ramdisk", required=True)
    p.add_argument("--cmdline", default="")
    p.add_argument("--name", default="vortex")
    for opt, default in (("base", 0x10000000), ("kernel-offset", 0x8000), ("ramdisk-offset", 0x1000000),
                         ("second-offset", 0xF00000), ("tags-offset", 0x100), ("pagesize", 2048)):
        p.add_argument("--" + opt, type=lambda s: int(s, 0), default=default)
    p.add_argument("-o", "--output", required=True)
    a = p.parse_args()

    kernel = open(a.kernel, "rb").read()
    ramdisk = open(a.ramdisk, "rb").read()
    cmdline = a.cmdline.encode()
    if len(cmdline) > 511:
        raise SystemExit("cmdline too long for a v0 header")

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b""):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))

    header = struct.pack(
        "<8s10I16s512s32s1024s",
        b"ANDROID!",
        len(kernel), a.base + a.kernel_offset,
        len(ramdisk), a.base + a.ramdisk_offset,
        0, a.base + a.second_offset,
        a.base + a.tags_offset,
        a.pagesize,
        0,          # header version 0
        0,          # os_version
        a.name.encode()[:15],
        cmdline,
        sha.digest(),
        b"",
    )
    with open(a.output, "wb") as f:
        f.write(pad(header, a.pagesize))
        f.write(pad(kernel, a.pagesize))
        f.write(pad(ramdisk, a.pagesize))


if __name__ == "__main__":
    main()
