#!/usr/bin/env python3
"""Drive a headless Vortex VM (tools/vm/run.sh with VORTEX_VM_HEADLESS=1).

  ctl.py WORKDIR shot out.png            screenshot (PPM, or PNG if Pillow/convert exists)
  ctl.py WORKDIR keys f1 down ret ...    press phone keys (QEMU key names; 'wait:MS' pauses)
  ctl.py WORKDIR hold f4 [ms]            hold a key
  ctl.py WORKDIR sh 'command'            run a command in the serial root shell, print output
"""
import os
import socket
import subprocess
import sys
import time


def monitor(work, cmd):
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.path.join(work, "monitor.sock"))
    s.settimeout(5)
    time.sleep(0.1)
    s.recv(65536)  # banner
    s.sendall((cmd + "\n").encode())
    time.sleep(0.3)
    try:
        out = s.recv(65536).decode(errors="replace")
    except socket.timeout:
        out = ""
    s.close()
    return out


def shot(work, path):
    ppm = path if path.endswith(".ppm") else path + ".ppm"
    monitor(work, "screendump " + os.path.abspath(ppm))
    time.sleep(0.5)
    if ppm != path:
        subprocess.run(["convert", ppm, path], check=True)
        os.unlink(ppm)


def keys(work, names, gap=0.25):
    # one monitor connection for the whole sequence, so repeated presses
    # land within the T9 multi-tap window like a person typing
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.path.join(work, "monitor.sock"))
    s.settimeout(0.2)
    for k in names:
        if k.startswith("wait:"):
            time.sleep(int(k[5:]) / 1000)
            continue
        s.sendall(("sendkey %s 100\n" % k).encode())
        time.sleep(gap)
        try:
            s.recv(65536)
        except socket.timeout:
            pass
    s.close()


def serial(work, command, timeout=120):
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.path.join(work, "serial.sock"))
    s.settimeout(1)
    marker = "__VX_DONE_%d__" % time.time_ns()
    s.sendall(("\n%s; echo %s $?\n" % (command, marker)).encode())
    buf, end = "", time.time() + timeout
    while time.time() < end:
        try:
            chunk = s.recv(65536).decode(errors="replace")
        except socket.timeout:
            continue
        buf += chunk
        # the marker appears once echoed and once as real output
        if buf.count(marker) >= 2 and buf.rstrip().splitlines()[-1:] and "\n" in buf.split(marker)[-1]:
            break
    s.close()
    body = buf.split(marker)
    out = body[1] if len(body) > 2 else buf
    return out.split("\n", 1)[-1].rstrip() if len(body) > 2 else buf


def main():
    work, cmd, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    if cmd == "shot":
        shot(work, args[0])
    elif cmd == "keys":
        keys(work, args)
    elif cmd == "hold":
        ms = int(args[1]) if len(args) > 1 else 1500
        monitor(work, "sendkey %s %d" % (args[0], ms))
        time.sleep(ms / 1000 + 0.5)
    elif cmd == "sh":
        print(serial(work, args[0], int(os.environ.get("VX_TIMEOUT", "120"))))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
