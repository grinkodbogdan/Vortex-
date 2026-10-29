#!/bin/sh
# Build the Nokia 6300 4G kernel: msm8916-mainline (covers MSM8909) with
# postmarketOS' armv7 config plus kernel/vortex.config.
#
#   kernel/build.sh [OUTDIR]
#
# Needs: a cross compiler (CROSS_COMPILE=armv7l-linux-musleabihf- by
# default, or LLVM=1), make, flex, bison, bc, perl, openssl headers.
# Produces OUTDIR/zImage, OUTDIR/dtbs/, OUTDIR/modules/ (to copy into the
# rootfs) and OUTDIR/kernel.release.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mkdir -p "${1:-$HERE/../out/kernel}" && cd "${1:-$HERE/../out/kernel}" && pwd)

# Pinned sources.
KVER=6.12.1
KTAG=v$KVER-msm8916
KURL=https://github.com/msm8916-mainline/linux/archive/refs/tags/$KTAG.tar.gz
PMAPORTS=690ccca91f4b5d60bd9e306f8449cd80e3daf682
PMCONFIG=https://gitlab.postmarketos.org/postmarketOS/pmaports/-/raw/$PMAPORTS/device/testing/linux-postmarketos-qcom-msm8916/config-postmarketos-qcom-msm8916.armv7
DTB=qcom/qcom-msm8909-nokia-leo.dtb

export ARCH=arm
if [ -z "${LLVM:-}" ]; then
	export CROSS_COMPILE=${CROSS_COMPILE:-armv7l-linux-musleabihf-}
fi
JOBS=${JOBS:-$(nproc)}

SRC=$OUT/linux-$KVER-msm8916
if [ ! -d "$SRC" ]; then
	curl -fL "$KURL" | tar -xz -C "$OUT"
fi
curl -fsSL -o "$OUT/base.config" "$PMCONFIG"

cd "$SRC"
cp "$OUT/base.config" .config
scripts/kconfig/merge_config.sh -m .config "$HERE/vortex.config"
make olddefconfig

[ -f "arch/arm/boot/dts/$DTB" ] || [ -f "arch/arm/boot/dts/${DTB%.dtb}.dts" ] ||
	echo "warning: $DTB is not in this kernel tree; check the nokia-leo dts" >&2

make -j"$JOBS" zImage dtbs modules
rm -rf "$OUT/modules"
make INSTALL_MOD_PATH="$OUT/modules" INSTALL_MOD_STRIP=1 modules_install
cp arch/arm/boot/zImage "$OUT/zImage"
mkdir -p "$OUT/dtbs"
cp arch/arm/boot/dts/qcom/qcom-msm8909-*.dtb "$OUT/dtbs/" 2>/dev/null || true
make -s kernelrelease > "$OUT/kernel.release"
echo "kernel $(cat "$OUT/kernel.release") built in $OUT"
