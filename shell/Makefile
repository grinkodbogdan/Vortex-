# vortex-shell
#
#   make                 device build (fbdev + evdev)
#   make headless        test build driven by a key script
#   make CC=armv7l-linux-musleabihf-gcc   cross-compile for the phone
#   make test            build + run the headless smoke tests

VERSION  ?= 0.1.0
CC       ?= cc
CFLAGS   ?= -Os -pipe
override CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter \
            -Wno-missing-field-initializers -Wno-format-truncation -ffunction-sections -fdata-sections \
            -DVORTEX_VERSION='"$(VERSION)"'
override LDFLAGS += -Wl,--gc-sections
override LDLIBS += -lm

PREFIX   ?= /usr
DESTDIR  ?=

COMMON = src/main.c src/ui.c src/gfx.c src/apps.c src/store.c src/json.c \
         src/proc.c src/sysinfo.c src/t9.c src/util.c

all: vortex-shell vortex-splash

# Nuklear + stb_image are big; build them once with optimisation for size.
build/nk_impl.o: src/nk_impl.c src/nk.h
	@mkdir -p build
	$(CC) $(CFLAGS) -w -c -o $@ $<

build/%.o: src/%.c src/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

OBJS = $(patsubst src/%.c,build/%.o,$(COMMON)) build/nk_impl.o

vortex-shell: $(OBJS) build/platform_linux.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

vortex-shell-headless: $(OBJS) build/platform_headless.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Boot logo (initramfs): standalone, no Nuklear.
vortex-splash: src/splash.c src/splash_data.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/splash.c

headless: vortex-shell-headless

test: vortex-shell-headless vortex-splash
	./tests/run.sh

install: vortex-shell vortex-splash
	install -Dm755 vortex-shell $(DESTDIR)$(PREFIX)/bin/vortex-shell
	install -Dm755 vortex-splash $(DESTDIR)$(PREFIX)/libexec/vortex/vortex-splash

clean:
	rm -rf build vortex-shell vortex-shell-headless vortex-splash tests/out

.PHONY: all headless test install clean
