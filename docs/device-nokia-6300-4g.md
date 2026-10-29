# Nokia 6300 4G (`nokia-leo`)

| | |
|---|---|
| SoC | Qualcomm Snapdragon 210 (MSM8909), 4× Cortex-A7 1.1 GHz, **armv7** |
| GPU | Adreno 304 (freedreno a3xx) |
| RAM / storage | 512 MB / 4 GB eMMC + microSD |
| Display | 240×320 TFT, GC9305 controller over SPI (`panel-mipi-dbi`) |
| Input | 12-key keypad, D-pad, 2 soft keys, call/end |
| Mainline status | postmarketOS: boots. Screen, Wi-Fi, BT, battery, mobile data and SMS work. Calls untested. ([wiki](https://wiki.postmarketos.org/wiki/Nokia_6300_4G_(nokia-leo))) |

Vortex builds on that postmarketOS work:

* **Kernel:** [msm8916-mainline/linux](https://github.com/msm8916-mainline/linux),
  tag `v6.12.1-msm8916`, with the device tree `qcom-msm8909-nokia-leo`.
* **Kernel config:** pmaports' `config-postmarketos-qcom-msm8916.armv7`,
  pinned to pmaports `690ccca9`, plus `kernel/vortex.config`.
* **Panel init sequence:** pmaports' `nokia,leo-gc9305-hlt-24-panel.txt`,
  compiled with `mipi-dbi-cmd` into `/lib/firmware`.
* **Bootloader:** [lk2nd](https://github.com/msm8916-mainline/lk2nd).

The 6300 4G is in pmaports' *archived* category, meaning nobody maintains it
there. The dts and kernel are shared with the maintained `qcom-msm8909`
generic port. The Nokia 8000 4G (`nokia-sparkler`) differs only in its panel
and keypad wiring.

## Flashing (outline)

> ⚠️ This replaces KaiOS. Back up the stock partitions first, especially
> `modem` and `persist`: Vortex loads the Wi-Fi and modem firmware from them.

1. Get EDL access and a firehose loader for the 6300 4G. The BananaHackers
   and postmarketOS wikis cover this. Back up all partitions.
2. Flash **lk2nd** (built for msm8909) to the `boot` partition.
3. Write `out/vortex-nokia-leo.img` from `rootfs/mkimage.sh` to a microSD card.
4. Boot. lk2nd finds `extlinux/extlinux.conf` on the SD card's first
   partition and boots the kernel with `root=/dev/mmcblk1p2`.
5. For development, `fastboot boot` works through lk2nd. For a console, set
   `VORTEX_SERIAL_GETTY=1` when building to get a serial getty on `ttyMSM0`
   (115200), or use USB networking.

## Bring-up checklist and open problems

Tick these off on real hardware:

- [ ] **Kernel boots without initramfs** from SD (`kernel/vortex.config`
      builds in `sdhci-msm` and ext4). If the card shows up as a different
      `mmcblkN`, rebuild the image with `rootfs/mkimage.sh -r /dev/mmcblkNp2`.
- [ ] **`/dev/fb0` appears** through DRM fbdev emulation on `panel-mipi-dbi`,
      and the shell draws (RGB565).
- [ ] **Keypad codes.** Run `evtest` and adjust `/etc/vortex/keymap.conf` if
      the soft keys or call/end differ from the defaults.
- [ ] **Backlight** at `/sys/class/backlight/*`. The udev rule gives group
      `video` write access.
- [ ] **Wi-Fi (WCNSS).** The firmware comes from the `modem` and `persist`
      partitions through `01-vortex-firmware.sh`. Then check `vortex-wifi scan`.
- [ ] **Web runtime display (main risk).** Cog's DRM platform renders through
      GBM/EGL on the KMS device. On this phone the KMS device
      (`panel-mipi-dbi`) has no GPU, and the Adreno 304 is a separate
      render-only device. Two ways forward:
      1. Mesa `kmsro`: render on freedreno, scan out on the mipi-dbi device.
         This needs `panel-mipi-dbi` added to Mesa's kmsro driver list.
      2. Software rendering: `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast`
         (commented out in `/etc/vortex/runtime.conf`). Simple, but slow on
         a Cortex-A7.

      There is also a third option: a small Vortex WPE backend that renders
      with Skia on the CPU into shared memory, and lets the shell blit it to
      fbdev. That would avoid EGL entirely.
- [ ] **WPE WebKit/Cog build** for armv7l-musl. WebKit needs a lot of RAM and
      time to cross-compile. JIT is off on armv7, so the C loop interpreter is
      used: slower, but smaller.
- [ ] **Modem** over QRTR (`qrtr-ns`, `rmtfs`, `pd-mapper`, `tqftpserv`), then
      oFono. postmarketOS also needs a `qmicli --dpm-open-port` step for
      mobile data on MSM8909. That is not ported yet.
- [ ] **Call audio** needs an ALSA UCM profile for the q6voice path.
- [ ] **Suspend.** The shell blanks the panel after `screen_timeout`; real
      suspend-to-idle with keypad wake still needs testing.
