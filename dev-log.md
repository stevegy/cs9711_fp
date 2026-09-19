# CS9711 FP — Development Log

## 2026-08-23 — Approved plan: CS9711 driver + Fedora login integration

### Status check (why it is not usable for login yet)
- Hardware path ready: device opens non-root, INIT handshake magic verified on real hardware,
  fprintd D-Bus-activated with enroll/verify/list/delete CLIs.
- System libfprint is freedesktop FPrint 1.94.10 (libfprint-2.so.2, fp_* GObject API).
  Drivers (VFS/UPEK/ELAN/AES...) are compiled in; no dlopen of external drivers; devel
  package ships client headers only. CS9711 is absent and there is no supported way to
  drop in an out-of-tree driver .so.
- No PAM configuration references pam_fprintd.so yet (checked /etc/pam.d and /usr/lib/pam.d).

### Summary
Port the CS9711 protocol into a native driver for the system libfprint (freedesktop FPrint,
SONAME libfprint-2.so.2, version 1.94.10), rebuild that exact libfprint version with the driver
compiled in, install it over the system library, enroll a template via fprintd, and wire
pam_fprintd into GDM and sudo for fingerprint login.

### Phase 0 — Source acquisition (via proxy: source ~/proxy.env)
- Obtain the exact upstream source Fedora built: dnf download --source libfprint; if the SRPM
  tarball is unavailable, clone github.com/freedesktop/libfprint at the tag matching 1.94.10.
  Apply any Fedora spec patches found in the SRPM so symbol versioning (LIBFPRINT_2.0.0) and
  behavior match the installed .so.
- Clone github.com/archeYR/libfprint-CS9711 into a work dir as code reference only
  (protocol, image geometry, template usage). Nothing from it is installed.

### Phase 1 — Driver development (this repo, driver/ folder)
- Add driver/libfprint-cs9711/ containing the CS9711 driver source for the FPrint in-tree
  driver API, structured on an existing built-in driver in the source tree
  (e.g. drivers/vfs5011.c or aes2501.c):
  - USB match: gusb descriptors for 2541:9711 and 2541:0236; claim interface 0, bulk OUT 0x01 / IN 0x81.
  - Protocol per dev-doc section 2 (validated by tools/cs9711_libusb): 8-byte command frames
    {0xEA, cmd, 0, 0, 0, 0, cmd, 0xEA}; INIT/RESET reply magic {ea 01 62 a0 00 00 c3 ea} verified;
    SCAN = send cmd then read 8000 + 24 bytes; RESET sleep 250 ms, USB wait timeout 300 ms.
  - Image: raw 34x236 frame upscaled to the 68x118 FpImage exactly as in the reference driver
    (row duplication/interleaving), fed to libfprint's built-in template engine for enroll
    stages and verify, same as other built-in drivers.
- Existing kernel files in driver/ (cs9711.ko etc.) remain untouched, per dev-doc.
- Build support: a Makefile target (or small script) that builds the upstream FPrint checkout
  with our driver file added via meson/ninja, producing libfprint-2.so; install step backs up
  the current system .so first, then installs the rebuilt one under the same name and runs
  ldd/nm verification against fprintd.
- Update dev-doc.md: replace the COPR/AUR route in Step 2/Deliverable 2 with this own-driver
  path (keep archeYR listed as protocol reference).

### Phase 2 — Install and functional test
- Back up /usr/lib64/libfprint-2.so.2.* to the repo's build backup dir (or /root) before
  overwriting; install rebuilt .so; verify fprintd still loads (symbol check: fp_context_new,
  fp_device_enroll_sync present with same version tags).
- Enroll: fprintd-enroll (repeated finger placements until done) — user performs the physical step.
- Verify: fprintd-verify succeeds for enrolled finger; fprintd-list shows the template; a
  different finger is rejected.
- Regression: tools/cs9711_libusb still captures a PGM non-root after the swap (both userspace
  clients must coexist; libusb exclusive claim is momentary, no conflict expected).

### Phase 3 — PAM login wiring
- Back up each modified file to driver/pam-backup/ (or repo) before editing.
- /etc/pam.d/gdm: insert auth sufficient pam_fprintd.so before the existing unix/password
  auth lines, keeping password as fallback. Confirm exact line semantics against the installed
  pam_fprintd man page at implementation time.
- /etc/pam.d/sudo: same auth sufficient pam_fprintd.so line.
- Test: sudo -v authenticates with finger without a password; GDM greeter logs in with
  fingerprint; password login still works as fallback.
  Rollback = restore backed-up PAM files (and, if needed, the backed-up libfprint .so).

### Test plan / acceptance criteria
- fprintd-enroll completes on real hardware; fprintd-verify accepts enrolled finger and rejects another.
- Fingerprint login at GDM greeter succeeds; sudo works with fingerprint; password fallback intact.
- tools/cs9711_libusb still writes a recognizable PGM (no regression from library swap).

### Assumptions
- PAM scope: GDM (greeter login) + sudo, per "login process"; su/login/console left unchanged
  unless requested.
- Rebuilding the exact upstream 1.94.10 source guarantees ABI match with the installed
  fprintd/pam module; if the Fedora SRPM turns out unobtainable, fall back to the upstream tag
  of the same version and re-verify symbols before installing.
- All network steps (SRPM download, git clone) go through ~/proxy.env as dev-doc requires.
- Single-user machine (steve); template storage stays at fprintd defaults.

### Phase 0 — Done (2026-08-23)
- Source RPM fetched: libfprint-1.94.100-1.fc44.src.rpm -> build/srpm/. Upstream:
  gitlab.freedesktop.org/libfprint/libfprint tag v1.94.100 (tarball bundled in SRPM),
  NO Fedora patches. Build system: meson, Fedora builds with -Ddrivers=all.
- Reference cloned: build/ref-libfprint-cs9711 (archeYR fork). Driver at
  libfprint/drivers/cs9711/{cs9711.c,cs9711.h} (~15 KB), self-contained:
  - open/close = gusb claim/release iface 0; activate = INIT SSM (RESET recovery path);
    change_state(AWAIT_FINGER_ON) = SCAN SSM (8000 + 24 bytes); RESET sleep 250 ms,
    USB wait 300 ms. id_table: 2541:0236, 2541:9711. img_class algorithm FPI_PRINT_SIGFM,
    nr_enroll_stages 15; enroll/verify inherited from FpImageDevice (no fpt_* in driver).
  - Image: 34x236 raw -> 68x118 via dy=y/2, dx=x*2+y%2; FPI_IMAGE_PARTIAL flag.
    Matches dev-doc section 2 and our tools/cs9711_libusb exactly.
- Compatibility vs our 1.94.100 tree: all used APIs present (fpi_usb_transfer_*,
  fpi_ssm_*, fpi_image_device_*, fp_image_new, FpIdEntry, FPI_PRINT_SIGFM,
  fpi_device_get_usb_device). Port = copy driver files + meson wiring only.
- Meson wiring points in src-libfprint: top-level meson.build drivers_info dict
  ('cs9711': {}) and libfprint/meson.build driver source map (pattern:
  'vfs5011' : files('drivers/vfs5011.c')). Build with -Ddrivers=all to match Fedora.

### Phase 1 — Done (2026-08-23)
- Driver source in repo: driver/libfprint-cs9711/{cs9711.c,cs9711.h} (canonical copy;
  build script re-syncs it into the source tree on every run).
- Build: `make libfprint` -> tools/build_libfprint.sh -> meson setup + compile in
  build/fpbuild with -Ddrivers=all -Ddoc=false -Dinstalled-tests=false. Full build
  succeeded on first pass (158 targets, no errors).
- Artifact: build/fpbuild/libfprint/libfprint-2.so.2.0.0.
- Verification:
  * cs9711 compiled in: nm shows FpDeviceCs9711_private_offset,
    fpi_device_cs9711_get_type/init/class_intern_init (internal symbols).
  * Exported API of our .so == system /usr/lib64/libfprint-2.so.2.0.0 exactly
    (91 dynamic T/W symbols, diff empty) -> ABI/symbol-versioning safe drop-in.
  * Generated metainfo (org.freedesktop.libfprint.metainfo.xml) is a strict superset of
    the installed one and adds modalias usb:v2541p9711* / usb:v2541p0236* -> Phase 2
    should install it too so fprintd sees the device (backup first).
  * Generated udev rule file covers only SPI drivers; USB drivers rely on our own
    udev/99-cs9711.rules, which is already installed. No change needed.
- dev-doc.md Step 2 / Deliverable 2 updated: own-driver path (COPR/AUR fork kept as
  implementation reference only); repo layout section now lists driver/ + build script.

### Phase 2 — Install and functional test (next; needs user at the reader)
1. Back up /usr/lib64/libfprint-2.so.2.* and /usr/share/metainfo/org.freedesktop.libfprint.metainfo.xml
   into build/backup-<date>/.
2. Install rebuilt .so + metainfo; verify with ldd/nm that fprintd/pam_fprintd still resolve.
3. fprintd-enroll (user places finger), fprintd-list, fprintd-verify; rejection test with
   a different finger.
4. Regression: tools/cs9711_libusb still captures PGM non-root.
