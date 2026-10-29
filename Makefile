# Vortex - top-level entry points.  See README.md.

.PHONY: all shell headless test kernel rootfs image screenshots clean

all: shell

shell:
	$(MAKE) -C shell

headless:
	$(MAKE) -C shell headless

# Host tests: shell screens (headless) + KaiOS runtime/installer + shim.
test:
	$(MAKE) -C shell test
	runtime/tests/run.sh

kernel:
	kernel/build.sh out/kernel

# Needs root, xbps and qemu-arm; see rootfs/build.sh for options.
rootfs:
	rootfs/build.sh

image:
	rootfs/mkimage.sh

clean:
	$(MAKE) -C shell clean
	rm -rf out
