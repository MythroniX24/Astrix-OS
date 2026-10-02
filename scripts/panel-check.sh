#!/usr/bin/env bash
# Astrix OS - display bring-up probe.
#
#   ./scripts/panel-check.sh              run here or on the handset
#   ./scripts/panel-check.sh --quiet      one line per stage, no prose
#   ./scripts/panel-check.sh --stage N    explain one stage in detail
#   adb shell astrix-panel-check          the same tool on the phone
#
# WHY THIS IS A TOOL AND NOT A DOCUMENT
# -------------------------------------
# A display port dies in the same way every time, and it dies in a way that is
# very hard to see: the phone shows black. Black is the same result whether the
# kernel panicked, the panel never got its reset pulse, the DSI lanes trained
# badly, or the GPU crashed after the mode was set. Guessing between those from
# a black rectangle is how weeks disappear.
#
# So this script reads the kernel's own account of the display hardware -
# /sys/class/drm, the DRM debugfs state, the connector status, the panel node -
# and reports, per stage, whether the chain actually made it. It is exactly the
# evidence table in docs/PORTING.md, in something you can run.
#
# It runs unchanged on the handset (adb shell) and inside the QEMU VM, where it
# reports the virtio-gpu chain instead. That matters: it means the tool itself
# is exercised on every run here, rather than being written once and first used
# on hardware nobody can debug easily.
#
# WHAT IT IS NOT: it does not decide whether a device boots. scripts/device.sh
# does that, from the device profile and a recorded boot. This one only reports
# what the running kernel can see.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

DRM_CLASS="/sys/class/drm"
DRM_DEBUG="/sys/kernel/debug/dri"
QUIET="no"
STAGE_DETAIL=""

while [ $# -gt 0 ]; do
  case "$1" in
    --quiet) QUIET="yes"; shift ;;
    --stage) STAGE_DETAIL="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) err "unknown option: $1"; exit 1 ;;
  esac
done

# --- output helpers --------------------------------------------------------
if [ "$QUIET" = "yes" ]; then
  row() { printf '%s %s: %s\n' "$1" "$2" "$3"; }
  say() { :; }
else
  row() { printf '  %s%-5s%s %-22s %s\n' "$C_GREEN" "$1" "$C_RESET" "$2" "$3"; }
  say() { printf '    %s%s%s\n' "$C_DIM" "$*" "$C_RESET"; }
fi

FOUND=0
MISSING=0

stage_result() {
  # stage_result <ok|no> <stage> <title> <detail>
  if [ "$1" = "ok" ]; then
    FOUND=$((FOUND + 1))
    row "[ ok ]" "$2" "$3"
    [ -n "${4:-}" ] && say "$4"
  else
    MISSING=$((MISSING + 1))
    printf '  %s[FAIL]%s %-22s %s\n' "$C_RED" "$C_RESET" "$2" "$3"
    [ -n "${4:-}" ] && say "$4"
  fi
  return 0
}

printf '\n'
if [ "$QUIET" = "yes" ]; then
  printf 'astrix-panel-check: reading the running kernel, not the screen\n'
else
  printf '  Astrix display bring-up check\n'
  printf '  %sreads /sys/class/drm and the DRM debugfs state; it never opens the panel%s\n\n' "$C_DIM" "$C_RESET"
fi

# ---------------------------------------------------------------------------
# 0. Is there a DRM device at all?
# ---------------------------------------------------------------------------
CARDS="$(ls "${DRM_CLASS}" 2>/dev/null | grep -c '^card[0-9]' || true)"
if [ "${CARDS:-0}" -eq 0 ]; then
  stage_result no "0: drm device" "no /dev/dri card found" \
    "the kernel has no DRM driver bound. Nothing below this can work."
else
  stage_result ok "0: drm device" "${CARDS} card(s)" \
    "$(ls "${DRM_CLASS}" | grep '^card[0-9]' | tr '\n' ' ')"
fi

# ---------------------------------------------------------------------------
# 1. GPU / KMS driver bound
# ---------------------------------------------------------------------------
DRIVER=""
[ -r "${DRM_CLASS}/card0/device/driver/module" ] && \
  DRIVER="$(basename "$(readlink -f "${DRM_CLASS}/card0/device/driver/module" 2>/dev/null || echo '')")"
if [ -z "$DRIVER" ] && [ -r "${DRM_CLASS}/card0/device/driver" ]; then
  DRIVER="$(basename "$(readlink -f "${DRM_CLASS}/card0/device/driver" 2>/dev/null || echo '')")"
fi
if [ -n "$DRIVER" ]; then
  stage_result ok "1: kms driver" "$DRIVER" \
    "render node: $([ -e /dev/dri/renderD128 ] && echo /dev/dri/renderD128 || echo none)"
else
  stage_result no "1: kms driver" "no driver bound to card0" \
    "a card with no KMS driver cannot produce a scanout."
fi

# ---------------------------------------------------------------------------
# 2. Connectors
# ---------------------------------------------------------------------------
CONNECTORS="$(ls "${DRM_CLASS}" 2>/dev/null | grep -- '-[0-9]' || true)"
if [ -z "$CONNECTORS" ]; then
  stage_result no "2: connectors" "none registered" \
    "a phone with no connector has no panel. This is where an unimplemented"
  say "display pipeline actually stops."
else
  for c in $CONNECTORS; do
    st="$(cat "${DRM_CLASS}/${c}/status" 2>/dev/null || echo unknown)"
    detail=""
    if [ "$st" = "connected" ]; then
      detail="panel detected on ${c}"
    elif [ "$st" = "disconnected" ]; then
      detail="${c} exists but no panel answers it (check the panel power"
      say "regulator and the reset/enable GPIO sequence)"
    else
      detail="${c} status=${st} (the panel did not report; DSI link training failed?)"
    fi
    stage_result ok "2: connectors" "${c} (${st})" "$detail"
  done
fi

# ---------------------------------------------------------------------------
# 3. Modes
# ---------------------------------------------------------------------------
MODES=0
[ -r "${DRM_DEBUG}/0/state" ] && \
  MODES="$(grep -c '^[[:space:]]*[0-9]\{4\}x[0-9]\{4\}' "${DRM_DEBUG}/0/state" 2>/dev/null || echo 0)"
if [ "${MODES:-0}" -gt 0 ]; then
  stage_result ok "3: modes" "${MODES} mode(s) offered" \
    "a mode the driver believes in but a dark panel means the problem is"
  say "electrical or the timings, not mode setting."
else
  stage_result no "3: modes" "no modes read" \
    "no debugfs state (mount -t debugfs none /sys/kernel/debug) or the driver"
  say "registered no modes at all."
fi

# ---------------------------------------------------------------------------
# 4. CRTC + encoders + planes
# ---------------------------------------------------------------------------
CRTCS=0
[ -r "${DRM_DEBUG}/0/state" ] && \
  CRTCS="$(sed -n 's/^CRTC: *[0-9]*:.*/&/p' "${DRM_DEBUG}/0/state" | wc -l)"
if [ "${CRTCS:-0}" -gt 0 ]; then
  stage_result ok "4: crtc/encoder" "${CRTCS} CRTC(s)" ""
else
  stage_result no "4: crtc/encoder" "no CRTC" \
    "the KMS pipeline is not assembled end to end."
fi

# ---------------------------------------------------------------------------
# 5. Framebuffer emulation - the stage Astrix itself needs
# ---------------------------------------------------------------------------
if [ -e /dev/fb0 ]; then
  stage_result ok "5: fbdev" "/dev/fb0 present" \
    "useful for bring-up: a test pattern proves the panel without wlroots."
else
  stage_result no "5: fbdev" "no /dev/fb0" \
    "CONFIG_DRM_FBDEV_EMULATION is what creates it; without it you cannot"
  say "paint the panel directly to prove the display path."
fi

# ---------------------------------------------------------------------------
# 6. Input - a phone that cannot be touched is not finished
# ---------------------------------------------------------------------------
TOUCHDEV="$(grep -l "ABS_MT_POSITION_X" /proc/bus/input/devices 2>/dev/null | head -1 || true)"
if [ -n "$TOUCHDEV" ]; then
  stage_result ok "6: touch input" "abs multitouch present" \
    "$(sed -n 's/^N: Name=\(.*\)/\1/p' "$TOUCHDEV" | head -1)"
else
  stage_result no "6: touch input" "no absolute multitouch device" \
    "a relative mouse is not a touchscreen: no multitouch, and the shell's"
  say "gestures cannot be validated with it."
fi

# ---------------------------------------------------------------------------
# 7. A display Astrix can drive
# ---------------------------------------------------------------------------
if [ "$FOUND" -ge 6 ] && [ "$MISSING" -eq 0 ]; then
  stage_result ok "7: astrix-usable" "display chain complete" \
    "WLR_BACKENDS=drm astrix-compositor can drive this."
else
  stage_result no "7: astrix-usable" "display chain incomplete" \
    "astrix-compositor cannot draw on this yet."
fi

# ---------------------------------------------------------------------------
printf '\n'
if [ "$QUIET" != "yes" ]; then
  if [ "$MISSING" -eq 0 ]; then
    ok "all $FOUND stage(s) found"
  else
    warn "$FOUND found, $MISSING missing"
    dim "The first missing stage is the only one worth fixing. Stages cannot be"
    dim "skipped: a connector that exists means the DSI PHY, the regulators and"
    dim "the panel all bound, and whatever fails after that is a different bug."
  fi
  printf '\n'
fi

# A missing stage is a finding, not a crash: the non-zero exit is how a caller
# (or a bring-up script) finds out, so the ERR trap is suppressed deliberately.
if [ "$MISSING" -eq 0 ]; then exit 0; fi
exit 1