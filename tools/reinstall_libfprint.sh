#!/bin/bash
# Rebuild (fast path) and install the CS9711-enabled libfprint, then restart fprintd.
#
# The per-iteration build/test loop for the in-tree driver:
#   1. ninja -C build/fpbuild      recompile driver/libfprint-cs9711/cs9711.c -> relink libfprint-2.so
#   2. one-time backup             stock .so + metainfo preserved in build/backup-stock/
#   3. install                     .so -> /usr/lib64, metainfo -> /usr/share/metainfo (sudo)
#   4. restart fprintd            (system unit; restorecon keeps SELinux labels right)
#
# First-time setup only: bash tools/build_libfprint.sh   (fetches SRPM, meson setup)
#
# Usage: tools/reinstall_libfprint.sh [--no-build] [--no-restart] [-h]
set -euo pipefail

cd "$(dirname "$0")/.."

ART="build/fpbuild/libfprint/libfprint-2.so.2.0.0"
META="build/fpbuild/libfprint/org.freedesktop.libfprint.metainfo.xml"
SYS_SO="/usr/lib64/libfprint-2.so.2.0.0"
SYS_META="/usr/share/metainfo/org.freedesktop.libfprint.metainfo.xml"
BKDIR="build/backup-stock"

do_build=1
do_restart=1
for arg in "$@"; do
    case "$arg" in
        --no-build)   do_build=0 ;;
        --no-restart) do_restart=0 ;;
        -h|--help)
            echo "Usage: $0 [--no-build] [--no-restart]"
            echo "  --no-build    install the existing build artifact without running ninja"
            echo "  --no-restart  don't restart fprintd after installing"
            exit 0 ;;
        *) echo "unknown option: $arg (see -h)" >&2; exit 2 ;;
    esac
done

# 1. Rebuild — fast path: only the changed TU(s) are recompiled and relinked.
if [ "$do_build" = 1 ]; then
    ninja -C build/fpbuild \
        || { echo "build failed (first-time setup needs: bash tools/build_libfprint.sh)" >&2; exit 1; }
fi

[ -f "$ART" ]  || { echo "artifact missing: $ART" >&2; exit 1; }
[ -f "$META" ] || { echo "metainfo missing: $META" >&2; exit 1; }
strings "$ART" | grep -q cs9711 \
    || { echo "cs9711 driver not linked into $ART — run: bash tools/build_libfprint.sh" >&2; exit 1; }

SUDO=""
[ "$(id -u)" = "0" ] || SUDO="sudo"

# 2. One-time backup of whatever is currently installed (first run only, so the
#    original stock files stay preserved even across repeated installs).
if [ ! -d "$BKDIR" ]; then
    mkdir -p "$BKDIR"
    cp -a "$SYS_SO" "$BKDIR/"
    [ -f "$SYS_META" ] && cp -a "$SYS_META" "$BKDIR/" || true
    echo "backed up stock files to $BKDIR/"
fi

# 3. Install (keeps the versioned soname file; /usr/lib64 symlinks stay intact).
$SUDO install -m 755 "$ART" "$SYS_SO"
$SUDO install -m 644 "$META" "$SYS_META"
# Files copied from $HOME carry home SELinux labels; reset them.
$SUDO restorecon -v "$SYS_SO" "$SYS_META" 2>/dev/null || true

# 4. Restart the daemon (system unit on Fedora) and confirm it came up.
if [ "$do_restart" = 1 ]; then
    $SUDO systemctl restart fprintd
    sleep 1
    if ! $SUDO systemctl is-active -q fprintd; then
        echo "fprintd failed to start — check: journalctl -u fprintd -n 50" >&2
        exit 1
    fi
fi

echo "installed CS9711-enabled libfprint -> $SYS_SO"
echo "next: plug in the reader, then 'fprintd-list' / 'fprintd-enroll'"
