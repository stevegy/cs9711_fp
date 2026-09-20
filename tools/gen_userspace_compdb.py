#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate the repo-root compile_commands.json for clangd (Zed).

Covers the two userspace translation units:
  * driver/libfprint-cs9711/cs9711.c — our CS9711 libfprint driver source;
    reuses the exact meson flags of the in-tree copy from
    build/fpbuild/compile_commands.json when available (minimal glib/gio
    fallback otherwise), so drivers_api.h & co. resolve with the right flags,
  * tools/cs9711_libusb.c — via pkg-config libusb-1.0.

Usage: python3 tools/gen_userspace_compdb.py [repo_root]
"""

import json
import os
import subprocess
import sys

root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.curdir)
out_path = os.path.join(root, "compile_commands.json")


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

    with open(out_path, "w") as f:
        json.dump(out, f, indent=2)
    print(f"wrote {out_path}: {len(out)} entries")


if __name__ == "__main__":
    main()
