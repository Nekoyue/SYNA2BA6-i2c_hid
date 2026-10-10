# Standalone I2C HID Modules for SYNA2BA6

Out-of-tree `i2c_hid` and `i2c_hid_acpi` kernel modules with a resync quirk for the
Synaptics I2C HID touchpad `SYNA2BA6:00 06CB:CF00`.

**Problem:** after the system has been idle or suspended, the touchpad still moves the
pointer but loses multitouch gestures (e.g. two-finger scroll).

**Fix attempt:** resync the touchpad automatically after idle and after system resume,
or manually on demand.

## How It Works

A resync sends `SET_POWER(ON)` to the device, then runs the HID driver's
`reset_resume()` hook. For `hid-multitouch` that releases all contacts and re-sends the
Input Mode feature report, which switches the touchpad back into multitouch mode. The
device is not reset, re-probed or rebound.

A resync is queued by any of these triggers:

| Trigger | When | Parameter |
|---|---|---|
| Idle | First touchpad input after an idle gap (default 30 s) | `syna_resync_after_idle_ms` |
| Resume | Kernel PM `complete` phase after system sleep | `syna_resync_after_resume` |
| Manual | Writing `Y` to the parameter | `syna_resync_now` |

All quirk code lives in the "Synaptics SYNA2BA6 resync" section of
[`i2c-hid_standalone/i2c-hid-core.c`](./i2c-hid_standalone/i2c-hid-core.c).

## Repository Layout

```text
i2c-hid_standalone/   kernel module sources (i2c-hid-core.c holds the quirk)
set-syna-power.sh     helper: manual resync, or unbind/bind with the stock modules
```

## Tested Environment

Written for Fedora with Secure Boot, but any distribution should work. Tested on:

- `7.0.10-201.fc44.x86_64`
- `7.2.7-200.fc44.x86_64`

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

Each resync logs `resyncing Synaptics device`, preceded by a line naming the trigger.

## Parameters

All parameters live under `/sys/module/i2c_hid/parameters/`.

Idle threshold in ms (default `30000`, `0` disables the idle trigger):

```sh
sudo sh -c 'printf "%s\n" 30000 > /sys/module/i2c_hid/parameters/syna_resync_after_idle_ms'
```

Resync after system resume (default `Y`):

```sh
sudo sh -c 'printf "%s\n" N > /sys/module/i2c_hid/parameters/syna_resync_after_resume'
sudo sh -c 'printf "%s\n" Y > /sys/module/i2c_hid/parameters/syna_resync_after_resume'
```

Manual resync:

```sh
sudo sh -c 'printf "%s\n" Y > /sys/module/i2c_hid/parameters/syna_resync_now'
```

## Helper Script

[`set-syna-power.sh`](./set-syna-power.sh) triggers a manual resync when the custom
module is loaded. Otherwise it falls back to my older fix, which unbinds and rebinds the
device and works with the stock modules. `--rebind` always uses the unbind/bind path.
Unbind/bind leaves input unresponsive for a moment while the device comes back. The
script header has example keyd and systemd configurations.

## Credits

Originally based on [coiby/standalone_i2c_hid](https://github.com/coiby/standalone_i2c_hid),
with these changes:

- Uses the modern split Linux `drivers/hid/i2c-hid/` layout, with separate `i2c_hid`
  and `i2c_hid_acpi` modules.
- Trims the HID ID list and DMI quirk hooks to this single device.
- New trigger logic and recovery path.

This project was created with the help of Codex/GPT-5.5 and Claude Code/Claude Opus 5.5.
