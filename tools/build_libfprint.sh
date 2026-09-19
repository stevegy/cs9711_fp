#!/bin/bash
# Build libfprint (freedesktop FPrint) v1.94.100 with the CS9711 driver integrated.
#
# Upstream source comes from the Fedora SRPM of the installed libfprint package,
# so the result matches what fprintd / pam_fprintd were built against.
# Network steps require: source ~/proxy.env
#
# Usage: bash tools/build_libfprint.sh [--clean]   (--clean re-extracts the source)
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD=build
TARBALL=$BUILD/srpm/libfprint-v1.94.100.tar.gz
SRC=$BUILD/src-libfprint
BUILDDIR=$BUILD/fpbuild

# 1. Fetch upstream source from the SRPM (already bundled if present).
if [ ! -f "$TARBALL" ]; then
    mkdir -p "$BUILD/srpm"
    [ -f ~/proxy.env ] && . ~/proxy.env
    dnf download --source libfprint
    (cd "$BUILD/srpm" && rpm2cpio "$(ls -t *.src.rpm | head -1)" | cpio -idm --quiet)
fi

# 2. Extract upstream source tree.
if [ ! -d "$SRC" ] || [ "${1:-}" = "--clean" ]; then
    rm -rf "$SRC"
    tar xzf "$TARBALL" -C "$BUILD"
    mv "$BUILD/libfprint-v1.94.100" "$SRC"
fi

# 3. Integrate our driver (idempotent).
cp driver/libfprint-cs9711/cs9711.c driver/libfprint-cs9711/cs9711.h "$SRC/libfprint/drivers/"
grep -q "'cs9711': {}" "$SRC/meson.build" \
    || sed -i "/'nb1010': {},/a\\    'cs9711': {}," "$SRC/meson.build"
grep -q "files('drivers/cs9711.c')" "$SRC/libfprint/meson.build" \
    || sed -i "/files('drivers\/nb1010.c')/a\\    'cs9711' : files('drivers/cs9711.c')," "$SRC/libfprint/meson.build"

# 4. Configure + build (Fedora builds with -Ddrivers=all; skip docs/tests).
if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" "$SRC" \
        -Ddrivers=all \
        -Ddoc=false \
        -Dinstalled-tests=false
fi
meson compile -C "$BUILDDIR"

echo "built: $BUILDDIR/libfprint/libfprint-2.so"
