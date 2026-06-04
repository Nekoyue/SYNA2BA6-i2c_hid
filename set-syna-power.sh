#!/usr/bin/env bash

## Place the script under `/usr/local/bin/set-syna-power.sh`

## As a systemd service: this only fix issues when resuming from sleep
## (not touchpad broken after idle)
## /etc/systemd/system/touchpad-after-wake-fix.service
# [Unit]
# Description=Restart touchpad on resume
# After=suspend.target exit.target default.target

# [Service]
# User=root
# Type=simple
# ExecStart=/usr/local/bin/set-syna-power.sh

# [Install]
# WantedBy=suspend.target
# WantedBy=exit.target
# WantedBy=default.target

## keyd to bind to the Copilot key:
## /etc/keyd/default.conf
# [meta+shift]
# f23 = command(/usr/local/bin/set-syna-power.sh)


set -euo pipefail

device="i2c-SYNA2BA6:00"
driver="/sys/bus/i2c/drivers/i2c_hid_acpi"

log() {
  local message="$*"

  logger -t set-syna-power "$message"

  if [[ -t 2 ]]; then
    printf '%s\n' "$message" >&2
  fi
}

main() {
  if [[ ! -d "$driver" ]]; then
    log "missing driver path: $driver"
    exit 1
  fi

  ## Works similarly to `sudo modprobe -r i2c_hid_acpi && sudo modprobe i2c_hid_acpi`
  log "rebinding $device"

  if [[ -e "$driver/$device" ]]; then
    printf '%s\n' "$device" > "$driver/unbind"
  fi

  printf '%s\n' "$device" > "$driver/bind"

  log "finished rebinding $device"
}

main "$@"
