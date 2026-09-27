# CS9711 USB Fingerprint Reader — Development Document

## 1. Device & Environment

- **Device**: Chipsailing CS9711 fingerprint reader, USB `2541:9711` ("CS9711Fingprint"), bus 003.
- **Interface**: one vendor-specific interface (class 255 / subclass 17 / proto 34), **two bulk endpoints**:
  - EP 0x01 OUT (bulk, 64-byte max packet)
  - EP 0x81 IN  (bulk, 64-byte max packet)
  - Full-speed (12 Mbps), 100 mA, no mainline/libfprint support.
- **Login stack (Fedora 44)**: `libfprint-1.94.10`, `fprintd-1.94.5`, `fprintd-pam` (`pam_fprintd.so`), `libusb1-devel` — all installed.
- **Design**: userspace-only. libusb talks to the sensor directly; no kernel module, so no rebuilds when the kernel changes and no conflict with libusb's exclusive claim.
- **Network**: requires proxy — `source ~/proxy.env` before any curl/dnf/web access.

## 2. Recovered Protocol (from reference libfprint driver `archeYR/libfprint-CS9711`)

No blind reverse-engineering required. Protocol is simple:

- **Command frame** (8 bytes, bulk OUT → 0x01):
  `buf[0]=buf[7]=0xEA`, `buf[1]=buf[6]=cmd`, `buf[2..5]=0`.
  Commands: `INIT=1`, `RESET=2`, `SCAN=4`.
- **INIT / RESET reply** (bulk IN ← 0x81): 8 bytes, expected
  `{0xea, 0x01, 0x62, 0xa0, 0x00, 0x00, 0xc3, 0xea}`.
- **SCAN** flow:
  1. Arm an 8000-byte IN read (timeout 0 → blocking until data).
  2. Send `SCAN` command.
  3. Receive **8000 bytes** block 1.
  4. Receive **24 bytes** block 2.
  5. Total **8024 bytes = 34 × 236** raw sensor frame.
- **Image geometry**: raw sensor frame is 34×236. libfprint upscales to a 68×118 image (each sensor row duplicated horizontally, rows paired). Userspace tools keep the raw 8024-byte frame; upscaling stays in libfprint.
- **Timing**: RESET sleep 250 ms; USB wait timeout 300 ms.
- **Other supported VID/PID** in reference: `2541:0236` and `2541:9711`.

## 3. Deliverables

1. **Userspace capture tool** (`tools/cs9711_libusb`, libusb) — drives the sensor directly from userspace: INIT handshake (magic verified), SCAN, writes the raw 8024-byte frame to a PGM. Confirms the protocol on real hardware and doubles as the capture path for enrollment/verification.
2. **libfprint driver integration** — our own CS9711 driver in `driver/libfprint-cs9711/`, compiled into a rebuild of the exact system libfprint 1.94.10 (from the Fedora SRPM source), so `fprintd` can enroll and verify. The COPR/AUR fork (`cwt/libfprint-cs9711`, AUR `libfprint-cs9711-git`) serves as protocol implementation reference only; nothing from it is installed.
3. **Login via PAM** — `fprintd-enroll` / `fprintd-verify` with the already-installed `fprintd-pam` (`pam_fprintd.so`).

No kernel module is built, loaded, or needed: the CS9711 is a plain USB bulk device, and a userspace stack avoids kernel-ABI churn and the libusb/kernel exclusive-claim conflict.

## 4. Repository Layout

```
cs9711_fp/
├── dev-doc.md               # this document
├── Makefile                 # builds the userspace tool only (no kernel targets)
├── driver/
│   └── libfprint-cs9711/   # our own CS9711 libfprint driver (cs9711.c, cs9711.h)
├── tools/
│   ├── cs9711_libusb.c      # userspace libusb protocol/capture tool
│   └── build_libfprint.sh   # rebuilds system FPrint 1.94.10 with our driver in-tree
└── udev/99-cs9711.rules     # device permissions for non-root / seat users
```

## 5. Implementation

### Step 1 — build and run the capture tool

- Headers: `dnf install libusb1-devel` (already installed).
- Build: `make` → `tools/cs9711_libusb`.
- Run: `./tools/cs9711_libusb out.pgm` → prints `INIT ... OK (magic verified)`, then `SCAN (place finger on sensor) ...`, writes the 34×236 raw frame.
- Verify a fingerprint ridge pattern appears in the PGM with a finger on the sensor.
- Device access: install the udev rule (Step 3) or run with elevated privileges.

### Step 2 — libfprint / fprintd enrollment and verification

- Driver: our own, in `driver/libfprint-cs9711/`. Build with `make libfprint` (`tools/build_libfprint.sh`): rebuilds upstream FPrint 1.94.10 from the Fedora SRPM tarball with the driver wired into meson (`-Ddrivers=all`, matching Fedora), producing a `libfprint-2.so` whose exported API is byte-identical to the installed one.
- The fork `github.com/archeYR/libfprint-CS9711` (COPR `cwt/libfprint-cs9711` / AUR `libfprint-cs9711-git`) is a code implementation reference only — nothing from it is installed.
- Enroll: `fprintd-enroll` — the same SCAN flow as Step 1, repeated for template quality; fprintd stores the template.
- Verify: `fprintd-verify`.
- Login: `fprintd-pam` is already installed, so gdm/su/sudo can use the enrolled finger.

### Step 3 — device permissions (udev)

`udev/99-cs9711.rules` lets the libusb tool and fprintd open the device without root:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="2541", ATTR{idProduct}=="9711", TAG+="uaccess", MODE="0660", GROUP="wheel"
```

Fedora has no `plugdev` group; `uaccess` covers the logged-in seat user and `wheel` the admin users. Apply with:

```
sudo udevadm control --reload-rules && sudo udevadm trigger
```

## 6. Risks / Decisions

- **No kernel module**: a USB bulk fingerprint sensor needs no kernel privileges. Earlier in the project a `cs9711.ko` char driver was developed; its sources (then in `driver/`) were removed from the repo (git history) once the userspace design was chosen — it would conflict with libusb and add kernel-update rebuild burden.
- **Frame format**: tools expose the raw 8024-byte sensor frame; the 68×118 upscale stays in libfprint/userspace.
- **No blind RE needed** — protocol confirmed by the reference driver, validated by `cs9711_libusb`.

## 7. Verification

- `cs9711_libusb` produces a recognizable fingerprint PGM (`out.pgm`).
- `fprintd-enroll` stores a template; `fprintd-verify` matches the enrolled finger and rejects others.
- Login works via `pam_fprintd` — no `/dev/cs9711` involved.

## Everything's verified. Here's the complete build → enable → test/debug loop as of today's state.

## Current state (verified just now)

| Item | State |
|---|---|
| Driver source | `driver/libfprint-cs9711/cs9711.c` — compiled **in-tree into `libfprint-2.so`** (not a standalone module) |
| Build artifact | ✅ current, driver linked in (`strings …libfprint-2.so.2.0.0 \| grep -c cs9711` → 17), metainfo generated at `build/fpbuild/libfprint/org.freedesktop.libfprint.metainfo.xml` |
| System library | ❌ still stock Fedora (`/usr/lib64/libfprint-2.so.2.0.0`, 0 cs9711 refs) → **fprintd cannot see the reader until the Phase 2 install** |
| Hardware | no CS9711 (2541:*) in `lsusb` right now — you'll need the reader plugged in for live tests |

## 1. Build loop

```bash
cd ~/works/cs9711_fp

# Fast inner loop (what you want 95% of the time): recompile just cs9711.c, relink the .so
ninja -C build/fpbuild

# Full/idempotent rebuild (re-runs SRPM/extract/meson-setup only if missing; no network needed now)
make libfprint        # = bash tools/build_libfprint.sh
```

Post-build sanity (same checks as Phase 1):
```bash
strings build/fpbuild/libfprint/libfprint-2.so.2.0.0 | grep -c cs9711          # expect ≥1
diff <(nm -D --defined-only /usr/lib64/libfprint-2.so.2.0.0) \
     <(nm -D --defined-only build/fpbuild/libfprint/libfprint-2.so.2.0.0)       # exported API must be identical (ABI match with fprintd/pam_fprintd)
```

Your **editor loop is already wired**: `make compdb` reuses the meson flags for this exact TU, so Zed/clangd gives you instant diagnostics while typing; `ninja` is the authority that links.

## 2. Enable — one-time install (Phase 2, needs sudo)

Per your dev-log plan (backup → swap `.so` + metainfo → verify):

```bash
cd ~/works/cs9711_fp
sudo mkdir -p build/backup-$(date +%F)
sudo cp /usr/lib64/libfprint-2.so.2.0.0 \
        /usr/share/metainfo/org.freedesktop.libfprint.metainfo.xml \
        build/backup-$(date +%F)/

sudo install -m 755 build/fpbuild/libfprint/libfprint-2.so.2.0.0 /usr/lib64/libfprint-2.so.2.0.0
sudo cp build/fpbuild/libfprint/org.freedesktop.libfprint.metainfo.xml /usr/share/metainfo/

# verify + restart (fprintd.service is a SYSTEM unit on this box)
sudo systemctl restart fprintd
fprintd-list        # should now list the CS9711 device when plugged in
```

Rollback = copy the two files back from `build/backup-*/`. The metainfo install is what adds the `usb:v2541p9711*`/`usb:v2541p0236*` modalias so fprintd enumerates your reader (confirmed in Phase 1).

## 3. Test/debug loop (each iteration after the one-time install)

```
edit cs9711.c (Zed, live clangd)
  → ninja -C build/fpbuild
  → sudo cp build/fpbuild/libfprint/libfprint-2.so.2.0.0 /usr/lib64/   (metainfo only once)
  → sudo systemctl restart fprintd
  → test
```

**Functional tests** (user at the reader):
- `fprintd-list` — device visible? templates present?
- `fprintd-enroll` — 15 finger placements (`nr_enroll_stages = 15`)
- `fprintd-verify` / negative test with another finger
- Regression: `make && ./tools/cs9711_libusb` still captures a PGM non-root

**Logs & driver-level debugging:**
```bash
journalctl -u fprintd -f                    # daemon + driver log lines
# driver's fp_dbg() calls go through g_debug under domain "cs9711" — to see them:
sudo systemctl edit fprintd                 # add:
#   [Service]
#   Environment="G_MESSAGES_DEBUG=cs9711"
```

**Deeper debug:** the .so is built with default `debugoptimized` (has `-g`), so after installing your build you can attach live:
```bash
gdb -p $(sudo systemctl show -p MainPID --value fprintd)
(gdb) b m_scan_state            # or dev_open / m_scan_read_cb_bulk / m_scan_submit_image
```

**Bisection trick:** if something fails, run `./tools/cs9711_libusb` — it speaks the same protocol (INIT/RESET/SCAN, 8000+24 B) with zero libfprint in between. If the tool works but fprintd's path doesn't, the bug is in your driver's SSM/callback layer, not the wire protocol.
