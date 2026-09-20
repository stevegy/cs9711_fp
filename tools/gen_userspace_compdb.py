#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate the repo-root compile_commands.json for clangd (Zed).

Covers three translation units:
  * driver/libfprint-cs9711/cs9711.c — our CS9711 libfprint driver source;
    reuses the exact meson flags of the in-tree copy from
    build/fpbuild/compile_commands.json when available (minimal glib/gio
    fallback otherwise), so drivers_api.h & co. resolve with the right flags,
  * tools/cs9711_libusb.c — via pkg-config libusb-1.0,
  * driver/cs9711.{c,h} — the abandoned out-of-tree kernel module, kept in
    the repo for reference (see dev-doc.md). Entries are generated against
    the newest installed kernel-devel tree (/usr/src/kernels/<ver>) with
    kbuild-style include paths and defines, so clangd resolves all
    <linux/...> headers and can index symbols. Note: KBUILD_MODNAME /
    KBUILD_MODFILE come from kbuild's command line (not headers), so they
    are defined here explicitly.

Usage: python3 tools/gen_userspace_compdb.py [repo_root]
"""

import json
import os
import re
import subprocess
import sys

root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.curdir)
out_path = os.path.join(root, "compile_commands.json")


def newest_kernel_dir():
    """Return the newest installed kernel-devel tree, or None."""
    base = "/usr/src/kernels"
    try:
        names = [n for n in os.listdir(base)
                 if os.path.isdir(os.path.join(base, n))]
    except OSError:
        return None
    if not names:
        return None

    def verkey(name):
        # kernel release prefix before the hyphen, e.g. 7.2.5 -> (7, 2, 5)
        return tuple(int(p) for p in re.findall(r"\d+", name.split("-", 1)[0]))

    return os.path.join(base, max(names, key=verkey))


def main() -> None:
    out = []

    # 1. our libfprint driver source, with the meson build's flags
    repo_drv = "driver/libfprint-cs9711/cs9711.c"
    try:
        with open(os.path.join(root, "build", "fpbuild", "compile_commands.json")) as f:
            meson_db = json.load(f)
    except (OSError, ValueError):
        meson_db = []

    for entry in meson_db:
        if entry["file"].endswith("drivers/cs9711.c"):
            cmd = entry.get("command") or " ".join(entry.get("arguments", []))
            rel = os.path.relpath(os.path.join(root, repo_drv), entry["directory"])
            out.append({
                "directory": entry["directory"],
                "command": cmd.replace(entry["file"], rel, 1),
                "file": rel,
            })
            break
    else:
        # meson DB absent (build/ cleaned): minimal fallback flags
        glib = subprocess.run(
            ["pkg-config", "--cflags", "glib-2.0", "gio-2.0"],
            capture_output=True, text=True).stdout.strip()
        out.append({
            "directory": root,
            "command": f"cc -std=c11 -D_GNU_SOURCE {glib} "
                       f"-Ibuild/src-libfprint/libfprint -xc {repo_drv}",
            "file": repo_drv,
        })

    # 2. userspace capture tool
    usbin = subprocess.run(["pkg-config", "--cflags", "libusb-1.0"],
                           capture_output=True, text=True).stdout.strip()
    out.append({
        "directory": root,
        "command": f"cc -std=gnu11 {usbin} -xc tools/cs9711_libusb.c",
        "file": "tools/cs9711_libusb.c",
    })

    # 3. abandoned kernel module (reference only; see dev-doc.md)
    kdir = newest_kernel_dir()
    if kdir:
        incs = (f"-I{kdir}/include -I{kdir}/arch/x86/include "
                f"-I{kdir}/arch/x86/include/generated "
                f"-I{kdir}/arch/x86/include/generated/uapi -I{kdir} "
                f"-I{kdir}/include/uapi -I{kdir}/arch/x86/include/uapi "
                f"-I{kdir}/include/generated/uapi")
        # kbuild defines KBUILD_MODNAME/KBUILD_MODFILE as string literals
        # (embedded quotes), so MODULE_* string concatenation works. The
        # escaped-quote form below lexes to that under both clangd's command
        # parser and real shells.
        common = (f"-std=gnu11 -nostdinc -D__KERNEL__ "
                  f'-D"KBUILD_MODNAME=\\\"cs9711\\\"" '
                  f'-D"KBUILD_MODFILE=\\\"driver/cs9711.mod.c\\\"" '
                  f"-include {kdir}/include/linux/kconfig.h "
                  f"-include {kdir}/include/generated/autoconf.h {incs}")
        for rel in ("driver/cs9711.c", "driver/cs9711.h"):
            if not os.path.exists(os.path.join(root, rel)):
                continue
            # The header alone only includes <linux/ioctl.h> and relies on
            # the .c's includes for basic types; force-include <linux/types.h>
            # so clangd can parse it standalone.
            extra = (f"-include {kdir}/include/linux/types.h "
                     if rel.endswith(".h") else "")
            out.append({
                "directory": root,
                "command": f"cc {common} {extra}{rel}",
                "file": rel,
            })
    else:
        print("warn: no kernel-devel tree under /usr/src/kernels; "
              "skipping kernel reference entries", file=sys.stderr)

    with open(out_path, "w") as f:
        json.dump(out, f, indent=2)
    print(f"wrote {out_path}: {len(out)} entries")


if __name__ == "__main__":
    main()
