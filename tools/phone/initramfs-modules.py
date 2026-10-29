#!/usr/bin/env python3
"""Copy kernel modules and their dependencies into an initramfs.

    initramfs-modules.py MODULE_DIR DEST NAME...

MODULE_DIR is /lib/modules/<release> of the kernel package.  Modules are
written to DEST uncompressed (busybox insmod cannot read .zst), and
DEST/load-order lists them dependencies first.  Dependencies come from each
module's own "depends=" modinfo field, because the postmarketOS kernel
package ships an empty modules.dep (depmod runs on the device).
"""
import os
import subprocess
import sys


def norm(name):
    for ext in (".zst", ".xz", ".gz"):
        if name.endswith(ext):
            name = name[: -len(ext)]
    if name.endswith(".ko"):
        name = name[:-3]
    return name.replace("-", "_")


def read(path):
    if path.endswith(".zst"):
        return subprocess.run(["zstd", "-qdc", path], check=True, capture_output=True).stdout
    if path.endswith(".xz"):
        return subprocess.run(["xz", "-dc", path], check=True, capture_output=True).stdout
    with open(path, "rb") as f:
        return f.read()


def depends(data):
    i = data.find(b"\0depends=")
    if i < 0:
        return []
    v = data[i + 9 : data.index(b"\0", i + 9)].decode()
    return [norm(d) for d in v.split(",") if d]


def main():
    mdir, dest, names = sys.argv[1], sys.argv[2], sys.argv[3:]
    index = {}
    for root, _, files in os.walk(os.path.join(mdir, "kernel")):
        for f in files:
            if ".ko" in f:
                index[norm(f)] = os.path.join(root, f)
    os.makedirs(dest, exist_ok=True)
    order, done = [], set()

    def add(name, chain=()):
        name = norm(name)
        if name in done:
            return
        if name in chain:
            sys.exit("dependency loop: " + " -> ".join(chain + (name,)))
        if name not in index:
            sys.exit("module not found: " + name)
        data = read(index[name])
        for d in depends(data):
            add(d, chain + (name,))
        with open(os.path.join(dest, name + ".ko"), "wb") as f:
            f.write(data)
        done.add(name)
        order.append(name)

    for n in names:
        add(n)
    with open(os.path.join(dest, "load-order"), "w") as f:
        f.write("\n".join(order) + "\n")
    print(" ".join(order))


if __name__ == "__main__":
    main()
