#!/usr/bin/env bash

## Recover the SYNA2BA6 touchpad when multitouch gestures stop working.
## Place the script under `/usr/local/bin/set-syna-power.sh`
##
## Usage:
##   set-syna-power.sh            resync via the custom i2c_hid module's
##                                syna_resync_now parameter if it is loaded,
##                                otherwise fall back to unbind/bind
##   set-syna-power.sh --rebind   always unbind/bind the device (works with
##                                the stock modules, but input stalls briefly)

## keyd to bind to the Copilot key:
## /etc/keyd/default.conf
# [meta+shift]
# f23 = command(/usr/local/bin/set-syna-power.sh)

## As a systemd service (stock modules only; the custom module reconfigures the
## touchpad itself, see the README). This only fixes issues
## when resuming from sleep, not the touchpad breaking after idle.
## /etc/systemd/system/touchpad-after-wake-fix.service
# [Unit]
# Description=Restart touchpad on resume
# After=suspend.target exit.target default.target

# [Service]
# User=root
# Type=simple
# ExecStart=/usr/local/bin/set-syna-power.sh --rebind

# [Install]
# WantedBy=suspend.target
# WantedBy=exit.target
# WantedBy=default.target


set -euo pipefail

device="i2c-SYNA2BA6:00"
driver="/sys/bus/i2c/drivers/i2c_hid_acpi"
manual_resync="/sys/module/i2c_hid/parameters/syna_resync_now"

log() {
  local message="$*"

  logger -t set-syna-power "$message"

  if [[ -t 2 ]]; then
    printf '%s\n' "$message" >&2
  fi
}

main() {
  local rebind=false

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --rebind)
        rebind=true
        ;;
      *)
        log "unknown argument: $1"
        exit 2
        ;;
    esac
    shift
  done

  if [[ "$rebind" == false && -e "$manual_resync" ]]; then
    log "triggering manual Synaptics wake/resync via $manual_resync"
    printf '%s\n' Y > "$manual_resync"
    log "finished manual Synaptics wake/resync trigger"
    return
  fi

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
