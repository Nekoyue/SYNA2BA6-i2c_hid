# Standalone I2C HID Modules for SYNA2BA6

Out-of-tree `i2c_hid` and `i2c_hid_acpi` kernel modules that keep multitouch working on
the Synaptics I2C HID touchpad `SYNA2BA6:00 06CB:CF00`.

**Problem:** after the system has been idle or suspended, the touchpad still moves the
pointer but loses multitouch gestures (e.g. two-finger scroll).

## Two Fixes

This project has two fixes, on two branches. Both recover the touchpad the same way, with
a [resync](#what-a-resync-does). They differ in *when* they resync.

| | **Root fix** (this branch, `reset-resync`) | **Fallback resync** (`master`) |
|---|---|---|
| Resyncs when | The firmware announces it reset itself | First touch after 30 s idle, and after resume |
| Based on | The [root cause](./ROOT-CAUSE.md) | Observed symptoms, no known cause |
| Timing | Right after the reset, usually while you aren't touching the pad | Once you touch the pad again, so the first moment of that touch can still be in mouse mode |
| How often | Only after an actual reset (about once a minute while idle) | After every 30 s pause, whether or not the device reset |

Use the **root fix** unless it misbehaves on your machine. The **fallback resync** is the
more conservative choice. You can also turn the fallback triggers on in this branch, on
top of the root fix ([Parameters](#parameters)).

To switch:

```sh
git switch reset-resync   # root fix
git switch master         # fallback resync
```

Then rebuild, re-sign and reload as described below.

## How It Works

### Root Cause

The touchpad firmware resets itself, mostly while the pad is idle, and comes back in its
default **mouse mode**. Linux doesn't recognize how this firmware announces the reset, so
nobody switches it back to touchpad mode. The pointer still moves (mouse reports), but
gestures are gone. Details and captures are in [ROOT-CAUSE.md](./ROOT-CAUSE.md).

### What a Resync Does

Both fixes use the same recovery. A resync sends `SET_POWER(ON)` to the device, then
runs the HID driver's `reset_resume()` hook. For `hid-multitouch` that releases all
contacts and re-sends the Input Mode feature report, which switches the touchpad back
into multitouch mode. The device is not reset, re-probed or rebound.

### Root Fix Triggers (On by Default)

| Trigger | Resyncs when | Parameter |
|---|---|---|
| Reset | The device announces it reset itself: the firmware's all-zero packet, its RMI "reset occurred" report, or the HID-spec zero-length report | `syna_resync_on_reset` |
| Mouse | A mouse-mode report arrives. In touchpad mode the device never sends one, so this is a safety net if a reset announcement is missed | `syna_resync_on_mouse` |

Both share a one-second holdoff, so one reset (two packets) queues one resync.

### Fallback Triggers (`master`'s, Off by Default Here)

| Trigger | Resyncs when | Parameter |
|---|---|---|
| Idle | First touchpad input after an idle gap | `syna_resync_after_idle_ms` |
| Resume | Kernel PM `complete` phase after system sleep | `syna_resync_after_resume` |

The resume trigger isn't needed with the root fix: the driver's normal resume path
already does the same as a resync.

### Manual Resync

Writing `Y` to `syna_resync_now` resyncs at any time, whichever fix is active. See the
[helper script](#helper-script) to bind this to a key.

All quirk code lives in the "Synaptics SYNA2BA6 resync" section of
[`i2c-hid_standalone/i2c-hid-core.c`](./i2c-hid_standalone/i2c-hid-core.c).

## Repository Layout

```text
i2c-hid_standalone/   kernel module sources (i2c-hid-core.c holds the quirk)
set-syna-power.sh     helper: manual resync, or unbind/bind with the stock modules
ROOT-CAUSE.md         investigation and findings behind the root fix
```

## Tested Environment

Written for Fedora with Secure Boot, but any distribution should work. Tested on:

- `7.0.10-201.fc44.x86_64` (fallback resync)
- `7.2.7-200.fc44.x86_64` (fallback resync, root fix)

## Build

Install kernel development files: `kernel-devel` on Fedora, `linux-headers` on other
distros. They must provide `/lib/modules/$(uname -r)/build`.

```sh
cd i2c-hid_standalone
make
```

This produces `i2c-hid.ko` and `i2c-hid-acpi.ko`. You will likely have to rebuild (and
re-sign) after each kernel upgrade.

To build against a specific kernel tree:

```sh
make KDIR=/usr/src/kernels/$(uname -r)
```

Clean generated files with `make clean`.

## Secure Boot (if enabled)

### MOK Setup (one-time)

Generate a local signing key:

```sh
openssl req -new -x509 -newkey rsa:2048 \
  -keyout MOK.priv \
  -outform DER \
  -out MOK.der \
  -nodes \
  -days 3650 \
  -subj "/CN=Local i2c-hid module signing/"
```

Enroll the public key:

```sh
sudo mokutil --import ./MOK.der
```

Reboot, then use the MOK manager screen to enroll the key. The enrollment survives
kernel upgrades.

### Sign Modules

After every rebuild, sign both modules:

```sh
/lib/modules/$(uname -r)/build/scripts/sign-file sha256 \
  ./MOK.priv ./MOK.der ./i2c-hid.ko
/lib/modules/$(uname -r)/build/scripts/sign-file sha256 \
  ./MOK.priv ./MOK.der ./i2c-hid-acpi.ko
```

Verify `vermagic` and signatures:

```sh
modinfo ./i2c-hid.ko | grep -E '^(name|vermagic|sig_id|signer|sig_hashalgo):'
modinfo ./i2c-hid-acpi.ko | grep -E '^(name|vermagic|sig_id|signer|sig_hashalgo):'
```

## Load

Unload the in-tree modules and insert the local pair:

```sh
sudo modprobe -r i2c_hid_acpi i2c_hid
sudo insmod ./i2c-hid.ko
sudo insmod ./i2c-hid-acpi.ko
```

Revert to the distribution modules:

```sh
sudo modprobe -r i2c_hid_acpi i2c_hid
sudo modprobe i2c_hid_acpi
```

For frequent kernel updates, prefer an akmods/DKMS package over manually replacing
files under `/lib/modules/<kernel>/`.

### Verify

Check that the out-of-tree module loaded and the touchpad re-enumerated:

```sh
lsmod | grep i2c_hid
sudo dmesg -w | grep -i 'i2c_hid\|SYNA2BA6\|hid-multitouch'
```

The expected log includes:

```text
i2c_hid: loading out-of-tree module taints kernel.
hid-multitouch ... SYNA2BA6:00 06CB:CF00 ... on i2c-SYNA2BA6:00
```

### Check the Root Fix

The counters show whether the root fix is keeping up:

```sh
cat /sys/module/i2c_hid/parameters/syna_resync_stats
```

```text
i2c-SYNA2BA6:00: resets=210 mouse_reports=0 resyncs=210
```

- `resets`: reset announcements the driver acted on. Expect dozens per hour while idle.
- `mouse_reports`: should stay at or near `0`. A growing count means resets are slipping
  past the reset trigger, and the mouse trigger is catching them.
- `resyncs`: all resyncs, from any trigger.

Resets are frequent, so the reset trigger and the resync itself only log at debug level.
The mouse, idle, resume and manual triggers each log a line naming the trigger. To log
every resync as well:

```sh
sudo sh -c 'echo "module i2c_hid +p" > /sys/kernel/debug/dynamic_debug/control'
```

## Parameters

All parameters live under `/sys/module/i2c_hid/parameters/`. Change one at runtime with:

```sh
sudo sh -c 'printf "%s\n" <value> > /sys/module/i2c_hid/parameters/<name>'
```

| Fix | Parameter | Default | Meaning |
|---|---|---|---|
| Root fix | `syna_resync_on_reset` | `Y` | Resync when the device announces a reset |
| Root fix | `syna_resync_on_mouse` | `Y` | Resync when a mouse-mode report arrives |
| Fallback | `syna_resync_after_idle_ms` | `0` | Resync on first input after this many ms idle; `0` disables. `master` uses `30000` |
| Fallback | `syna_resync_after_resume` | `N` | Resync after system resume. `master` uses `Y` |
| Manual | `syna_resync_now` | – | Write `Y` to resync now (write-only) |
| Stats | `syna_resync_stats` | – | Counters, see [Check the Root Fix](#check-the-root-fix) (read-only) |

To run both fixes together, turn the fallback triggers back on:

```sh
sudo sh -c 'printf "%s\n" 30000 > /sys/module/i2c_hid/parameters/syna_resync_after_idle_ms'
sudo sh -c 'printf "%s\n" Y > /sys/module/i2c_hid/parameters/syna_resync_after_resume'
```

To set them at load time, pass them to `insmod`:

```sh
sudo insmod ./i2c-hid.ko syna_resync_after_idle_ms=30000
```

If the module is installed and loaded with `modprobe` (e.g. via akmods/DKMS), use a
modprobe config such as `/etc/modprobe.d/i2c_hid.conf` instead:

```text
options i2c_hid syna_resync_after_idle_ms=30000
```

## Helper Script

[`set-syna-power.sh`](./set-syna-power.sh) triggers a manual resync when the custom
module is loaded (either branch). Otherwise it falls back to my oldest fix, which unbinds
and rebinds the device and works with the stock modules. `--rebind` always uses the unbind/bind path.
Unbind/bind leaves input unresponsive for a moment while the device comes back. The
script header has example keyd and systemd configurations.

## Credits

Originally based on [coiby/standalone_i2c_hid](https://github.com/coiby/standalone_i2c_hid),
with these changes:

- Uses the modern split Linux `drivers/hid/i2c-hid/` layout, with separate `i2c_hid`
  and `i2c_hid_acpi` modules.
- Trims the HID ID list and DMI quirk hooks to this single device.
- Resync recovery path, with the fallback triggers (`master`) and the root fix
  (`reset-resync`).

This project was created with the help of Codex/GPT-5.5 and Claude Code/Claude Opus 5.5.
