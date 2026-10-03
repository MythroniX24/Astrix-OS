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

  # -------------------------------------------------------------------------
  # ...and that it stops refusing only when told to, loudly.
  #
  # A gate that cannot be opened is not a gate: nobody ever learns whether a
  # kernel boots by refusing to try. So ASTRIX_ALLOW_UNVERIFIED_FLASH=1 has to
  # get past the same check, and it has to say loudly that it is doing so. Both
  # halves are asserted here, because a safety gate that opens silently is
  # worse than one that never opens.
  # -------------------------------------------------------------------------
  out="$(ASTRIX_ALLOW_UNVERIFIED_FLASH=1 bash scripts/flash-device.sh "${UNSUPPORTED}" --flash 2>&1)" && rc=0 || rc=$?
  if [ "$rc" -eq 2 ]; then
    err "the override did not get past gate 1 (still exit 2)"
    err "the gate must be openable, or no port can ever be started"
    FAILED=$((FAILED + 1))
  else
    pass "ASTRIX_ALLOW_UNVERIFIED_FLASH=1 gets past the boot-status gate"
  fi
  if printf '%s' "$out" | grep -q "UNVERIFIED"; then
    pass "the override says out loud that it is flashing unverified hardware"
  else
    err "the override flashes without warning; that is how a phone gets lost"
    FAILED=$((FAILED + 1))
  fi
  if printf '%s' "$out" | grep -q "ASTRIX_ALLOW_UNVERIFIED_FLASH"; then
    pass "it names the variable that did it, so it can be found in a shell history"
  else
    warn "the override does not name the variable it is acting on"
  fi
  # Reaching the fastboot stage without a phone must fail there and nowhere
  # earlier: if it fails on "fastboot not found" that is fine, but it must not
  # have printed a flash plan.
  if printf '%s' "$out" | grep -q "Commands:"; then
    err "it printed flash commands for an unbooted device"
    FAILED=$((FAILED + 1))
  else
    pass "still no flash command was emitted"
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

# ---------------------------------------------------------------------------
printf '\n'
banner_msg="the per-device port tool exists, and its gates hold"
printf '  %s\n' "$banner_msg"
# A device that has a profile but no way to make progress is a dead end with
# documentation. The port script is the other half of the promise: it must
# exist, it must explain the two routes, and it must refuse to flash without
# the same explicit override everything else uses.
PORT="scripts/port-redmi-8a.sh"
if [ ! -x "$PORT" ]; then
  err "no executable ${PORT}"
  dim "  docs/REDMI-8A.md points readers at it; it has to exist"
  FAILED=$((FAILED + 1))
else
  pass "${PORT} exists and is executable"

  if bash -n "$PORT" 2>/dev/null; then
    pass "it parses"
  else
    err "it does not parse"
    FAILED=$((FAILED + 1))
  fi

  out="$(bash "$PORT" status 2>&1)" && rc=0 || rc=$?
  if [ "$rc" -ne 0 ]; then
    err "'status' exited ${rc}"
    FAILED=$((FAILED + 1))
  else
    pass "'status' runs and reports"
  fi
  # The whole point of the script is that it names the two routes and is
  # explicit about what each one proves. Copy that loses and the page becomes
  # another promise nobody checked.
  for want in "android-host" "boot-test" "Adreno 505"; do
    if printf '%s' "$out" | grep -q "$want"; then
      pass "status mentions '${want}'"
    else
      err "status does not mention '${want}'"
      FAILED=$((FAILED + 1))
    fi
  done
  if printf '%s' "$out" | grep -qi "does not prove"; then
    pass "status says what the routes do NOT prove"
  else
    err "status never says what a route fails to prove"
    dim "  that omission is exactly how a port ends up oversold"
    FAILED=$((FAILED + 1))
  fi

  # The flash gate, on the device-specific path.
  out="$(bash "$PORT" boot-test stage 2>&1)" && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    err "boot-test stage exited 0 with no override"
    FAILED=$((FAILED + 1))
  else
    pass "boot-test stage refuses without the override (exit ${rc})"
  fi
  if printf '%s' "$out" | grep -q "ASTRIX_ALLOW_UNVERIFIED_FLASH"; then
    pass "it names the variable that unlocks it"
  else
    err "the refusal does not say how to proceed"
    FAILED=$((FAILED + 1))
  fi

  # With the override it must get past consent and stop at the missing phone,
  # which is the only honest place for it to stop.
  out="$(ASTRIX_ALLOW_UNVERIFIED_FLASH=1 bash "$PORT" boot-test stage 2>&1)" && rc=0 || rc=$?
  if printf '%s' "$out" | grep -qi "refusing"; then
    err "the override did not get past the consent gate"
    FAILED=$((FAILED + 1))
  else
    pass "the override gets past the consent gate"
  fi
  if printf '%s' "$out" | grep -qi "unverified"; then
    pass "it says out loud that the image is unverified"
  else
    err "the override flashes without warning"
    FAILED=$((FAILED + 1))
  fi
  if printf '%s' "$out" | grep -qiE "no boot image|no phone in fastboot|fastboot not found"; then
    pass "it then stops for a real, explicable reason (exit ${rc})"
  else
    err "after the override it failed for an unexplained reason"
    printf '%s\n' "$out" | sed 's/^/      /' | head -10
    FAILED=$((FAILED + 1))
  fi

  # build must never report success for an image it did not write. abootimg
  # leaves a zero-length file behind when it gives up, and a script that does
  # not look would print "ok" over nothing - which is how a phone ends up with
  # an unbootable boot partition and a green build log.
  out="$(bash "$PORT" boot-test build 2>&1)" && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    if printf '%s' "$out" | grep -q "valid Android boot image" ||
       printf '%s' "$out" | grep -q "from the stock header"; then
      pass "boot-test build validated the image it wrote"
    else
      err "boot-test build exited 0 without validating the image"
      FAILED=$((FAILED + 1))
    fi
  else
    pass "boot-test build fails cleanly with no kernel present (exit ${rc})"
    if printf '%s' "$out" | grep -qi "no kernel"; then
      pass "and it says which input is missing"
    else
      warn "the build failure does not name the missing input"
    fi
  fi
fi

# ---------------------------------------------------------------------------
printf '\n'
banner_msg="the redmi-8a profile records the routes it can actually offer"
printf '  %s\n' "$banner_msg"
(
  # shellcheck source=/dev/null
  source "config/devices/redmi-8a.conf"
  missing=""
  for k in DEVICE_TEST_ROUTES DEVICE_ANDROID_HOST_SUPPORTED DEVICE_BOOT_TEST_SUPPORTED \
           DEVICE_GUI_PORT_STEPS DEVICE_BOOT_TEST_STEPS DEVICE_BOOT_TEST_CONSOLE \
           DEVICE_BOOT_TEST_EXPECTS_DISPLAY DEVICE_BOOT_TEST_REFLOWS_ANDROID; do
    [ -n "${!k:-}" ] || missing="${missing} ${k}"
  done
  [ -z "$missing" ] || { err "missing:${missing}"; exit 1; }
  # "unsupported" and "you can still run the GUI today" have to coexist in one
  # file without either one quietly overwriting the other.
  [ "${DEVICE_BOOT_STATUS}" = "unsupported" ] || {
    err "an unported device must not claim supported"; exit 1; }
  [ "${DEVICE_VERIFIED_BOOT}" = "no" ] || {
    err "an unbooted device must not claim a verified boot"; exit 1; }
  # The display expectation is the field most likely to be quietly wrong, and
  # it is the one that decides whether the user looks at the right screen.
  [ "${DEVICE_BOOT_TEST_EXPECTS_DISPLAY}" = "no" ] || {
    err "a boot test on an Adreno 505 phone cannot expect a display"; exit 1; }
  exit 0
) 2>&1 | sed 's/^/      /' && pass "the profile is honest about both routes" || FAILED=$((FAILED + 1))

printf '\n'
if [ "$FAILED" -ne 0 ]; then
  die "${FAILED} device check(s) failed"
fi
ok "device profiles are consistent and the flasher refuses what it must"