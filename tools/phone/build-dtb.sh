#!/bin/sh
# Build the Nokia 6300 4G device tree with Vortex's additions (audio) from
# the msm8916-mainline source the postmarketOS kernel was built from.
#
#   tools/phone/build-dtb.sh OUT.dtb
#
# Needs: git, cpp, dtc.  The tag must match the kernel package (6.6.0).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1
TAG=${KERNEL_TAG:-v6.6-msm8916}
SRC=${KERNEL_SRC:-}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

if [ -z "$SRC" ]; then
	SRC=$T/linux
	git clone -q --depth 1 --filter=blob:none --sparse --branch "$TAG" \
		https://github.com/msm8916-mainline/linux "$SRC"
	git -C "$SRC" sparse-checkout set --skip-checks arch/arm/boot/dts/qcom \
		include/dt-bindings include/uapi/linux scripts/dtc/include-prefixes
fi
cp "$HERE/dts/qcom-msm8909-nokia-leo-vortex.dts" "$T/"
cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
	-I "$SRC/scripts/dtc/include-prefixes" -I "$SRC/arch/arm/boot/dts/qcom" \
	"$T/qcom-msm8909-nokia-leo-vortex.dts" -o "$T/leo.pp"
dtc -q -I dts -O dtb -o "$OUT" "$T/leo.pp"
echo "build-dtb: $OUT ($(stat -c %s "$OUT") bytes, from $TAG)"
