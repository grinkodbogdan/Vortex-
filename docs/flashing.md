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

The kernel is postmarketOS' `linux-postmarketos-qcom-msm8916` 6.12.1, which
covers MSM8909, with the `qcom-msm8909-nokia-leo` device tree. The bootloader
is postmarketOS' prebuilt `lk2nd-msm8916` 23.1.

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

* lk2nd starts the Vortex kernel. The screen stays dark until the panel
  driver loads.
* **The first boot takes several minutes.** The image was put together on a
  PC, so the phone now runs every package's setup step once
  (`/var/log/vortex-firstboot.log`). The screen shows *"Vortex: first-boot
  setup, please wait..."*.
* Then the Vortex home screen appears. Later boots go straight to it.

Keys: the soft keys are the left/right labels, OK selects, C goes back,
red End goes home (hold it for the power menu), and green Call opens the
dialer.

## Debugging

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
