# SYNA2BA6 Root-Cause Debugging

This branch (`debug/syna-root-cause`) adds instrumentation to `i2c_hid`. The goal is to
find out *why* the touchpad loses multitouch gestures, not just to recover from it.
The resync quirk from `master` is still there and still on by default.

## Hypotheses

The resync fixes the problem by re-sending the Input Mode feature report **and**
releasing all contacts. Either of these failures would explain "pointer moves, gestures
don't":

| | Hypothesis | What the logs would show |
|---|---|---|
| A | Firmware falls back to mouse mode | Input switches from report `0x03` (touchpad) to `0x02` (mouse); Input Mode reads back `04 00` instead of `04 03` |
| A1 | ...because the device reset itself | A `device-initiated reset` line shortly before the switch. The stock driver ignores this event |
| B | A contact gets stuck | Reports stay on `0x03`; the last report before an idle gap still has a finger down |

Report IDs come from this device's report descriptor:

| ID | Type | Content |
|---|---|---|
| `0x02` | input | Mouse collection (relative X/Y, buttons) |
| `0x03` | input | Touchpad (Precision Touchpad) contacts |
| `0x04` | feature | Input Mode (`0x03` = touchpad, `0x00` = mouse) |
| `0x06` | feature | Surface switch / button switch |
| `0x0d` | feature | Latency mode |

## Findings So Far

**2026-10-10, first capture** (`observe` mode, quirk off):

```text
13:20:09  touchpad reports (0x03), healthy
          ... 29.5 s without touch ...
13:20:39  first packet after idle: 13 zero bytes (report ID 0x00)
          then RMI attention report 0x0c: 0c 02 01 00 ...
13:21:05  only mouse reports (0x02) from here on
13:21:37  manual resync -> touchpad reports again at 13:21:39
```

- **A confirmed, B ruled out (this occurrence).** During the broken window,
  `libinput record` saw 1,483 `REL_X/REL_Y` events on the Mouse node and none on the
  Touchpad node.
- **The firmware re-initializes after about 30 s without touch** and comes back in its
  default mouse mode.
- **No spec-style reset signal:** `unsolicited_resets=0`. The firmware announces the reset
  differently, with an all-zero packet followed by an RMI attention report. This touchpad
  also exposes Synaptics' RMI-over-HID channel (reports `0x09`-`0x0c`, plus feature
  `0x0f`, the same IDs as `hid-rmi`), which `hid-multitouch` ignores.
- **No mouse reports in healthy mode.** All 8,272 touchpad reports came before the reset.
  A mouse report is therefore a reliable signal that the mode was lost.
- **`GET_REPORT` always NACKs (`-121`)**, even right after a resync, while `SET_REPORT`
  works. This firmware probably doesn't support reading features back, so the feature
  dumps carry no information.

Open questions: does every mouse-mode switch come with the zero packet and `0x0c`? And
does resyncing on the first mouse report (`fix-test`) fix the bug for good?

## What Gets Logged

All lines start with `syna-dbg:`. To follow them live: `journalctl -kf | grep syna-dbg`.

- **Device-initiated reset:** a zero-length input report with no reset pending.
- **Report ID switch:** for example `input report 0x03 (touchpad) -> 0x02 (mouse)`.
  Switching to mouse also triggers a feature dump.
- **Idle gaps:** after more than `syna_dbg_gap_ms` (5 s) of silence, the last report
  before the gap and the first one after it, in raw bytes.
- **Feature dumps:** Input Mode, Surface/Button switch and Latency, read back from the
  device. A dump runs before and after every resync, after resume, on a mark, after a
  device-initiated reset and on a switch to mouse mode.
- **Suspend/resume:** the result of each `SET_POWER` and `reset_resume` call.
- **Input read errors:** these were silent before.
- **Reports other than touchpad/mouse:** for example the zero packet or the RMI
  attention report, with their raw bytes.

New parameters, all under `/sys/module/i2c_hid/parameters/`:

| Parameter | Default | Purpose |
|---|---|---|
| `syna_dbg_gap_ms` | `5000` | Idle-gap logging threshold, `0` disables |
| `syna_dbg_resync_on_reset` | `N` | Resync on a spec-style device-initiated reset |
| `syna_dbg_resync_on_mouse` | `N` | Resync on the first mouse-mode report (candidate fix) |
| `syna_dbg_poll_ms` | `0` | Read Input Mode every N ms and log changes |
| `syna_dbg_mark` | – | Write a note: logs it with counters and a feature dump |
| `syna_dbg_stats` | – | Read-only counters |

## Setup

Build, sign and load exactly as in the [README](./README.md), from this branch:

```sh
git switch debug/syna-root-cause
cd i2c-hid_standalone && make
# sign both .ko files (README, "Sign Modules"), then:
sudo modprobe -r i2c_hid_acpi i2c_hid
sudo insmod ./i2c-hid.ko && sudo insmod ./i2c-hid-acpi.ko
journalctl -k -n 50 | grep syna-dbg   # expect "debug instrumentation active"
```

For `record`, install the libinput tools: `sudo dnf install libinput-utils`.

## Test Protocol

### 1. Observe the unmodified bug

```sh
sudo ./syna-debug.sh observe     # quirk off, logging on
```

Use the laptop normally. Include idle periods (walk away for a few minutes) and a
suspend/resume or two. **As soon as gestures break:**

```sh
sudo ./syna-debug.sh mark "gestures broken"
sudo ./syna-debug.sh record 20   # do a two-finger scroll, then one-finger moves
sudo ./syna-debug.sh resync      # recover
sudo ./syna-debug.sh mark "after manual resync"
sudo ./syna-debug.sh collect
```

Send me the `syna-debug-*.log` and `syna-record-*.yml` files. Also note roughly what
happened before it broke: idle for how long, lid closed, suspend, on battery or AC.

### 2. Optional: watch Input Mode directly

If phase 1 doesn't settle it, poll Input Mode as well:

```sh
sudo sh -c 'echo 2000 > /sys/module/i2c_hid/parameters/syna_dbg_poll_ms'
```

This logs the moment the mode changes, even without input. The downside is that polling
keeps the device and the I2C bus awake, which **may hide the bug**. If the bug stops
happening while polling, that is itself a useful clue: it points at the firmware's
low-power state.

### 3. Test the candidate fix

```sh
sudo ./syna-debug.sh fix-test
```

The idle and resume triggers are off. The only recovery left is a resync on the first
mouse-mode report (or on a spec-style reset). Expect at most a few pixels of pointer
movement before gestures come back, and look for `mouse-mode report, queuing Synaptics
resync` in the log. If gestures survive a few days of normal use, including suspend,
this trigger can replace the idle timer on `master`.

### Back to normal

```sh
sudo ./syna-debug.sh restore
```

Or load the `master` build again.
