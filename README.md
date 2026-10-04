# cs9711_fp — Chipsailing CS9711 fingerprint reader for Linux (libfprint/fprintd)

Adds support for the **Chipsailing CS9711** optical fingerprint reader
(USB `2541:9711`, common in "CS9711Fingprint" hardware kits) to the standard
Linux fprint stack: **libfprint → fprintd → pam_fprintd / desktop login UIs**.

The driver is implemented *in-tree* inside libfprint (as a `FpImageDevice`),
built against the exact Fedora SRPM of the installed libfprint package, and
installed over the stock `libfprint-2.so` so that fprintd, the KDE/GNOME
"User & Fingerprint" panels, and PAM all work with zero configuration changes.

> Status: working driver, development stage. Enrollment succeeds; verify
> quality is being tuned (see `docs/session-summary-2026-09-27.md`).

## Layout

```
driver/libfprint-cs9711/   the CS9711 libfprint driver (FpImageDevice subclass)
tools/build_libfprint.sh   one-time: fetch/extract SRPM, wire driver in, meson+ninja build
tools/reinstall_libfprint.sh  fast loop: sync driver, ninja, install .so + metainfo, restart fprintd
tools/cs9711_libusb(.c)    standalone libusb protocol-validation tool (INIT/RESET/SCAN, saves a PGM frame)
udev/99-cs9711.rules       userspace access for seat user + wheel group
build/                    build trees, SRPM cache, stock .so backup (gitignored)
docs/                     design/dev docs and session logs
```

## Requirements

- Fedora 44 (x86_64) with `libfprint` + `fprintd` installed
  (`libfprint-1.94.100-1.fc44`, `fprintd-1.94.5-5.fc44` at time of writing)
- `meson`, `ninja` (plus the normal libfprint build deps)

## Build & install

```bash
# one-time setup (fetches + caches SRPM, meson setup in build/fpbuild)
bash tools/build_libfprint.sh

# per-iteration loop after editing driver/libfprint-cs9711/*.c{,h}:
# sync -> ninja -> sanity check -> sudo install .so+metainfo -> restorecon -> restart fprintd
bash tools/reinstall_libfprint.sh            # add --no-build / --no-restart to skip steps

# Makefile shortcuts: `make libfprint` (= full build script), `make` (libusb tool),
# `make compdb` (compile_commands.json for clang/clangd/zed)
```

First run of the loop backs up the stock `/usr/lib64/libfprint-2.so.2.0.0`
and metainfo into `build/backup-stock/` — restore from there if ever needed.

## Enroll / verify (as your normal user — **not** sudo)

```bash
fprintd-enroll     # ~15 placements (nr_enroll_stages); press firm and flat each stage
fprintd-verify
fprintd-list
```

Templates land in `/var/lib/fprint/<user>/cs9711/<finger>/<id>`.
Enrolling with `sudo` writes to the *root* user's store and is useless for you —
and an unparseable print file there will crash fprintd (upstream NULL-err bug,
see docs). The KDE "Fingerprint Authentication" panel performs the same
D-Bus calls as `fprintd-enroll`.

## Current known issues / open items

- `verify-no-match` after a marginal enrollment — next diagnostic:
  `./build/fpbuild/examples/img-capture` (frame quality vs. threshold tuning;
  `bz3_threshold` is commented out in the driver). Details + plan in
  `docs/session-summary-2026-09-27.md`.
- SDDM greeter has no fingerprint unlock on Fedora/KDE — password at the lock
  screen; fingerprint covers in-session unlocks. Greeter unlock = separate project.
- fprintd upstream bug: deserializing a bad print file segfaults instead of
  logging (`err == NULL` deref in `file_storage.c`) — worth an upstream report.

## Docs

- `docs/fingerprint-drive.md` — Linux fingerprint driver programming model & call flow
- `docs/dev-doc.md`, `docs/dev-log.md` — development notes / log
- `docs/session-summary-2026-09-27.md` — state of play and next steps
- `docs/cs9711-architecture.drawio` — architecture diagram (draw.io)
