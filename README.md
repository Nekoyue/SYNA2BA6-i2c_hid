# Standalone I2C HID Modules for SYNA2BA6

This repository builds `i2c_hid` and `i2c_hid_acpi` kernel modules that 
add an idle wake/resync quirk for the Synaptics I2C HID touchpad
`SYNA2BA6:00 06CB:CF00`.

Attempt to fix: after the system has been idle or suspended, 
the touchpad still moves the pointer but loses multitouch gestures (e.g. two-finger scroll). 
The quirk queues a lightweight wake/resync on the first touchpad input after an
idle delay (defaults to 100s) and can be tuned with:

```sh
sudo sh -c 'printf "%s\n" 30000 > /sys/module/i2c_hid/parameters/syna_reprobe_after_idle_ms'
```

Set it to `0` to disable idle-triggered wake/resync.

I also include my old fix (rebind device) under [./set-syna-power.sh](./set-syna-power.sh), 
see script's header for usages.

## Credits
This project is based on the work originally done by
[coiby/standalone_i2c_hid](https://github.com/coiby/standalone_i2c_hid), with changes:

- Uses the modern split Linux `drivers/hid/i2c-hid/` layout, with separate
  `i2c_hid` and `i2c_hid_acpi` modules.
- Trims the HID ID list and DMI quirk hooks to this single device.

This project was created with the help of Codex/GPT-5.5.

## Tested Environment

Written for Fedora (7.0.10-201.fc44.x86_64) with Secure Boot, 
but any distribution should work.

Constraints:

- A built `.ko` only loads into the exact kernel ABI it was built for. The
  module `vermagic` must match the running kernel.
- Secure Boot requires locally built modules to be signed with an enrolled key.

## Check Kernel Headers

Install kernel development files for the exact kernel you want to build for. On
Fedora this usually means booting the target kernel, then installing matching
`kernel-devel`.

On other distributions, install the package that provides the build directory
for the running kernel, usually available through `/lib/modules/$(uname -r)/build`.

## Build

You will likely have to rebuild and optionally re-sign the kernel modules after
each kernel upgrade.

```sh
cd i2c-hid_standalone
make
```

To build against a specific kernel tree:

```sh
make KDIR=/usr/src/kernels/$(uname -r)
```

Produces `i2c-hid.ko` and `i2c-hid-acpi.ko`.

Clean generated files with:

```sh
make clean
```

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

Enroll the public key once:

```sh
sudo mokutil --import ./MOK.der
```

Reboot, then use the MOK manager screen to enroll the key. The enrollment
survives kernel upgrades.

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

## Load Kernel Modules

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

For frequent kernel updates, prefer an akmods/DKMS package over manually replacing files 
under `/lib/modules/<kernel>/` instead.

### Verify with dmesg

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
