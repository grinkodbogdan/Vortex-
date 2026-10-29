# Architecture

```
            ┌──────────────────────────── userspace ─────────────────────────────┐
 keypad ──► │ evdev ──► vortex-shell (Nuklear, fbdev)                            │
            │             │  home · launcher · Banana Store · settings · dialer  │
            │             ├─ curl ──────────────► store-db data.json             │
            │             ├─ vortex-kai-install ► /var/lib/vortex/apps/<id>/     │
            │             ├─ vortex-kai-run ────► cog (WPE WebKit, DRM/KMS)      │
            │             │                         └─ app://<id>/ + kaios-shim  │
            │             ├─ vortex-wifi ───────► wpa_supplicant, dhcpcd         │
            │             └─ vortex-call ───────► oFono ─► QRTR modem           │
            │ runit (Void) · zram swap · udev only at boot                       │
            └────────────────────────────────────────────────────────────────────┘
              Linux (msm8916-mainline, MSM8909) · lk2nd · Nokia 6300 4G
```

## vortex-shell

`shell/src/`. This is the only process that stays resident and draws.

| File | Role |
|---|---|
| `main.c` | Event loop. It sleeps in `poll()` until a key, a child process or the next minute. |
| `platform_linux.c` | `/dev/fb0` (RGB565 or XRGB8888) and evdev input. Also blanks the panel and hands the display to apps. |
| `platform_headless.c` | In-memory display driven by a key script. Used by the tests and for screenshots. |
| `gfx.c` | Software rasteriser for Nuklear's draw commands, with antialiased rounded rects, circles, lines and text. It skips frames that are identical to the last one. |
| `ui.c` | The screens. Focus moves by keypad, so each screen keeps its own selection and draws onto the Nuklear window canvas. |
| `apps.c` | Built-in screens plus KaiOS apps found under `apps_dir`. PNG icons are decoded on demand, with an LRU of 24 icons. |
| `store.c` | BananaHackers `store-db` v3 client. The JSON is parsed into compact records and then freed. |
| `json.c` | Two-pass JSON parser. Each document takes one allocation. |
| `t9.c` | Multi-tap text entry, the KaiOS way (abc / Abc / 123). |
| `proc.c`, `sysinfo.c`, `util.c` | Child processes, battery/memory/backlight, config and keymap. |

Memory rules the shell follows:

* Nuklear runs with `nk_init_fixed()` on a static 128 KB buffer, so no UI
  allocation happens after start-up.
* The TTF is read once and baked at four sizes into a single alpha atlas.
  The clock size holds digits only. The TTF and the baking scratch memory are
  freed afterwards.
* Anything that can be reloaded is dropped before a web app starts. That
  includes the store records and the decoded icons, followed by
  `malloc_trim()` on glibc.
* The shell never redraws when nothing changed. Even after a redraw, it only
  writes to the framebuffer if the command list hash differs from the last
  frame.

The look ("steel night") is the Vortex desktop scaled down to a phone: a
navy-to-steel gradient wallpaper, translucent dark "glass" panels with thin
borders, azure selection bars, letter-spaced cyan section titles, and a
status bar with battery, RAM use and a bold clock. The home screen carries a
SYSTEM INFO card. The theme lives in the `T` table and the `wallpaper()`,
`glass()`, `highlight()`, `caps()` and `meter()` helpers at the top of
`ui.c`.

Keys work as on KaiOS:

* The soft keys run the actions labelled at the bottom of the screen.
* OK selects. Back goes up one level.
* End goes home, closes the running app, or hangs up a call. Holding End
  opens the power menu.
* Pressing a digit on the home screen opens the dialer.

## KaiOS app runtime

`runtime/` handles installing and running apps.

`vortex-kai-install` accepts both the OmniSD/BananaHackers layout (an outer zip
holding `application.zip` and `metadata.json`) and plain app zips. It unpacks
the app to `apps_dir/<id>` and injects `/vortex-kaios-shim.js` as the first
script of every HTML page. It builds the new copy inside `apps_dir` and then
renames it into place, so a reinstall or update is atomic. App IDs are
restricted to `[A-Za-z0-9._-]`.

`vortex-kai-run` starts Cog, a single-view WPE WebKit browser:

* `--dir-handler=app:<dir>` serves the app as `app://<id>/`. Absolute paths
  like `/js/app.js`, which KaiOS apps rely on, resolve inside the app.
* `--disable-web-security=true` stands in for KaiOS `systemXHR`/`mozSystem`.
  Apps are user-installed and local, much like privileged apps on KaiOS.
* Each app gets its own `XDG_DATA_HOME`/`XDG_CACHE_HOME`. That keeps its
  `localStorage` and IndexedDB separate, like origins on KaiOS.
* `--web-mem-limit` and `--doc-viewer` keep WebKit's caches small.
  `Malloc=1` uses the system allocator instead of bmalloc.
* The app's `oom_score_adj` is 500 and the shell's is −900. Under memory
  pressure the app is killed, never the shell.
* `packaging/void/srcpkgs/cog/patches/quit-on-window-close.patch` makes
  `window.close()` exit Cog. KaiOS apps quit that way, and the shim calls it
  on an unhandled Backspace.

While an app runs, the shell stops drawing and keeps reading evdev. It does
this only to catch End, which kills the app's process group. When the app
exits, the shell repaints.

See [kaios-apps.md](kaios-apps.md) for what the shim emulates.

## System

* **Init:** runit from Void. The only enabled services are `vortex-shell`,
  `wpa_supplicant` and `dhcpcd`. The two Wi-Fi services have `down` files, so
  they cost nothing until the user turns Wi-Fi on.
* **udev:** runs only for coldplug. `core-services/90-vortex.sh` stops the
  daemon after Void's `02-udev.sh` has settled the devices. The udev database
  in `/run/udev` stays behind for libinput.
* **Firmware:** `core-services/01-vortex-firmware.sh` mounts the stock
  `modem` and `persist` partitions read-only before coldplug and links their
  blobs into `firmware_class.path`.
* **Icons:** built-in apps (Banana Store, Phone, Settings, System) get
  vector icons drawn from Nuklear primitives (`icon_*()` in `ui.c`), so they
  need no image files or icon memory. KaiOS apps use their manifest PNGs.
* **Memory:** zram swap sized to about 40% of RAM with lz4, and
  `vm.swappiness=100` (`/etc/sysctl.d/90-vortex.conf`).
* **Privileges:** the shell runs as user `vortex` in the `video`, `input` and
  `audio` groups. Only `sv`, `poweroff` and `reboot` are allowed through doas
  (`/etc/doas.conf`).
* **Kernel:** msm8916-mainline with postmarketOS' armv7 config, plus
  `kernel/vortex.config`. That fragment adds fbdev emulation and zram,
  removes VTs, and builds in what is needed to boot without an initramfs.
