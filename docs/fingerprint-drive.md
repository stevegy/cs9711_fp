# The Linux fingerprint programming model (libfprint)

## 1. The three layers and who owns what

Explain the model and map every stage to your actual driver functions.

```
┌──────────────────────────────────────────────────────────┐
│ fprintd (system daemon, D-Bus service)                     │
│   - owns templates (FPR_HOME), Enroll/Verify/Cancel API    │
│   - NO hardware knowledge                                  │
├──────────────────────────────────────────────────────────┤
│ libfprint core (libfprint.so)                              │
│   - loads driver .so modules, matches VID/PID              │
│   - OWNS the operation state machine (the "stages")        │
│   - OWNS the fingerprint algorithm (enroll/verify math)    │
├──────────────────────────────────────────────────────────┤
│ YOUR driver module: fprint-cs9711.so  (GObject type)       │
│   - only answers: open/close, activate/deactivate,         │
│     "a state happened — what should I do?", and            │
│     "here's one captured image"                            │
└──────────────────────────────────────────────────────────┘
```

The key inversion vs. the kernel model you just deleted: in a kernel `usb_driver`/`cdev` module, *you* own the whole protocol, op lifecycle and blocking semantics. In libfprint's **image-device model**, the core library owns everything around one job — **capture one image per finger-on event** — and pokes your driver at fixed points. That's why the design survives kernel updates untouched.

## 2. What the driver must provide (your contract)

Your module is a GObject subclass of `FP_TYPE_IMAGE_DEVICE` (`G_DEFINE_TYPE(FpDeviceCs9711, fpi_device_cs9711, FP_TYPE_IMAGE_DEVICE)`). The core calls only five vtable slots, which you fill in `fpi_device_cs9711_class_init`:

| Vtable slot | Your function | When the core calls it |
|---|---|---|
| `img_open` | `dev_open` | Before any operation. You claim USB interface 0 (`g_usb_device_claim_interface`) and must finish with `fpi_image_device_open_complete(dev, err)` |
| `activate` | `dev_activate` | Operation is about to run (enroll *or* verify) |
| `deactivate` | `dev_deactivate` | Operation finished/cancelled |
| `change_state` | `dev_change_state` | **Every time the core's op state machine moves** — this is the stage dispatcher |
| `img_close` | `dev_close` | After deactivation; you release the interface |

Plus static description in `class_init`, which the core uses before any callback:
- `id_table` (`FpIdEntry`): `{0x2541, 0x0236}`, `{0x2541, 0x9711}` — libusb enumeration matches a physical reader against all loaded drivers' tables; match ⇒ instantiate your type.
- `scan_type = FP_SCAN_TYPE_PRESS` — tells the core this is a press-and-hold sensor: between captures the core will wait for *finger off*, not just start the next scan.
- `nr_enroll_stages = 15` — enroll means **15 captured images** fed to the algorithm.
- `img_width/img_height = 68×118` — the image format you hand back (after your repack).

Every entry point has a matching `_complete(...)` call you must make exactly once (`open_complete`, `activate_complete`, `close_complete`) — that's the async handshake with the core.

## 3. The operation state machine — where "stages" live

The core drives the op through these states (the enum your `change_state` receives):

```
IDLE ──open──▶ ACTIVATE ──▶ AWAIT_FINGER_ON ⇄ AWAIT_FINGER_OFF ──▶ SCANNING ──▶ ...
                              (repeat per enroll stage)              │
                                                              SUCCESS/FAILED ──▶ DEACTIVATE ──▶ close
```

**Enroll:** core loops `nr_enroll_stages` (15×): `AWAIT_FINGER_ON` → driver reports finger-on → `SCANNING` (you capture one image, core runs the algorithm on it and grows the template, fprintd emits `EnrollProgress`) → you report finger-off → `AWAIT_FINGER_OFF`/next stage. After stage 15 the template is complete and the core hands it to fprintd for storage.
**Verify:** one single pass of `AWAIT_FINGER_ON → SCANNING`; the algorithm compares against the stored template; core reports success/failure to fprintd (`VerificationSucceeded/Failed`).

### How each stage lands on your functions

| Stage (core side) | Your code that runs |
|---|---|
| **open** | `dev_open`: claim iface 0, zero `image_buffer`, `open_complete` |
| **activate** (op starting) | `dev_activate` → **INIT SSM** (`m_init_state`, 6 states): send `INIT(1)` → read 8-byte status; on timeout, recovery path: drain stale reply → send `RESET(2)` → retry `INIT(1)` once → validate magic `EA 01 62 A0 00 00 C3 EA` (`m_init_read_cb_check_expected`) → `activate_complete` |
| **AWAIT_FINGER_ON** | `dev_change_state(dev, AWAIT_FINGER_ON)` is the *only* state you react to: it starts the **SCAN SSM** (`m_scan_state`, 6 states) |
| inside scan (state machine, async via libusb callbacks) | `M_SCAN_INIT_SLEEP` (250 ms settle) → `M_SCAN_INIT_READ`: async bulk-IN for block 1 (8000 B) **and** send `SCAN(4)`, plus `fpi_image_device_report_finger_status(dev, TRUE)` ← this is how *you* tell the core "finger on" and it moves to SCANNING → wait for block-1 callback (`m_scan_read_cb_bulk` validates exactly 8000 B) → `M_SCAN_GET_IMAGE_TAIL`: block 2 (exactly 24 B) → `M_SCAN_SEND_POST_SCAN`: send `RESET(2)` → `M_SCAN_IMAGE_COMPLETE` |
| image hand-off | `m_scan_submit_image`: repacks the raw 34×236 buffer into the 68×118 `FpImage` (row interleaving `dy=y/2, dx=2x+y%2`), sets `FPI_IMAGE_PARTIAL`, then **`fpi_image_device_image_captured(dev, img)`** ← this is the single "product" of every scan; then `report_finger_status(dev, FALSE)` (finger off) |
| core side after hand-off | algorithm consumes the image (enroll: merge into template + progress signal; verify: match), state machine advances or op completes |
| **deactivate / close** | `dev_deactivate` → `deactivate_complete`; `dev_close` → release interface, `close_complete` |

So the call direction is strictly one-way for control (core → your vtable) and one push up for data/events: **images** via `fpi_image_device_image_captured`, **finger presence** via `report_finger_status`. You never ask the core to do anything.

### The internal machinery: two cooperating SSMs

Your driver's "working mode" is two small sequential state machines on libfprint's `FpiSsm` helper, both event-driven by USB completion callbacks (`transfer->ssm` back-pointer; callbacks call `fpi_ssm_next_state` / `fpi_ssm_mark_failed`):

- **INIT SSM** — once per `activate`; pure device handshake/recovery (INIT → [timeout ⇒ RESET → INIT] → status check).
- **SCAN SSM** — one per finger-on; the whole wire transaction: `SCAN(4)` → 8000 B + 24 B → `RESET(2)` → repack → submit.

The USB helpers are also fixed in mode for you by the hardware's quirks (`usb_read_in` always requests the max 8000 B and disables `short_is_error`, because the sensor sometimes answers out of sequence — errors are judged per-block in the callbacks).

## 4. Concurrency / exclusivity (the "working mode" at system level)

- fprintd holds the device exclusively during an op (interface claim + single consumer), which is why your standalone `tools/cs9711_libusb` capture tool only runs cleanly when fprintd isn't mid-operation — whoever claims interface 0 first owns the reader.
- The udev rule (`GROUP=wheel`, `MODE=0660`) is what lets *both* fprintd (as root) and you (in `wheel`) claim it at all — no kernel module involved anywhere.

## 5. Contrast with the model you just removed

| | Kernel char-driver model | libfprint image-device model (yours) |
|---|---|---|
| Op lifecycle owner | Your module | libfprint core |
| Algorithm | n/a (raw images to userspace) | inside libfprint, per your `nr_enroll_stages`/image format |
| Driver job | full protocol + cdev read/write/ioctl | 5 callbacks + "capture one image" |
| Kernel update | rebuild `.ko` | nothing — pure userspace `.so` |

If you want, I can turn this into a sequence diagram (fprintd ⇄ core ⇄ your SSMs ⇄ sensor, with the exact function names as lifelines) in the same draw.io style as your architecture doc.
