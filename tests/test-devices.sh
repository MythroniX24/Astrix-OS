#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Device profiles must not tell a comfortable lie.
#
# Why this test exists
# -------------------
# A device profile is the only thing standing between "I read this on a forum"
# and "Astrix supports this phone". Getting that wrong costs somebody a phone,
# so the claims in these files are treated as assertions that have to be true,
# and this test fails the build if one is not.
#
# It also proves the flashing tool actually refuses. A safety gate that has
# never been observed refusing is not a safety gate; it is a comment. So the
# test runs flash-device.sh against a real profile and requires it to exit
# non-zero WITHOUT a device attached - which is the situation where a careless
# script would happily start erasing things.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
# shellcheck source=../scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"

DEVICE_DIR="config/devices"
FAILED=0

if [ ! -d "$DEVICE_DIR" ]; then
  die "no device profiles at ${DEVICE_DIR}"
fi

PROFILES=("${DEVICE_DIR}"/*.conf)
[ -e "${PROFILES[0]}" ] || die "no device profiles found in ${DEVICE_DIR}"

# ---------------------------------------------------------------------------
printf '\n'
for p in "${PROFILES[@]}"; do
  name="$(basename "$p" .conf)"
  printf '  %s\n' "$name"

  # Load the profile in a subshell so one profile cannot leak variables into
  # the next one. Without this, checking two devices silently checks the
  # second one against the first one's values.
  check_profile() {
    # shellcheck source=/dev/null
    source "$p"

    local missing=""
    local k
    for k in DEVICE_NAME DEVICE_CODENAME DEVICE_SOC DEVICE_ARCH \
             DEVICE_PANEL_WIDTH DEVICE_PANEL_HEIGHT DEVICE_PANEL_REFRESH \
             DEVICE_GPU DEVICE_GPU_DRIVER_MAINLINE DEVICE_BOOTLOADER_UNLOCK \
             DEVICE_PARTITION_SCHEME DEVICE_BOOT_STATUS DEVICE_VERIFIED_BOOT \
             DEVICE_DSI_LANES DEVICE_DSI_MAX_LINK_RATE \
             DEVICE_PANEL_HBLANK_PERCENT DEVICE_PANEL_VBLANK_PERCENT \
             DEVICE_DISPLAY_DATA_PROVENANCE \
             DEVICE_REQUIRED_KERNEL_FEATURES DEVICE_DOWNSTREAM_ONLY_FEATURES; do
      [ -n "${!k:-}" ] || missing="${missing} ${k}"
    done
    if [ -n "$missing" ]; then
      err "missing required key(s):${missing}"
      return 1
    fi
    pass "all required keys present"

    # The one that actually matters.
    if [ "${DEVICE_BOOT_STATUS}" = "supported" ] && [ "${DEVICE_VERIFIED_BOOT}" != "yes" ]; then
      err "claims supported but DEVICE_VERIFIED_BOOT=${DEVICE_VERIFIED_BOOT}"
      dim "  no physical device has been booted by Astrix as of this commit"
      dim "  claiming otherwise would be the one kind of lie this project does not tell"
      return 1
    fi
    pass "boot claim is not stronger than the evidence (${DEVICE_BOOT_STATUS}/${DEVICE_VERIFIED_BOOT})"

    if [ "${DEVICE_BOOT_STATUS}" = "unsupported" ] && [ -z "${DEVICE_BOOT_BLOCKERS:-}" ]; then
      err "unsupported but no DEVICE_BOOT_BLOCKERS recorded"
      return 1
    fi
    if [ -n "${DEVICE_BOOT_BLOCKERS:-}" ]; then
      pass "blockers recorded: ${DEVICE_BOOT_BLOCKERS}"
    fi

    [ "${DEVICE_ARCH}" = "arm64" ] || { err "arch ${DEVICE_ARCH}, expected arm64"; return 1; }
    [ "${DEVICE_PANEL_WIDTH}" -ge 320 ] || { err "panel width implausible"; return 1; }
    pass "arch and panel are plausible (${DEVICE_ARCH}, ${DEVICE_PANEL_WIDTH}x${DEVICE_PANEL_HEIGHT})"

    # Every feature listed as upstream-available must be one the checker can
    # actually verify. A feature it cannot check is a claim nobody is testing.
    local f
    for f in $(printf '%s' "${DEVICE_REQUIRED_KERNEL_FEATURES}" | tr ',' ' '); do
      case "$f" in
        panfrost|mmc-mtk|ufs-mediatek|phy-mtk-ufs|i2c-mtk|mtk-smi|mtk-thermal|\
thermal-mtk|mtk-dvfsrc|regulator-mt6392|ufshc|ufs-qcom|phy-qcom-qmp|\
phy-qcom-qcom-mmp|regulator-qcom-rpmh|watchdog-qcom-wdt|clk-qcom|thermal-qcom|\
drm-mipi-dsi|drm-mipi-dsi-phy|drm-panel-of|drm-panel-bridge|drm-fbdev|\
uart-mtk|uart-8250-dw|uart-msm|earlycon) ;;
        *) err "'${f}' is listed as upstream but scripts/device.sh cannot check it"
           dim "  either add a case to device.sh, or move it to DEVICE_DOWNSTREAM_ONLY_FEATURES"
           return 1 ;;
      esac
    done
    pass "every upstream feature is actually verified by scripts/device.sh"
  }

  if check_profile 2>&1 | sed 's/^/      /'; then
    :
  else
    FAILED=$((FAILED + 1))
  fi
done

# ---------------------------------------------------------------------------
printf '\n'
banner_msg="flashing tool refuses an unsupported device"
printf '  %s\n' "$banner_msg"

# Pick any unsupported profile and require the flasher to refuse it *with no
# device attached*. If the script ever gets to the fastboot stage here, that is
# a bug worth failing the build over.
UNSUPPORTED=""
for p in "${PROFILES[@]}"; do
  dev="$(basename "$p" .conf)"
  status="$( ( source "$p"; printf '%s' "${DEVICE_BOOT_STATUS}" ) )"
  if [ "$status" = "unsupported" ]; then
    UNSUPPORTED="$dev"
    break
  fi
done

if [ -z "$UNSUPPORTED" ]; then
  # Every profile claims support: that is a thing worth knowing, not a failure.
  info "  no unsupported profile to test against; every known device claims support"
  warn "  if that is not true yet, the profiles are lying and this test cannot help"
else
  out="$(bash scripts/flash-device.sh "${UNSUPPORTED}" --flash 2>&1)" && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    err "flash-device.sh ${UNSUPPORTED} --flash exited 0"
    err "it must refuse an unsupported device even with --flash"
    FAILED=$((FAILED + 1))
  else
    pass "refused to flash ${UNSUPPORTED} (exit ${rc})"
  fi
  if printf '%s' "$out" | grep -qi "fastboot flash"; then
    err "it emitted fastboot commands for an unsupported device"
    FAILED=$((FAILED + 1))
  else
    pass "no fastboot command was emitted"
  fi
  if printf '%s' "$out" | grep -q "not supported"; then
    pass "it says why, in words, instead of failing silently"
  else
    warn "the refusal does not explain itself"
  fi
fi

# ---------------------------------------------------------------------------
printf '\n'
banner_msg="the flash-bundle builder refuses an unsupported device too"
printf '  %s\n' "$banner_msg"
# build-device.sh is the other half of the same promise: it must not assemble
# a bootable bundle for hardware nobody has booted. Both tools were written to
# refuse, and neither had ever been observed refusing - until now.
if [ -z "$UNSUPPORTED" ]; then
  info "  no unsupported profile to test against; skipping"
else
  out="$(bash scripts/build-device.sh "${UNSUPPORTED}" 2>&1)" && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    err "build-device.sh ${UNSUPPORTED} exited 0"
    err "it must refuse to build a bundle for a device that has never booted"
    FAILED=$((FAILED + 1))
  else
    pass "refused to build a bundle for ${UNSUPPORTED} (exit ${rc})"
  fi
  if printf '%s' "$out" | grep -q "Refusing to build"; then
    pass "it says why, in words"
  else
    warn "the refusal does not explain itself"
  fi
  if [ -d "build/device/${UNSUPPORTED}" ]; then
    err "it left a bundle behind at build/device/${UNSUPPORTED}"
    FAILED=$((FAILED + 1))
  else
    pass "no bundle was written to disk"
  fi
fi

printf '\n'
if [ "$FAILED" -ne 0 ]; then
  die "${FAILED} device check(s) failed"
fi
ok "device profiles are consistent and the flasher refuses what it must"