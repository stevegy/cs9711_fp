# CS9711 Fingerprint — Session Summary (2026-09-27)

## ✅ Done & working
- **Driver**: in-tree libfprint image driver at `driver/libfprint-cs9711/`, builds into
  `libfprint-2.so` via `build/fpbuild`. Device shows as **"Chipsailing CS9711 Fingerprint"**
  (typo fixed, commit `ddb2014`).
- **Build loop**: `tools/reinstall_libfprint.sh` — syncs driver files into
  `build/src-libfprint/`, runs `ninja -C build/fpbuild`, sanity-checks the artifact,
  installs to `/usr/lib64` + metainfo (sudo), restorecon, restarts fprintd.
  Two bugs fixed along the way: missing source-sync into the meson copy, and a
  `set -o pipefail` + `grep -q` trap (`grep -q` exits on first match → SIGPIPE kills
  `strings` → pipeline reports failure even when the check passed; replaced with a count).
- **Enrollment**: successful — template at `/var/lib/fprint/steve/cs9711/0/2`
  ("left-index-finger"). Proves the previously-crashing load path now works.

## 🐛 The core-dump mystery (solved, archived)
Old template `/var/lib/fprint/root/cs9711/0/7` (written Aug 2 by a `sudo fprintd-enroll`
test) could not be deserialized by the newer fprintd (updated Aug 5 to 1.94.5-5.fc44),
and an **upstream fprintd bug** (`g_print("Error deserializing data: %s", err->message)`
with `err == NULL` at `file_storage.c:194`) turned that parse failure into a SEGV
(`segfault at 8` = GError `.message` field).
- Backed up to `~/fprintd-template-backup-20260802/root-cs9711-0/7` (cmp-verified) and removed.
- Unrelated to the CS9711 driver itself (driver is a pure image device; template
  generation happens in libfprint core).
- Any unparseable print file will still segfault fprintd until upstream fixes it —
  worth filing an upstream bug.

## 🔴 Open: `verify-no-match`
`fprintd-verify` completes but reports **no match** — likely a weak template (journal
showed `Failed to detect minutiae: No minutiae found` during enroll) and/or conservative
dev-driver image processing (`bz3_threshold` commented out at cs9711.c line 465,
`nr_enroll_stages = 15`).

### Next diagnostic (finger on reader)
```bash
cd cs9711_fp && ./build/fpbuild/examples/img-capture
```
Interpretation:
| img-capture result | Meaning | Next move |
|---|---|---|
| Good bright frames, but verify still no-match | Template built from bad enroll data | Re-enroll: `fprintd-delete && fprintd-enroll`, firm + flat presses for each of 15 stages |
| Frames near-black / pure noise | Driver image pipeline / threshold issue | Tune driver in `cs9711.c` (raw 68×118 frames → libfprint generic pipeline) |
| No frames at all | Capture SSM / USB state issue | Debug the SSM with journal traces |

If re-enroll with firm presses still no-matches: the dev driver may need its own image
preprocessing (normalization/threshold) before minutiae extraction — real work on
`cs9711.c`, using `img-capture` output as ground truth.

## ℹ️ Lock-screen note
SDDM greeter has **no fingerprint unlock** on Fedora/KDE (password only at lock screen;
checked `/etc/sddm.conf` + `/etc/sddm.conf.d/`, no fingerprint option in SDDM).
Fingerprint works for in-session unlocks. True greeter unlock would need custom
PAM/helper work — a separate project.

## State
- Repo: `cs9711_fp`, branch `master`, tip `ddb2014`
- fprintd: restarted, active; our libfprint installed in `/usr/lib64`
  (stock backup in `build/backup-stock/`)
- Reader: unplugged as of this summary (USB ID 2541:9711; note the sensor's own USB
  descriptor string still says "CS9711Fingprint" — that's vendor firmware, not our name)
