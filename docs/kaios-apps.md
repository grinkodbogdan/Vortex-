# KaiOS apps on Vortex

Vortex runs **packaged KaiOS 2.5 apps**, the kind distributed through the
BananaHackers store and as OmniSD zips. They run in WPE WebKit with a
compatibility shim. KaiOS 2.5 used Gecko 48, and WebKit is a much newer
engine, so standard HTML, CSS and JS mostly just work. What needs help is the
Mozilla/B2G-specific APIs and the key handling.

## Installing

* **From the phone:** open *Banana Store*, pick a category or search with
  T9, open an app and press *INSTALL*. Installed apps show up in the launcher.
* **From a file:** run
  `vortex-kai-install --id myapp --file myapp.zip` as user `vortex`.
* **Removing:** in the launcher, choose *Options → Uninstall*, or run
  `vortex-kai-remove myapp`. This also deletes the app's private data.

## What the shim provides (`runtime/kaios-shim.js`)

| KaiOS feature | On Vortex |
|---|---|
| Soft keys / Call / Back | F1, F2 and Menu become `SoftLeft`/`SoftRight`. Esc and BrowserBack become `Backspace` (keyCode 8). Send/F3 becomes `Call`. |
| Unhandled Backspace exits the app | Yes, through `window.close()`. Cog is patched to quit. |
| `new XMLHttpRequest({mozSystem: true})` | Works: web security is disabled for local apps |
| `navigator.mozApps.getSelf()`, `mgmt.getAll()` | Returns the app's own manifest |
| `navigator.mozSettings` | Get, set and observers. Values persist per app. |
| `localStorage`, IndexedDB | Native WebKit, separate for each app |
| `navigator.mozSetMessageHandler` / system messages | Accepted; no messages are delivered |
| `MozActivity` (share, pick, open…) | Fails with `NO_PROVIDER` |
| `navigator.getDeviceStorage` (SD card files) | Reports "unavailable" |
| `navigator.mozMobileConnections`, `mozTelephony`, `mozIccManager` | Empty or absent |
| `navigator.mozAlarms`, `mozContacts` | Empty results |
| KaiAds (`getKaiAd`) | Calls `onerror` straight away, so apps don't hang |
| Audio / video | WebKit with GStreamer (build option `media`) |

`runtime/tests/shim-test.js` checks the shim in Chromium: key mapping,
Backspace-to-exit and the API stubs.

## Apps tested

| App | Where | Result |
|---|---|---|
| BankiTube (YouTube via Invidious) | VM, 2026-10-29 | Installs, launches, renders, keys work. Search fails because every public Invidious API is down or behind anti-bot checks; the app fails the same way on KaiOS. See [vm.md](vm.md). |

## What works and what doesn't

The store labels each app with a **type**. Vortex shows a warning on the app
page for the last two rows.

| Type | Expectation |
|---|---|
| `packaged`, `privileged` | Most should work: games, readers, podcast players, utilities and network clients |
| `hosted`, `weblink` | Should work (the zip carries the launcher page) |
| `certified` | Often rely on system APIs like `webapps-manage`, `engmode`, telephony or device storage, which the shim cannot provide |
| `root` | Need KaiOS internals (adbroot, OmniBB…). These won't work and aren't needed on Vortex. |

The BananaHackers web store clients ("Store Client", "openGiraffes") install
apps through `navigator.mozApps.mgmt.import`, which Vortex doesn't have.
**The native Banana Store in the shell replaces them.** It reads the same
`store-db` database.

## Roadmap for better compatibility

* Device storage backed by `~/` or the SD card, through a small local file
  server on `app://` or a WebKit web extension
* `MozActivity` routing between installed apps (pick image, share, open URL)
* Notifications and alarms delivered by the shell
* Optional spatial navigation (`navigator.spatialNavigationEnabled`) through
  WebKit's `enable-spatial-navigation` setting

## Phone features for apps

`vortex-webview` gives the shim a native bridge
(`window.webkit.messageHandlers.vortex`), so these KaiOS/Web APIs reach the
hardware:

| API | Backed by |
|---|---|
| `<audio>`, `<video>`, Web Audio | GStreamer → ALSA (`/etc/asound.conf`: phone, or Bluetooth headphones) |
| keys (SoftLeft/Right, Call, Backspace, `*`, `#`, digits, arrows, Enter) | the keypad hwdb remap + the shim's KaiOS key names |
| `navigator.vibrate()` | the PM8909 vibration motor (force feedback) |
| `navigator.volumeManager` | `vortex-audio volume` (0-15), with a volume bar |
| `navigator.getBattery()`, `navigator.battery` | `/sys/class/power_supply` |
| `navigator.mozPower` (brightness, power off, reboot) | the backlight, `doas poweroff/reboot` |
| `navigator.getDeviceStorage()` (sdcard, music, pictures, videos) | `/var/lib/vortex/storage` (`adb push` files there) |
| `Notification` | allowed; shown as a short vibration |

Telephony, SMS, contacts and alarms are still stubs.
