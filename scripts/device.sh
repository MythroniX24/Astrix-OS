#!/usr/bin/env bash
# Astrix OS - device support inspector.
#
# Answers one question honestly: "does Astrix boot on this phone?" - and when
# the answer is no, says exactly what is missing instead of leaving it to be
# discovered with the phone in fastboot.
#
#   ./scripts/device.sh list
#   ./scripts/device.sh show redmi-8a
#   ./scripts/device.sh check redmi-8a
#   ./scripts/device.sh check --verbose redmi-8a
#
# `check` verifies the profile is internally consistent and that every kernel
# feature the profile claims the SoC needs is actually enabled in
# config/kernel/config. That is a real check with real teeth: a profile that
# asks for a driver the kernel does not build is caught here rather than as a
# black screen on a phone.
#
# It deliberately does NOT claim a device boots. Only a profile with
# DEVICE_VERIFIED_BOOT=yes has ever been observed booting by anyone.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

# Overridable so the test suite can check deliberately broken fixture profiles.
DEVICE_DIR="${ASTRIX_DEVICE_DIR:-${ASTRIX_REPO_ROOT}/config/devices}"
KERNEL_CONFIG="${ASTRIX_REPO_ROOT}/config/kernel/config"

usage() {
  sed -n '2,22p' "$0"
  cat <<'EOF'

usage:
  device.sh list                    list every known device profile
  device.sh show <device>           print a profile
  device.sh check <device>          verify the profile and the kernel config
  device.sh kernel-features <device>  print kernel features the SoC requires
  device.sh display-budget <device>   compute the MIPI DSI link budget

<device> is a profile name, e.g. redmi-8a, moto-g64-5g.
EOF
}

list_devices() {
  local f
  printf '\n  %-24s %-18s %s\n' "PROFILE" "BOOT STATUS" "DEVICE"
  printf '  %-24s %-18s %s\n' "------------------------" "------------------" "------"
  for f in "${DEVICE_DIR}"/*.conf; do
    [ -e "$f" ] || continue
    (
      # shellcheck source=/dev/null
      source "$f"
      printf '  %-24s %-18s %s\n' "$(basename "$f" .conf)" "${DEVICE_BOOT_STATUS:-?}" \
        "${DEVICE_NAME:-?}"
    )
  done
  printf '\n'
  warn "boot status above is a claim, not a measurement."
  dim "Only a profile with DEVICE_VERIFIED_BOOT=yes has been seen booting by anyone."
  printf '\n'
}

load_profile() {
  local dev="$1"
  local path="${DEVICE_DIR}/${dev}.conf"
  [ -f "$path" ] || {
    err "no such device profile: ${dev}"
    dim "known: $(ls "${DEVICE_DIR}" | sed 's/\.conf$//' | tr '\n' ' ')"
    exit 1
  }
  # shellcheck source=/dev/null
  source "$path"
  DEVICE_PROFILE_PATH="$path"
}

show_profile() {
  load_profile "$1"
  printf '\n'
  printf '  %-22s %s\n' "profile" "$(basename "${DEVICE_PROFILE_PATH}")"
  printf '  %-22s %s\n' "device" "${DEVICE_NAME}"
  printf '  %-22s %s\n' "codename" "${DEVICE_CODENAME}"
  printf '  %-22s %s\n' "soc" "${DEVICE_SOC}"
  printf '  %-22s %sx%s @ %sHz\n' "panel" "${DEVICE_PANEL_WIDTH}" \
    "${DEVICE_PANEL_HEIGHT}" "${DEVICE_PANEL_REFRESH:-?}"
  printf '  %-22s %s\n' "gpu" "${DEVICE_GPU}"
  printf '  %-22s %s\n' "gpu driver (mainline)" "${DEVICE_GPU_DRIVER_MAINLINE}"
  printf '  %-22s %s\n' "bootloader unlock" "${DEVICE_BOOTLOADER_UNLOCK}"
  printf '  %-22s %s\n' "partitions" "${DEVICE_PARTITION_SCHEME}"
  printf '  %-22s %s\n' "BOOT STATUS" "${DEVICE_BOOT_STATUS}"
  printf '  %-22s %s\n' "verified boot" "${DEVICE_VERIFIED_BOOT}"
  if [ -n "${DEVICE_BOOT_BLOCKERS:-}" ]; then
    printf '  %-22s %s\n' "blockers" "${DEVICE_BOOT_BLOCKERS}"
  fi
  printf '\n'
  case "${DEVICE_BOOT_STATUS}" in
    unsupported)
      err "Astrix does NOT boot on this device."
      if [ -n "${DEVICE_PORT_STEPS:-}" ]; then
        printf '  the path to a boot, in order:\n'
        printf '  %s\n' "${DEVICE_PORT_STEPS}" | tr ',' '\n' | sed 's/^/    /'
      fi
      dim "  Reference ports exist for this board; see DEVICE_PORT_REFERENCES in the profile."
      printf '\n'
      ;;
    experimental|supported)
      printf '  claimed: %s\n' "${DEVICE_BOOT_STATUS}"
      [ "${DEVICE_VERIFIED_BOOT:-no}" = "yes" ] ||
        warn "not verified by an actual boot."
      printf '\n'
      ;;
  esac
}

kernel_features() {
  load_profile "$1"
  printf '%s\n' "${DEVICE_REQUIRED_KERNEL_FEATURES}" | tr ',' '\n' | sed '/^$/d'
}

# Verify the profile is coherent and that the kernel config actually enables
# the subsystems the profile says the SoC needs.
check_profile() {
  local dev="$1" verbose="${2:-no}"
  local failed=0
  load_profile "$dev"

  printf '\n  %s (%s)\n' "${DEVICE_NAME}" "$(basename "${DEVICE_PROFILE_PATH}")"

  # --- required keys present -------------------------------------------------
  local key
  for key in DEVICE_NAME DEVICE_CODENAME DEVICE_SOC DEVICE_ARCH \
             DEVICE_PANEL_WIDTH DEVICE_PANEL_HEIGHT DEVICE_GPU \
             DEVICE_GPU_DRIVER_MAINLINE DEVICE_BOOTLOADER_UNLOCK \
             DEVICE_BOOT_STATUS DEVICE_VERIFIED_BOOT \
             DEVICE_REQUIRED_KERNEL_FEATURES DEVICE_DOWNSTREAM_ONLY_FEATURES; do
    if [ -z "${!key:-}" ]; then
      err "missing required key: ${key}"
      failed=$((failed + 1))
    fi
  done

  # --- arch must match what we actually build -------------------------------
  if [ "${DEVICE_ARCH:-}" != "${ASTRIX_ARCH}" ]; then
    err "profile arch ${DEVICE_ARCH} != build arch ${ASTRIX_ARCH}"
    failed=$((failed + 1))
  else
    pass "architecture matches the build (${ASTRIX_ARCH})"
  fi

  # --- panel must be sane ----------------------------------------------------
  if [ "${DEVICE_PANEL_WIDTH}" -lt 320 ] || [ "${DEVICE_PANEL_HEIGHT}" -lt 320 ]; then
    err "panel ${DEVICE_PANEL_WIDTH}x${DEVICE_PANEL_HEIGHT} is not a phone panel"
    failed=$((failed + 1))
  else
    pass "panel ${DEVICE_PANEL_WIDTH}x${DEVICE_PANEL_HEIGHT}"
  fi

  # --- the display-path data must be present in full -------------------------
  # A profile that names a panel but cannot say what the link has to carry is
  # half a profile; the missing half is exactly the part a bring-up needs.
  local dkey
  for dkey in DEVICE_DSI_LANES DEVICE_DSI_MAX_LINK_RATE \
               DEVICE_PANEL_HBLANK_PERCENT DEVICE_PANEL_VBLANK_PERCENT; do
    if [ -z "${!dkey:-}" ]; then
      err "missing display data: ${dkey}"
      failed=$((failed + 1))
    fi
  done

  # --- a profile may not claim a boot nobody observed ------------------------
  # This is the check that matters most: it is the difference between a
  # project that is honest about its state and one that is not.
  if [ "${DEVICE_VERIFIED_BOOT}" = "yes" ] && [ "${DEVICE_BOOT_STATUS}" != "supported" ]; then
    err "DEVICE_VERIFIED_BOOT=yes but DEVICE_BOOT_STATUS=${DEVICE_BOOT_STATUS}"
    err "these contradict each other; fix the profile rather than the check"
    failed=$((failed + 1))
  elif [ "${DEVICE_BOOT_STATUS}" = "supported" ] && [ "${DEVICE_VERIFIED_BOOT}" != "yes" ]; then
    err "claims boot support with no verified boot"
    err "no physical device has been booted by Astrix. Set DEVICE_VERIFIED_BOOT=yes"
    err "only after an actual boot has been observed and recorded."
    failed=$((failed + 1))
  else
    pass "boot claims are consistent (status=${DEVICE_BOOT_STATUS}, verified=${DEVICE_VERIFIED_BOOT})"
  fi

  # --- unsupported devices must say why --------------------------------------
  if [ "${DEVICE_BOOT_STATUS}" = "unsupported" ] && [ -z "${DEVICE_BOOT_BLOCKERS:-}" ]; then
    err "unsupported but DEVICE_BOOT_BLOCKERS is empty"
    err "an unsupported device must state what is blocking it"
    failed=$((failed + 1))
  elif [ "${DEVICE_BOOT_STATUS}" = "unsupported" ]; then
    pass "blockers recorded: ${DEVICE_BOOT_BLOCKERS}"
  fi

  # --- can the panel physically be driven at all? ----------------------------
  # This is arithmetic on the vendor's own panel numbers, checked before any
  # driver work is started. A panel that does not fit the link is a design
  # problem, not a driver problem, and it is worth knowing first.
  local budget_out budget_rc=0 need
  budget_out="$(bash "${SCRIPT_DIR}/display-budget.sh" "$dev" 2>&1)" || budget_rc=$?
  need="$(printf '%s' "$budget_out" | sed -n 's/^  required per lane  *//p')"
  need="${need:-unknown}"
  case "$budget_rc" in
    0) pass "display link budget fits (needs ${need} per lane)" ;;
    1) err "the panel does not fit the DSI link at the configured rate"
       printf '%s\n' "$budget_out" | grep -E '^ +[0-9]' | sed 's/^/      /'
       failed=$((failed + 1))
       ;;
    *) err "could not compute a display budget for this profile"
       printf '%s\n' "$budget_out" | sed 's/^/      /'
       failed=$((failed + 1))
       ;;
  esac
  if printf '%s' "$budget_out" | grep -q "little headroom"; then
    warn "the budget fits but with little headroom; read the profile comment"
  fi
  if [ -z "${DEVICE_DISPLAY_DATA_PROVENANCE:-}" ]; then
    err "DEVICE_DISPLAY_DATA_PROVENANCE is not recorded"
    err "  display numbers must say where they came from, assumed or measured"
    failed=$((failed + 1))
  else
    pass "display data provenance recorded"
  fi

  # --- the kernel config must cover what the profile asks for ----------------
  # Each upstream-available feature maps to a CONFIG_ symbol that must be
  # enabled in config/kernel/config. A feature with no mapping is a gap in
  # this check, not a pass, so it is reported rather than silently skipped.
  local feat cfgsym
  for feat in $(printf '%s' "${DEVICE_REQUIRED_KERNEL_FEATURES}" | tr ',' ' '); do
    case "$feat" in
      panfrost)                    cfgsym="CONFIG_DRM_PANFROST" ;;
      mmc-mtk)                     cfgsym="CONFIG_MMC_MTK" ;;
      ufs-mediatek)                cfgsym="CONFIG_UFS_MEDIATEK" ;;
      phy-mtk-ufs)                 cfgsym="CONFIG_PHY_MTK_UFS" ;;
      i2c-mtk)                     cfgsym="CONFIG_I2C_MTK" ;;
      mtk-smi)                     cfgsym="CONFIG_MTK_SMI" ;;
      mtk-thermal)                 cfgsym="CONFIG_THERMAL_MTK" ;;
      thermal-mtk)                cfgsym="CONFIG_THERMAL_MTK" ;;
      mtk-dvfsrc)                  cfgsym="CONFIG_MTK_DVFSRC" ;;
      regulator-mt6392)            cfgsym="CONFIG_REGULATOR_MT6392" ;;
      mtk-scp)                     cfgsym="CONFIG_ARM_SCMI" ;;
      ufshc)                       cfgsym="CONFIG_UFSHCD" ;;
      ufs-qcom)                    cfgsym="CONFIG_UFS_QCOM" ;;
      phy-qcom-qmp)                cfgsym="CONFIG_PHY_QCOM_QMP" ;;
      phy-qcom-qcom-mmp)           cfgsym="CONFIG_PHY_QCOM_QCOM_MMP" ;;
      regulator-qcom-rpmh)         cfgsym="CONFIG_REGULATOR_QCOM_RPMH" ;;
      watchdog-qcom-wdt)           cfgsym="CONFIG_WATCHDOG_QCOM_WDT" ;;
      clk-qcom)                    cfgsym="CONFIG_COMMON_CLK_QCOM" ;;
      thermal-qcom)                cfgsym="CONFIG_THERMAL_QCOM" ;;
      drm-mipi-dsi)                cfgsym="CONFIG_DRM_MIPI_DSI" ;;
      drm-mipi-dsi-phy)            cfgsym="CONFIG_DRM_MIPI_DSI_PHY" ;;
      drm-panel-of)                cfgsym="CONFIG_DRM_PANEL_OF" ;;
      drm-panel-bridge)            cfgsym="CONFIG_DRM_PANEL_BRIDGE" ;;
      drm-fbdev)                   cfgsym="CONFIG_DRM_FBDEV_EMULATION" ;;
      uart-mtk)                    cfgsym="CONFIG_SERIAL_8250_MT6577" ;;
      uart-8250-dw)                cfgsym="CONFIG_SERIAL_8250_DW" ;;
      uart-msm)                    cfgsym="CONFIG_SERIAL_MSM" ;;
      earlycon)                    cfgsym="CONFIG_SERIAL_EARLYCON" ;;
      *)                           cfgsym="" ;;
    esac
    if [ -z "$cfgsym" ]; then
      err "no kernel-config mapping for required feature '${feat}'"
      err "  add a case to scripts/device.sh so this is actually checked"
      failed=$((failed + 1))
      continue
    fi
    if [ ! -f "$KERNEL_CONFIG" ]; then
      err "kernel config not found: $KERNEL_CONFIG"
      failed=$((failed + 1))
      break
    fi
    if grep -qE "^${cfgsym}=y" "$KERNEL_CONFIG"; then
      pass "${cfgsym} enabled (${feat})"
    else
      err "${cfgsym} is not enabled but the profile needs '${feat}'"
      err "  add ${cfgsym}=y to config/kernel/config"
      failed=$((failed + 1))
    fi
  done

  # --- features upstream does not have --------------------------------------
  # These are reported, not failed: they are the honest answer to "why doesn't
  # this boot", and pretending a .config could fix them would be the lie.
  local down
  for down in $(printf '%s' "${DEVICE_DOWNSTREAM_ONLY_FEATURES:-}" | tr ',' ' '); do
    warn "no upstream driver: ${down}"
  done
  if [ -n "${DEVICE_DOWNSTREAM_ONLY_FEATURES:-}" ]; then
    dim "  these are missing drivers, not missing config options. No .config"
    dim "  change can bring them into existence."
  fi

  if [ "${DEVICE_VERIFIED_BOOT}" != "yes" ]; then
    printf '\n'
    warn "This device has never been booted by Astrix."
    dim "Treat it as a porting target, not as a supported platform."
  fi
  printf '\n'
  return "${failed}"
}

CMD="${1:-}"
case "$CMD" in
  list) list_devices ;;
  show) [ -n "${2:-}" ] || { usage; exit 1; }; show_profile "$2" ;;
  check)
    [ -n "${2:-}" ] || { usage; exit 1; }
    verbose="no"; [ "${3:-}" = "--verbose" ] && verbose="yes"
    check_profile "$2" "$verbose"
    ;;
  kernel-features) [ -n "${2:-}" ] || { usage; exit 1; }; kernel_features "$2" ;;
  display-budget)
    [ -n "${2:-}" ] || { usage; exit 1; }
    bash "${SCRIPT_DIR}/display-budget.sh" "$2"
    ;;
  -h|--help|help|"") usage ;;
  *) err "unknown command: $CMD"; usage; exit 1 ;;
esac