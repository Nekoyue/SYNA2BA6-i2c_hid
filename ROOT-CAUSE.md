# SYNA2BA6 Root Cause

Why the touchpad loses multitouch gestures, and how this branch's fix follows from it.

## Short Version

The touchpad firmware re-initializes itself, mostly while the touchpad is idle, and comes
back in its default **mouse mode**. It announces the reset in a non-standard way that
Linux ignores, so nobody re-sends the Input Mode feature report. The pointer keeps
moving (mouse reports), but gestures are gone (no touchpad reports).

The `reset-resync` branch treats that announcement as the device-initiated reset it is
and reconfigures the device straight away.

## Report IDs

From this device's report descriptor:

| ID | Type | Content |
|---|---|---|
| `0x02` | input | Mouse collection (relative X/Y, buttons) |
| `0x03` | input | Touchpad (Precision Touchpad) contacts |
| `0x04` | feature | Input Mode (`0x03` = touchpad, `0x00` = mouse) |
| `0x06` | feature | Surface switch / button switch |
| `0x0b`, `0x0c` | input | Synaptics RMI-over-HID data / attention |
| `0x0d` | feature | Latency mode |

The RMI channel (reports `0x09`-`0x0c`, feature `0x0f`) uses the same IDs as `hid-rmi`.
`hid-multitouch` ignores it.

## Hypotheses

| | Hypothesis | Evidence for it |
|---|---|---|
| A | Firmware falls back to mouse mode | Input switches from report `0x03` to `0x02` |
| A1 | ...because the device reset itself | A reset announcement shortly before the switch |
| B | A contact gets stuck | Reports stay on `0x03`; last report before idle still has a finger down |

**A and A1 confirmed, B ruled out.**

## Findings

**First capture** (2026-10-10, no automatic resync):

```text
13:20:09  touchpad reports (0x03), healthy
          ... 29.5 s without touch ...
13:20:39  first packet after idle: 13 zero bytes (report ID 0x00)
          then RMI attention report 0x0c: 0c 02 01 00 ...
13:21:05  only mouse reports (0x02) from here on
13:21:37  manual resync -> touchpad reports again at 13:21:39
```

- During the broken window, `libinput record` saw 1,483 `REL_X/REL_Y` events on the
  Mouse node and none on the Touchpad node.
- All 8,272 touchpad reports came before the reset. In healthy touchpad mode the device
  **never** sends mouse reports, so a mouse report reliably means the mode was lost.
- No spec-style reset signal (zero-length input report). The firmware sends an
  all-zero packet instead, followed 2 ms later by RMI attention `0c 02 01`. In RMI4
  terms, `02 01` reads as "F01 interrupt, device status = reset occurred".
- `GET_REPORT` always NACKs (`-121`), even right after a resync, while `SET_REPORT`
  works. The firmware can't report its own Input Mode, so the mode change can only be
  inferred from the input reports.

**Second capture** (45 min on battery, resync on mouse report only):

- 11 reset announcements, always the same two packets. All 5 mouse-mode switches came
  after one, and each was fixed within about 60 ms.
- The first reset in a chain comes about 25-30 s after the last touch. Mouse reports
  follow when you next touch the pad.
- Not every long idle triggers it: gaps of 46-128 s often passed without a reset.
- No system event correlates with the resets (nothing in the journal within 15 s). This
  looks internal to the firmware.

**Third capture** (8 h of normal use, resync on reset + mouse, idle and resume
triggers off):

- 210 resets, 210 resyncs, **0 mouse reports**. Gestures never broke.
- 4 s2idle suspend/resume cycles, all clean without the resume trigger. The core resume
  path already sends `SET_POWER(ON)` and runs `reset_resume()`.
- While idle, the firmware keeps resetting every 20-70 s (190 of 210 resets had no touch
  since the previous one). That's thousands of resyncs a day, so the fix logs them at
  debug level only.

## The Fix

`i2c-hid-core.c` queues a resync when any of these arrives:

- a zero-length input report with no reset pending (the spec's announcement),
- an all-zero packet, or RMI attention `0c 02 01` (this firmware's announcement),
- a mouse report `0x02` (safety net, in case an announcement is missed).

A one-second holdoff makes the two-packet announcement queue a single resync.

## Interrupt Load

An unrelated observation: the I2C controller (`i2c_designware`) raises about 63
interrupts per touchpad report, about 2,000/s while a finger is on the pad and close to
none when idle. That comes from the standard I2C controller driver interrupting roughly
once per received byte, plus `i2c-hid` reading the full 64-byte `wMaxInputLength` (sized
for the RMI reports) when a touchpad report is 32 bytes. The stock modules behave the
same. The touchpad's IRQ thread used about 0.15% of one core over 8 hours.

## Reproducing the Investigation

The instrumented build (feature dumps, idle-gap and report-switch logging, Input Mode
polling, `syna-debug.sh`) lives in commit `c711351`:

```sh
git switch --detach c711351
```

Its `DEBUG.md` has the full test protocol.
