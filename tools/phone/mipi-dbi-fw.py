#!/usr/bin/env python3
"""Compile a panel-mipi-dbi init sequence (text) into the kernel's firmware
format.  Same format as notro/panel-mipi-dbi's mipi-dbi-cmd:

    "MIPI DBI" + 7 NUL bytes, version byte 1, then per line of the text file
    "command C [P...]" -> C, len(P), P...      "delay MS" -> 0x00, 1, MS

    mipi-dbi-fw.py OUT.bin IN.txt
"""
import sys


def compile_seq(text):
    out = bytearray(b"MIPI DBI" + b"\0" * 7 + b"\x01")
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        word, *args = line.split()
        vals = [int(a, 0) for a in args]
        if any(v < 0 or v > 255 for v in vals):
            sys.exit("line %d: value out of range" % n)
        if word == "command" and vals:
            out += bytes([vals[0], len(vals) - 1] + vals[1:])
        elif word == "delay" and len(vals) == 1:
            out += bytes([0, 1, vals[0]])
        else:
            sys.exit("line %d: cannot parse: %s" % (n, line))
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[2]) as f:
        data = compile_seq(f.read())
    with open(sys.argv[1], "wb") as f:
        f.write(data)
