# vortex-adbd
#
#   make                 build (static for the phone: make CC=... LDFLAGS=-static)
#   make test            run the protocol tests against a real adb client

CC      ?= cc
CFLAGS  ?= -Os -pipe
override CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
override LDLIBS += -lutil -lpthread

vortex-adbd: adbd.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ adbd.c $(LDLIBS)

test: vortex-adbd
	./tests/run.sh

clean:
	rm -f vortex-adbd

.PHONY: test clean
