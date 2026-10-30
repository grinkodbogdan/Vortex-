#!/bin/sh
# Rebuild the few kernel modules Vortex patches (tools/phone/kernel/*.patch)
# for the packaged postmarketOS kernel, from the source tag it was built
# from, and drop them into its module tree.
#
#   tools/phone/build-modules.sh KERNEL_CONFIG MODULE_DIR
#
# The modules come out unsigned (the kernel loads them, tainted: signature
# checking is not enforced) and without symbol versions (CONFIG_MODVERSIONS
# is off), so only vermagic has to match, which is checked.
# Needs: git, make, bc, flex, bison, arm-linux-gnueabihf-gcc.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CONFIG=$1
MODDIR=$2
TAG=${KERNEL_TAG:-v6.6-msm8916}
CROSS=${CROSS_COMPILE:-arm-linux-gnueabihf-}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
msg() { printf 'build-modules: %s\n' "$*"; }

git clone -q --depth 1 --branch "$TAG" https://github.com/msm8916-mainline/linux "$T/linux"
cd "$T/linux"
rm -rf .git   # no "+" from setlocalversion: the release must be 6.6.0-msm8916
for p in "$HERE"/kernel/*.patch; do
	patch -p1 -s < "$p"
	msg "applied ${p##*/}"
done
cp "$CONFIG" .config
K="make -s ARCH=arm CROSS_COMPILE=$CROSS KBUILD_MODPOST_WARN=1"
$K olddefconfig
$K modules_prepare
$K M=sound/soc/qcom modules

MODULES="sound/soc/qcom/snd-soc-apq8016-sbc.ko"
for ko in $MODULES; do
	dest=$MODDIR/kernel/$ko
	# a compressed original is replaced by the plain module (depmod runs on
	# first boot)
	if [ ! -e "$dest" ] && [ -e "$dest.zst" ]; then
		zstd -qdc "$dest.zst" > "$dest" && rm -f "$dest.zst"
	fi
	want=$(modinfo -F vermagic "$dest" 2>/dev/null || strings "$dest" | sed -n 's/^vermagic=//p')
	got=$(modinfo -F vermagic "$ko" 2>/dev/null || strings "$ko" | sed -n 's/^vermagic=//p')
	[ "$want" = "$got" ] || { msg "vermagic mismatch for $ko: '$got' vs '$want'"; exit 1; }
	"${CROSS}strip" --strip-debug "$ko"
	cp "$ko" "$dest"
	msg "installed $ko ($got)"
done
