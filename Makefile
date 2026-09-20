# Top-level Makefile for cs9711_fp — userspace fingerprint tooling only.
# No kernel module targets: the CS9711 is driven from userspace (libusb).

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PKGCONF ?= pkg-config

all: tools/cs9711_libusb

tools/cs9711_libusb: tools/cs9711_libusb.c
	$(CC) $(CFLAGS) $< -o $@ $(shell $(PKGCONF) --cflags --libs libusb-1.0)

clean:
	rm -f tools/cs9711_libusb out.pgm

.PHONY: all clean libfprint

# Build libfprint with the CS9711 driver integrated (see dev-log.md, Phase 1)
libfprint:
	bash tools/build_libfprint.sh

.PHONY: compdb

# Regenerate compile_commands.json for clangd/Zed (see tools/gen_userspace_compdb.py)
compdb:
	python3 tools/gen_userspace_compdb.py .
