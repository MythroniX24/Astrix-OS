#!/usr/bin/env bash
# Astrix OS - flash a device.
#
#   ./scripts/flash-device.sh <device> --dry-run      show exactly what would run
#   ./scripts/flash-device.sh <device> --flash        actually do it
#
# READ THIS BEFORE USING --flash
# ---------------------------------
# This script erases partitions on a phone. It is written so that the
# destructive path is hard to reach by accident:
#
#   1. It refuses to run at all for any profile whose DEVICE_BOOT_STATUS is
#      not "supported". Both target phones are currently "unsupported", so
#      today this script will not flash either of them - which is correct.
#      Flashing a phone with a kernel that has no display driver is how you
#      get a device that only boots to a black screen over USB.
#   2. It refuses to flash a device whose bootloader is locked, because
#      fastboot flash on a locked bootloader either fails or, worse, succeeds
#      in ways the vendor did not intend.
#   3. It requires --flash (not implied) AND a typed confirmation of the exact
#      device codename.
#   4. --dry-run is the default shape of the output: every fastboot command is
#      printed, with the real arguments, before anything is sent.
#
# The failure mode this is defending against is a phone that no longer boots.
# That risk is not recoverable by us, so the safe path is the only path.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

DEVICE_DIR="${ASTRIX_REPO_ROOT}/config/devices"
BUNDLE_DIR="${ASTRIX_BUILD_DIR:-build}/device"
MODE="dry-run"
DEVICE=""

usage() {
  sed -n '2,26p' "$0"
  cat <<'EOF'

usage:
  flash-device.sh <device> --dry-run     print every command; change nothing
  flash-device.sh <device> --flash       flash for real (asks first)

Devices:
EOF
  ls "${DEVICE_DIR}" 2>/dev/null | sed 's/\.conf$//' | sed 's/^/  /'
  printf '\n'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) MODE="dry-run"; shift ;;
    --flash)   MODE="flash"; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) err "unknown option: $1"; usage; exit 1 ;;
    *)  DEVICE="$1"; shift ;;
  esac
done

[ -n "$DEVICE" ] || { usage; exit 1; }

PROFILE="${DEVICE_DIR}/${DEVICE}.conf"
if [ ! -f "$PROFILE" ]; then
  err "no such device profile: ${DEVICE}"
  dim "known: $(ls "${DEVICE_DIR}" | sed 's/\.conf$//' | tr '\n' ' ')"
  exit 1
fi
# shellcheck source=/dev/null
source "$PROFILE"

# ---------------------------------------------------------------------------
printf '\n  Device:  %s (%s)\n' "${DEVICE_NAME}" "${DEVICE_CODENAME}"
printf '  SoC:     %s\n' "${DEVICE_SOC}"
printf '  Panel:   %sx%s @ %sHz\n' "${DEVICE_PANEL_WIDTH}" "${DEVICE_PANEL_HEIGHT}" \
  "${DEVICE_PANEL_REFRESH}"
printf '  Boot:    %s (verified: %s)\n' "${DEVICE_BOOT_STATUS}" "${DEVICE_VERIFIED_BOOT}"
printf '  Unlock:  %s\n\n' "${DEVICE_BOOTLOADER_UNLOCK}"

# ---------------------------------------------------------------------------
# Gate 1: has anyone actually booted this?
# ---------------------------------------------------------------------------
if [ "${DEVICE_BOOT_STATUS}" != "supported" ] || [ "${DEVICE_VERIFIED_BOOT}" != "yes" ]; then
  err "Refusing to flash: this device is not supported by Astrix."
  printf '\n'
  dim "  status=${DEVICE_BOOT_STATUS} verified-boot=${DEVICE_VERIFIED_BOOT}"
  dim "  Nothing in this repository has booted on this hardware. Flashing it now"
  dim "  would very likely leave a phone that shows nothing."
  if [ -n "${DEVICE_BOOT_BLOCKERS:-}" ]; then
    printf '\n'
    dim "  What is actually blocking it:"
    printf '%s\n' "${DEVICE_BOOT_BLOCKERS}" | tr ',' '\n' | sed 's/^/    - /'
  fi
  if [ -n "${DEVICE_PORT_STEPS:-}" ]; then
    printf '\n'
    dim "  The path to a boot, in order:"
    printf '%s\n' "${DEVICE_PORT_STEPS}" | tr ',' '\n' | sed 's/^/    /'
  fi
  printf '\n'
  dim "  ./scripts/device.sh show ${DEVICE}   for the full picture"
  printf '\n'
  exit 2
fi

# ---------------------------------------------------------------------------
# Gate 2: is the bootloader unlocked?
# ---------------------------------------------------------------------------
command -v fastboot >/dev/null || die "fastboot not found (apt-get install android-tools-fastboot)"

log "Checking the bootloader state"
if ! fastboot devices | grep -q .; then
  die "no device in fastboot mode. Boot the phone to fastboot and retry."
fi

UNLOCKED="$(fastboot getvar unlocked 2>&1 | sed -n 's/.*unlocked: *//p' | head -1)"
case "$UNLOCKED" in
  yes|true) ok "bootloader is unlocked" ;;
  no|false)
    err "the bootloader is locked."
    printf '\n'
    dim "  Unlock it first: ${DEVICE_BOOTLOADER_UNLOCK}"
    printf '\n'
    dim "  Note that unlocking erases the device. There is no way around that,"
    dim "  and nothing in Astrix can shorten or bypass it."
    printf '\n'
    exit 3
    ;;
  *) die "could not determine the bootloader state (got '${UNLOCKED}'). Refusing to continue." ;;
esac

# ---------------------------------------------------------------------------
# Gate 3: an explicit, typed confirmation.
# ---------------------------------------------------------------------------
if [ "$MODE" != "flash" ]; then
  printf '\n'
  warn "DRY RUN - nothing will be written to the device."
  printf '\n'
fi

BUNDLE="${BUNDLE_DIR}/${DEVICE}"
[ -d "$BUNDLE" ] || die "no flash bundle for ${DEVICE} at ${BUNDLE}. Run scripts/build-device.sh first."

run() {
  printf '  %s' "$*"
  if [ "$MODE" = "flash" ]; then
    printf '\n'
    "$@"
  else
    printf '   # not executed (dry run)\n'
  fi
}

printf '\n  Commands:\n'
run fastboot flash boot "${BUNDLE}/boot.img"
if [ -f "${BUNDLE}/dtb.img" ]; then
  run fastboot flash dtbo "${BUNDLE}/dtb.img"
fi
run fastboot flash userdata "${BUNDLE}/rootfs.img"

printf '\n'
warn "The userdata flash above erases the device."

if [ "$MODE" != "flash" ]; then
  printf '\n'
  ok "dry run complete; nothing was written."
  dim "Re-run with --flash to actually do this."
  printf '\n'
  exit 0
fi

printf '  Type the device codename (%s) to continue, anything else aborts: ' "${DEVICE_CODENAME}"
read -r CONFIRM
if [ "$CONFIRM" != "${DEVICE_CODENAME}" ]; then
  printf '\n'
  ok "aborted; nothing was written."
  exit 0
fi

printf '\n'
log "Flashing ${DEVICE_NAME} - this erases the device"
fastboot flash boot "${BUNDLE}/boot.img"
[ -f "${BUNDLE}/dtb.img" ] && fastboot flash dtbo "${BUNDLE}/dtb.img"
fastboot flash userdata "${BUNDLE}/rootfs.img"
fastboot reboot

printf '\n'
ok "flashed. Serial console output is the only proof of a successful boot;"
warn "a phone that shows a black screen has not booted, however clean the flash was."
printf '\n'