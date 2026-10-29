#!/usr/bin/env python3
"""Pack a directory into a gzip'd newc cpio archive (an initramfs), with all
files owned by root.  mkcpio.py DIR OUT.gz"""
import gzip
import os
import stat
import sys


def entry(name, st, data, ino):
    mode = st.st_mode
    hdr = "070701" + "".join("%08x" % v for v in (
        ino, mode, 0, 0, st.st_nlink if stat.S_ISDIR(mode) else 1, 0, len(data),
        0, 0, 0, 0, len(name) + 1, 0))
    out = hdr.encode() + name.encode() + b"\0"
    out += b"\0" * ((4 - len(out) % 4) % 4)
    out += data + b"\0" * ((4 - len(data) % 4) % 4)
    return out


def main(root, dst):
    blobs, ino = [], 1
    for dirpath, dirs, files in os.walk(root):
        dirs.sort()
        for n in [""] + sorted(dirs) + sorted(files):
            if n == "" and dirpath == root:
                continue
            path = os.path.join(dirpath, n) if n else dirpath
            if n in dirs:
                continue  # emitted when walked
            rel = os.path.relpath(path, root)
            st = os.lstat(path)
            if stat.S_ISLNK(st.st_mode):
                data = os.readlink(path).encode()
            elif stat.S_ISREG(st.st_mode):
                data = open(path, "rb").read()
            else:
                data = b""
            blobs.append(entry(rel, st, data, ino))
            ino += 1
    blobs.append(entry("TRAILER!!!", os.stat(root), b"", 0))
    with gzip.open(dst, "wb", compresslevel=9) as f:
        f.write(b"".join(blobs))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
