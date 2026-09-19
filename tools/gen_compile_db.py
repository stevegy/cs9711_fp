#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate a clangd-friendly compile_commands.json for an out-of-tree module.

The kernel's gen_compile_commands.py records the exact *gcc* command line used
to build the module. The editor's language server (clangd) parses with clang,
which rejects some gcc-only flags. We drop the ones that can hard-error and
silence the rest, so the IDE stops complaining about flags like
'-fconserve-stack' without touching the real gcc build.

Usage: gen_compile_db.py KDIR BUILD_DIR OUTPUT
"""

import json
import shlex
import subprocess
import sys

# gcc-only driver flags that clang doesn't recognize at all (these would show
# up as "Unknown argument: ..." diagnostics in clangd). Dropping them is safe:
# they only affect codegen/instrumentation, not parsing or types.
STRIP_EXACT = {
    "-fconserve-stack",
    "-fno-allow-store-data-races",
    "-fno-inline-functions-called-once",
    "-mrecord-mcount",
    "-fdiagnostics-show-context=2",
    "-falign-jumps=1",
    "-falign-loops=1",
    "-mindirect-branch-register",
    "-mindirect-branch-cs-prefix",
}

# Same, for flags that take a value (match any value the kernel might use).
STRIP_PREFIXES = (
    "-fsanitize=",
    "-fzero-init-padding-bits=",
    "-mindirect-branch=",
    "-mfunction-return=",
    "-mharden-sls=",
    "-fmin-function-alignment=",
)

# Catch-all: silently ignore any remaining gcc-only driver flags / -W options.
ADD_FLAGS = ["-Wno-unknown-argument", "-Wno-unknown-warning-option"]


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2

    kdir, build_dir, out = sys.argv[1:]

    subprocess.run(
        [sys.executable, f"{kdir}/scripts/clang-tools/gen_compile_commands.py",
         "-d", build_dir, "-o", out],
        check=True,
    )

    with open(out) as f:
        entries = json.load(f)

    for entry in entries:
        args = shlex.split(entry["command"])
        args = [a for a in args
                if a not in STRIP_EXACT
                and not any(a.startswith(p) for p in STRIP_PREFIXES)]
        args += ADD_FLAGS
        entry["command"] = " ".join(shlex.quote(a) for a in args)

    with open(out, "w") as f:
        json.dump(entries, f, indent=2)

    return 0


if __name__ == "__main__":
    sys.exit(main())
