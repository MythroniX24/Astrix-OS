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
  # Clear the directory first. The property under test is "build-device.sh
  # writes nothing", not "this path has never existed on this machine" -
  # scripts/port-<device>.sh android-host pack legitimately writes a different
  # bundle to the same place, and a developer who ran that first used to fail
  # this check for doing the right thing.
  rm -rf "build/device/${UNSUPPORTED}"
  out="$(bash scripts/build-device.sh "${UNSUPPORTED}" 2>&1)" && rc=0 || rc=$?
  if [ "$rc" -eq 0 ]; then
    err "build-device.sh ${UNSUPPORTED} exited 0 on a second run"
    FAILED=$((FAILED + 1))
  else
    pass "it refuses consistently, not just the first time"
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
banner_msg="every device profile has a port tool, and its gates hold"
printf '  %s\n' "$banner_msg"
# A device that has a profile but no way to make progress is a dead end with
# documentation. The port script is the other half of the promise: it must
# exist, it must explain the routes, and it must refuse to flash without the
# same explicit override everything else uses. Loop over every device, so a
# new profile cannot arrive without one.
for p in "${PROFILES[@]}"; do
  dev="$(basename "$p" .conf)"
  PORT="scripts/port-${dev}.sh"
  printf '\n'
  printf '  %s\n' "${PORT}"

  if [ ! -x "$PORT" ]; then
    err "no executable ${PORT}"
    dim "  the device docs point readers at it; it has to exist"
    FAILED=$((FAILED + 1))
    continue
  fi
  pass "exists and is executable"

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
  # The whole point of the script is that it names the routes and is explicit
  # about what each one proves. Copy that loses and the page becomes another
  # promise nobody checked.
  for want in "android-host" "boot-test"; do
    if printf '%s' "$out" | grep -q "$want"; then
      pass "status mentions '${want}'"
    else
      err "status does not mention '${want}'"
      FAILED=$((FAILED + 1))
    fi
  done
  gpu="$( ( source "$p"; printf '%s' "${DEVICE_GPU}" ) )"
  if printf '%s' "$out" | grep -qF "$gpu"; then
    pass "status names the GPU (${gpu})"
  else
    err "status does not name the GPU; a reader cannot tell why the screen stays dark"
    FAILED=$((FAILED + 1))
  fi
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
done

# ---------------------------------------------------------------------------
printf '\n'
banner_msg="each profile records the routes it can actually offer"
printf '  %s\n' "$banner_msg"
for p in "${PROFILES[@]}"; do
  dev="$(basename "$p" .conf)"
  (
    # shellcheck source=/dev/null
    source "$p"
    missing=""
    for k in DEVICE_TEST_ROUTES DEVICE_ANDROID_HOST_SUPPORTED DEVICE_BOOT_TEST_SUPPORTED \
             DEVICE_GUI_PORT_STEPS DEVICE_BOOT_TEST_STEPS DEVICE_BOOT_TEST_CONSOLE \
             DEVICE_BOOT_TEST_EXPECTS_DISPLAY DEVICE_BOOT_TEST_REFLOWS_ANDROID \
             DEVICE_BOOT_TEST_DARK_REASON DEVICE_BOOT_TEST_CMDLINE; do
      [ -n "${!k:-}" ] || missing="${missing} ${k}"
    done
    [ -z "$missing" ] || { err "missing:${missing}"; exit 1; }
    # "unsupported" and "you can still run the GUI today" have to coexist in
    # one file without either one quietly overwriting the other.
    [ "${DEVICE_BOOT_STATUS}" = "unsupported" ] || {
      err "an unported device must not claim supported"; exit 1; }
    [ "${DEVICE_VERIFIED_BOOT}" = "no" ] || {
      err "an unbooted device must not claim a verified boot"; exit 1; }
    # The display expectation is the field most likely to be quietly wrong,
    # and it is the one that decides whether the user stares at a black
    # screen waiting for something that was never going to appear.
    [ "${DEVICE_BOOT_TEST_EXPECTS_DISPLAY}" = "no" ] || {
      err "an unported display path cannot expect a display"; exit 1; }

    # A vendor kernel that builds is NOT a device that boots. These two are the
    # claims most likely to be quietly collapsed into one, so the profile has to
    # state them separately: DEVICE_VENDOR_KERNEL_BUILDS is a fact about this
    # repo, DEVICE_VERIFIED_BOOT is a fact about a phone.
    if [ -n "${DEVICE_VENDOR_KERNEL_BUILDS:-}" ]; then
      [ "${DEVICE_VENDOR_KERNEL_REPO_BAD:-}" ] || {
        err "records a working vendor repo but not the unbuildable one it replaced"; exit 1; }
      # The known-bad repo must stay marked bad. Pointing back at it would
      # resurrect a 404-riddled tree as if it were usable.
      case "${DEVICE_VENDOR_KERNEL_REPO}" in
        *"redmi8a/android_kernel_xiaomi_olive"*) 
          err "DEVICE_VENDOR_KERNEL_REPO points at the incomplete tree"; exit 1 ;;
      esac
      # Building the kernel resolves nothing about driving the panel.
      [ "${DEVICE_VERIFIED_BOOT}" = "no" ] || {
        err "a building vendor kernel must not be reported as a verified boot"; exit 1; }
      [ "${DEVICE_BOOT_TEST_EXPECTS_DISPLAY}" = "no" ] || {
        err "a kernel that only builds must not be expected to light the panel"; exit 1; }

      # V32: the vendor device tree's preferred panel node does not match this
      # hardware. That is a known defect, so the profile has to say so - and the
      # requirement is unconditional. Making it conditional on the field already
      # being set would be a guard that switches itself off the moment it
      # matters: a profile that quietly dropped these keys would read like
      # "the panel timings are fine now", which is the opposite of the truth and
      # exactly the kind of thing that gets flashed to a phone.
      [ -n "${DEVICE_VENDOR_KERNEL_PANEL_PREFERRED:-}" ] || {
        err "does not record which panel node the vendor DT prefers (V32: it is wrong, and that has to be written down)"; exit 1; }
      [ -n "${DEVICE_VENDOR_KERNEL_PANEL_CORRECT:-}" ] || {
        err "records a preferred panel node but not the correct one to point it at"; exit 1; }
      [ -n "${DEVICE_VENDOR_KERNEL_PANEL_NOTE:-}" ] || {
        err "records a wrong preferred panel node with no explanation of why"; exit 1; }
      pass "${dev}: records the vendor DT's preferred panel node and the one to use instead"
    fi
    exit 0
  ) > /tmp/astrix-profile-check.$$ 2>&1 && \
    pass "${dev}: the profile is honest about its routes" || \
    { sed 's/^/      /' /tmp/astrix-profile-check.$$; FAILED=$((FAILED + 1)); }
  rm -f /tmp/astrix-profile-check.$$
done

# ---------------------------------------------------------------------------
# CI must not be allowed to drift into a claim.
#
# The failure mode this guards against is specific and not hypothetical: a
# green Actions badge sitting next to a device profile that says
# DEVICE_VERIFIED_BOOT="no" reads, to a human skimming, like the phone works.
# It does not. CI runs the host suite and it compiles a kernel; neither event has
# ever touched a Redmi 8A. So the workflows are asserted here:
#   - they exist at all (a deleted workflow silently turns CI back into
#     "someone ran it by hand once")
#   - the host workflow runs ./tests/run-all.sh itself, not a filtered or
#     hand-picked subset of it
#   - the vendor-kernel workflow runs the same script the build host runs
#   - no workflow claims a hardware boot
# ---------------------------------------------------------------------------
ci_check() {
  local label="$1" file="$2"
  local wf=".github/workflows/${file}"

  if [ ! -f "$wf" ]; then
    err "${label}: ${wf} is missing (CI is supposed to exist)"
    return 1
  fi
  # Comments are excluded on purpose: these workflows *document* the non-claim,
  # and saying "DEVICE_VERIFIED_BOOT stays no" in a comment is the honest thing
  # to do. What must not exist is code that sets it.
  if sed 's/[[:space:]]*#.*$//' "$wf" | grep -Eq 'DEVICE_VERIFIED_BOOT=|VERIFIED_BOOT=yes|BOOT_TEST_EXPECTS_DISPLAY=yes'; then
    err "${label}: a workflow must never assert a hardware boot"
    return 1
  fi
  pass "${label}: exists and claims no hardware boot"
}

if ! ci_check "host tests" "ci.yml"; then
  FAILED=$((FAILED + 1))
fi

# The host workflow has to call the suite script itself. A workflow that
# inlined a subset of the tests would go green while the real suite went red,
# which is the exact opposite of what CI is for.
if [ -f .github/workflows/ci.yml ]; then
  if grep -Eq '(^|[[:space:]])\./tests/run-all\.sh([[:space:]]|$)' .github/workflows/ci.yml; then
    pass "host tests: CI runs ./tests/run-all.sh itself"
  else
    err "host tests: ci.yml does not run ./tests/run-all.sh"
    FAILED=$((FAILED + 1))
  fi
fi

if ! ci_check "olive vendor kernel" "vendor-kernel-olive.yml"; then
  FAILED=$((FAILED + 1))
fi

if [ -f .github/workflows/vendor-kernel-olive.yml ]; then
  if grep -q 'build-vendor-kernel\.sh' .github/workflows/vendor-kernel-olive.yml; then
    pass "olive vendor kernel: CI runs the same script the build host runs"
  else
    err "olive vendor kernel: the workflow does not run scripts/build-vendor-kernel.sh"
    FAILED=$((FAILED + 1))
  fi
fi

printf '\n'
if [ "$FAILED" -ne 0 ]; then
  die "${FAILED} device check(s) failed"
fi
ok "device profiles are consistent and the flasher refuses what it must"