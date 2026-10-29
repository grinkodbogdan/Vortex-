# Memory budget

**Target: 10–15 MB of userspace RAM with the phone idle on the home screen.**
This is measured as the sum of PSS over all processes (`tools/memreport.sh`).
Kernel memory is not included, and neither is a web app while one is running.

The phone has 512 MB. The budget exists so that nearly all of it is left for
the app the user opens. A KaiOS app runs in WebKit, and no web engine fits in
15 MB. WebKit typically needs 60–150 MB per app. It is started on demand and
fully torn down on exit, so it costs nothing at idle.

## Idle budget (estimates for the device; shell measured on host)

| Process | PSS (approx.) | Notes |
|---|---|---|
| runit + runsvdir + runsv×3 + svlogd | ~1.0 MB | |
| **vortex-shell** | **~1.0–1.5 MB** | Static musl binary. On the glibc host build, RssAnon measured 996 KB. |
| qrtr-ns, rmtfs, pd-mapper, tqftpserv | ~2.0 MB | Needed for the modem and Wi-Fi firmware on MSM8909 |
| dbus-daemon + ofono | ~3.5 MB | Telephony. Can be disabled. |
| wpa_supplicant + dhcpcd | ~4.0 MB | Only while Wi-Fi is on |
| udevd | 0 | Exits after boot (`VORTEX_UDEV_RESIDENT=0`) |
| getty | 0 | No VTs. The serial getty only runs with `VORTEX_SERIAL_GETTY=1`. |
| **Total** | **~7.5 MB** idle, **~11.5 MB** with Wi-Fi on | |

These are estimates, apart from the shell's host measurement. Confirm them on
the phone with `tools/memreport.sh`.

## How the shell stays small

* There is one 240×320×4 back buffer (300 KB). The framebuffer is mmapped.
  The atlas size is printed at start-up (`vortex: font atlas …` in the
  shell log).
* Nuklear uses a fixed 128 KB arena (`nk_init_fixed`) and never mallocs after
  start-up.
* The font atlas is 128 KB (512×256, alpha only). It holds four sizes: body
  text with Latin and Cyrillic, two UI sizes with Latin-1 only, and a
  digits-only clock. The glyph count stays under the 1000 at which Nuklear
  doubles the atlas width. The TTF is freed after baking.
* Icons are 44×44 ARGB, about 7.7 KB each. At most 24 are cached and they are
  decoded lazily. The PNG decoder's buffers are freed right away.
* The store database (116 apps, 107 KB of JSON) becomes about 62 KB of
  records. The JSON text and DOM are freed straight after parsing.
* Before a web app starts, the store records and icons are dropped.
* The shell has no threads and no GPU stack, and it links only libc and libm.
  It is built `-Os`, static and with `--gc-sections`. The x86-64 host
  build is about 180 KB.

## Measuring

```sh
tools/memreport.sh        # per-process PSS table, total, zram stats
tools/memreport.sh -t     # total only, in KiB (for scripts)
```

The **System** screen in the shell also shows MemAvailable and the shell's own
RSS.

## Leaving room for apps

* zram swap is sized to 40% of RAM with lz4. Compressed WebKit heaps make room
  for more than RAM alone allows.
* WebKit uses `--web-mem-limit=180` (MiB) to start shedding caches.
  `--doc-viewer` cuts resource caching. `Malloc=1` disables bmalloc's
  pre-reserved pools.
* OOM priorities: web app +500, shell −900. If something must die, it is the
  app, and the shell repaints when the app exits.
