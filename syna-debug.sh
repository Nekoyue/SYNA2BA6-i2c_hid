#!/usr/bin/env bash

## Drive the SYNA2BA6 debug build of i2c_hid (debug branch only, see DEBUG.md).
##
## Usage (as root):
##   syna-debug.sh observe          turn the resync quirk off so the bug shows
##                                  up unmodified; debug logging stays on
##   syna-debug.sh fix-test         quirk off, but resync whenever the device
##                                  resets itself (candidate root-cause fix)
##   syna-debug.sh restore          back to the default quirk settings
##   syna-debug.sh mark [note...]   log a note, counters and a feature dump
##   syna-debug.sh resync           recover the touchpad (manual resync)
##   syna-debug.sh record [secs]    libinput record of the Mouse + Touchpad
##                                  nodes (default 20 s, needs libinput-utils)
##   syna-debug.sh collect [file]   save parameters, counters and the kernel
##                                  log of this boot to a file
##   syna-debug.sh status           show parameters and counters

set -euo pipefail

params="/sys/module/i2c_hid/parameters"

die() {
  printf '%s\n' "$*" >&2
  exit 1
}

need_debug_build() {
  [[ -e "$params/syna_dbg_stats" ]] ||
    die "debug build of i2c_hid is not loaded ($params/syna_dbg_stats missing)"
}

set_param() {
  printf '%s\n' "$2" > "$params/$1"
}

show_status() {
  local p

  for p in syna_resync_after_idle_ms syna_resync_after_resume \
           syna_dbg_resync_on_reset syna_dbg_gap_ms syna_dbg_poll_ms; do
    printf '%-28s %s\n' "$p" "$(cat "$params/$p")"
  done
  printf '\n'
  cat "$params/syna_dbg_stats"
}

event_node() {
  # Print /dev/input/eventN for the SYNA2BA6 node whose name ends in $1
  awk -v want="$1" '
    /^N: Name=/ { hit = ($0 ~ /SYNA2BA6/ && $0 ~ want "\"$") }
    hit && /^H: Handlers=/ {
      for (i = 1; i <= NF; i++)
        if ($i ~ /^event[0-9]+$/) { print "/dev/input/" $i; exit }
    }' /proc/bus/input/devices
}

give_to_user() {
  if [[ -n "${SUDO_USER:-}" ]]; then
    chown "$SUDO_USER:" "$1"
  fi
}

main() {
  local cmd="${1:-status}"
  [[ $# -gt 0 ]] && shift

  need_debug_build

  case "$cmd" in
    observe)
      set_param syna_resync_after_idle_ms 0
      set_param syna_resync_after_resume N
      set_param syna_dbg_resync_on_reset N
      show_status
      ;;
    fix-test)
      set_param syna_resync_after_idle_ms 0
      set_param syna_resync_after_resume N
      set_param syna_dbg_resync_on_reset Y
      show_status
      ;;
    restore)
      set_param syna_resync_after_idle_ms 30000
      set_param syna_resync_after_resume Y
      set_param syna_dbg_resync_on_reset N
      set_param syna_dbg_poll_ms 0
      show_status
      ;;
    mark)
      set_param syna_dbg_mark "${*:-mark}"
      sleep 0.5
      journalctl -k -n 20 --no-pager | grep -E 'syna-dbg' || true
      ;;
    resync)
      set_param syna_resync_now Y
      ;;
    record)
      local secs="${1:-20}" mouse touchpad out

      command -v libinput >/dev/null && libinput record --help >/dev/null 2>&1 ||
        die "libinput record missing, install it with: sudo dnf install libinput-utils"

      mouse="$(event_node Mouse)"
      touchpad="$(event_node Touchpad)"
      [[ -n "$mouse" && -n "$touchpad" ]] || die "SYNA2BA6 input nodes not found"

      out="syna-record-$(date +%Y%m%d-%H%M%S).yml"
      printf 'Recording %s (Mouse) and %s (Touchpad) for %s s into %s\n' \
        "$mouse" "$touchpad" "$secs" "$out"
      printf 'Try a two-finger scroll, then one-finger moves.\n'
      timeout --signal=INT "$secs" \
        libinput record --multiple -o "$out" "$mouse" "$touchpad" || true
      give_to_user "$out"
      printf 'Saved %s\n' "$out"
      ;;
    collect)
      local out="${1:-syna-debug-$(date +%Y%m%d-%H%M%S).log}"

      {
        printf '# %s\n# %s\n\n' "$(date --iso-8601=seconds)" "$(uname -r)"
        show_status
        printf '\n# kernel log, this boot\n'
        journalctl -k -b -o short-precise --no-pager |
          grep -E 'syna-dbg|i2c_hid|SYNA2BA6|hid-multitouch|PM: |ACPI: PM|Freezing|Restarting tasks' ||
          true
      } > "$out"
      give_to_user "$out"
      printf 'Saved %s\n' "$out"
      ;;
    status)
      show_status
      ;;
    *)
      die "unknown command: $cmd (see the header of $0)"
      ;;
  esac
}

main "$@"
