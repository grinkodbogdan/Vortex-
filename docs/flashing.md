# Flashing Vortex on the Nokia 6300 4G

> ⚠️ **Experimental, not yet booted on real hardware.** Every piece was
> built and checked on a PC: the shell's ARM binary passes its tests under
> emulation, the boot image and the panel firmware were verified by
> decoding them, and the same software stack runs in the Vortex VM. But this
> exact image has never run on a 6300 4G. Expect to need the serial
> console. Make full backups first; you can always restore KaiOS from them.

## What's in the package

| File | Goes to | Purpose |
|---|---|---|
| `boot-partition.img` | `boot` partition (EDL) | lk2nd bootloader with the Vortex kernel + initramfs 512 KiB after it |
| `userdata.img` | `userdata` partition (EDL) | Vortex root filesystem (ext4, label `vortex`). Replaces KaiOS user data. |
| `vortex-sdcard.img` | microSD card | Alternative: the same root filesystem on a card, so internal storage stays untouched |
| `lk2nd.img`, `vortex-boot.img`, `vortex-rootfs.simg` | | The same pieces separately, for lk2nd's fastboot |
| `SHA256SUMS` | | Check with `sha256sum -c SHA256SUMS` |

The boot image's initramfs mounts whichever partition is labelled
`vortex`, so the root filesystem can live on the SD card or in `userdata`.

The kernel is postmarketOS v24.06's `linux-postmarketos-qcom-msm8916` 6.6,
which covers MSM8909, with the `qcom-msm8909-nokia-leo` device tree. Kernels
6.11 and later leave this phone's panel dark. The bootloader is lk2nd's
official `msm8909` build (23.1), which knows the 6300 4G (`nokia,leo`).

> alpha1 used the wrong lk2nd (`msm8916`). On the 6300 4G it does not start,
> and the phone falls back to Nokia's locked fastboot (`fastboot boot` then
> answers "unknown command"). Re-flash `boot` from alpha2 or later over EDL.

## You need

* A PC with [`edl`](https://github.com/bkerler/edl) (Qualcomm EDL/firehose
  client), plus `fastboot` (optional).
* The **Nokia 6300 4G firehose programmer**, linked from the BananaHackers
  wiki.
* A way into EDL mode. On the 6300 4G the usual route is to enable debug
  mode (`*#*#33284#*#*`), connect USB and run `adb reboot edl`. The
  BananaHackers wiki also lists key combinations and the test-point method.

## 1. Back up (do not skip)

```sh
edl --loader=<6300-firehose>.mbn rl backup/ --genxml     # every partition
```

At minimum keep `boot`, `modem`, `persist` and `userdata`. Vortex reads the
Wi-Fi and modem firmware from `modem` and `persist` at boot and never writes
to them.

## 2a. Install to internal storage (replaces KaiOS)

```sh
edl --loader=<6300-firehose>.mbn w boot boot-partition.img
edl --loader=<6300-firehose>.mbn w userdata userdata.img
edl --loader=<6300-firehose>.mbn reset
```

## 2b. Or keep KaiOS's data and use a microSD card

```sh
sudo dd if=vortex-sdcard.img of=/dev/sdX bs=4M conv=fsync   # sdX = the card reader
edl --loader=<6300-firehose>.mbn w boot boot-partition.img
```

Put the card in the phone and power it on. Only `boot` changes. To go back
to KaiOS, restore your `boot` backup.

## 3. First boot

* lk2nd draws "lk2nd-msm8909 23.1" over the KaiOS logo and starts the
  Vortex kernel. A few seconds later the Vortex logo appears, and the USB
  debug console and adb become available on the PC.
* **The first boot takes several minutes.** The image was put together on a
  PC, so the phone now runs every package's setup step once
  (`/var/log/vortex-firstboot.log`). The boot logo says *"Setting up Vortex
  (first boot only)"*.
* Then the Vortex home screen appears. Later boots go straight to it.

Keys: the soft keys are the left/right labels, OK selects, C goes back,
red End goes home (hold it for the power menu), and green Call opens the
dialer.

## Debugging

* **adb:** Vortex runs an adb daemon on USB (alongside the serial console):
  `adb devices`, `adb shell`, `adb push`/`pull`, `adb reboot` (also
  `reboot bootloader` for lk2nd's fastboot and `reboot edl`). There is no
  authorisation prompt. For adb over Wi-Fi, run `touch /etc/vortex/adb-tcp`
  and `sv restart adbd`, then `adb connect PHONE-IP:5555` (anyone on the
  network gets a root shell then). To turn adb off: `sv down adbd`.
* **A KaiOS app does not open** ("App failed to start"): each launch logs to
  `/var/cache/vortex/logs/APP_ID.log` (the previous run is kept as `.old`).
* **No sound:** the sound card ("msm8909") appears once the modem has started,
  which can take a minute or two after boot. `cat /var/log/audio/current`
  says whether the drivers loaded and the mixer was set, `aplay -l` lists the
  card, and `speaker-test -c2 -twav -l1` plays a test. Audio is new in this
  release (device tree in `tools/phone/dts/`).
* **Calls and SMS** (Phone and Messages): ModemManager talks to the modem;
  `cat /var/log/modem/current` shows what the watcher did, `mmcli -m any`
  the modem's state (a SIM PIN must be entered with
  `mmcli -i any --pin=1234` for now).  Messages are kept in
  `/var/lib/vortex/sms/messages.tsv`.  Call audio and the microphone need the
  patched sound module (built with the image).
* **Torch:** assumed to be on GPIO 31 like Qualcomm's reference boards;
  if the Torch app says "No torch on this phone" or nothing lights up, that
  guess was wrong.
* **Camera:** not supported (no MSM8909 camera driver in Linux).
* **Bluetooth:** Settings → Bluetooth switches it, Bluetooth devices scans and
  connects (OK on a device pairs and connects it). From a shell:
  `vortex-radio bt on|off|status|scan|connect MAC|forget MAC`. Connected
  Bluetooth headphones get the sound automatically (bluez-alsa) and the
  phone takes it back when they disconnect; `vortex-audio output` says
  where sound goes.
* **Wi-Fi does not come on:** Settings shows "No Wi-Fi hardware found" when
  the Wi-Fi chip did not start. `dmesg | grep -i -e wcn -e pronto -e firmware`
  shows why (the firmware comes from the phone's `modem` partition).

> alpha2 did not load the charger driver (`pm8916-lbc`) in its initramfs.
> That driver tells the USB controller that a cable is plugged in, so the
> debug console never appeared, and the screen kept the lk2nd/KaiOS image.
> alpha3 loads it, along with the panel driver.

* **Serial console:** `ttyMSM0` at 115200 baud. The test points are in the
  board schematics linked from the postmarketOS wiki. Build with
  `VORTEX_SERIAL_GETTY=1` to get a login there. If no `vortex` partition is
  found, the initramfs drops to a shell on that console.
* **Wrong keys:** run `evtest` and adjust `/etc/vortex/keymap.conf`.
* **lk2nd fastboot:** lk2nd enters fastboot when it cannot boot. Then
  `fastboot flash boot vortex-boot.img` replaces only the kernel, and
  `fastboot flash userdata vortex-rootfs.simg` replaces only the root
  filesystem.
* Known open problems are listed in
  [device-nokia-6300-4g.md](device-nokia-6300-4g.md).

## Restoring KaiOS

```sh
edl --loader=<6300-firehose>.mbn w boot backup/boot.bin
edl --loader=<6300-firehose>.mbn w userdata backup/userdata.bin   # if you used 2a
```
