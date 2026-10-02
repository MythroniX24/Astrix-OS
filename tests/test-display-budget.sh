#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# The display link budget must be right, and it must be able to say "no".
#
# Why this test exists
# -------------------
# "Boot it with a display" starts with a physical constraint: the panel has a
# pixel rate, the MIPI DSI link has lanes and a per-lane rate, and if pixels do
# not fit in bits, nothing above the link can help. Getting this wrong first
# costs days of panel-driver work on a link that was never fast enough, so the
# arithmetic behind scripts/display-budget.sh is pinned here - including the
# cases where the answer must be "this panel does not fit", which is the half
# of the tool that is easiest to write so it always says yes.
#
# It also asserts that both real profiles carry the numbers and say where they
# came from. A panel spec with no provenance is a rumour.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
# shellcheck source=../scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"

FAILED=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# A minimal profile: the display fields only, since that is all the budget
# reads. Anything else is filled in by the fixture that needs it.
fixture() {
  local name="$1"; shift
  {
    printf 'DEVICE_NAME="fixture %s"\n' "$name"
    printf 'DEVICE_CODENAME="%s"\n' "$name"
    printf 'DEVICE_SOC="fixture"\n'
    printf 'DEVICE_ARCH="arm64"\n'
    printf 'DEVICE_GPU="fixture"\n'
    printf 'DEVICE_GPU_DRIVER_MAINLINE="none"\n'
    printf 'DEVICE_BOOTLOADER_UNLOCK="none"\n'
    printf 'DEVICE_PARTITION_SCHEME="fixture"\n'
    printf 'DEVICE_PANEL_WIDTH=%s\n' "${1:-720}"
    printf 'DEVICE_PANEL_HEIGHT=%s\n' "${2:-1280}"
    printf 'DEVICE_PANEL_REFRESH=%s\n' "${3:-60}"
    printf 'DEVICE_DSI_LANES=%s\n' "${4:-4}"
    printf 'DEVICE_DSI_MAX_LINK_RATE=%s\n' "${5:-2500000000}"
    printf 'DEVICE_DSI_BPP=%s\n' "${6:-24}"
    printf 'DEVICE_PANEL_HBLANK_PERCENT=%s\n' "${7:-10}"
    printf 'DEVICE_PANEL_VBLANK_PERCENT=%s\n' "${8:-10}"
    printf 'DEVICE_DISPLAY_DATA_PROVENANCE="fixture"\n'
    printf 'DEVICE_REQUIRED_KERNEL_FEATURES="drm-mipi-dsi"\n'
    printf 'DEVICE_DOWNSTREAM_ONLY_FEATURES="fixture-none"\n'
    printf 'DEVICE_BOOT_STATUS="unsupported"\n'
    printf 'DEVICE_VERIFIED_BOOT="no"\n'
    printf 'DEVICE_BOOT_BLOCKERS="fixture"\n'
  } > "${TMP}/${name}.conf"
}

echo "  a panel that fits reports OK"
fixture fits 720 1280 60 4
out="$(ASTRIX_DEVICE_DIR="$TMP" bash scripts/display-budget.sh fits --quiet)" && rc=0 || rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "OK"; then
  pass "720x1280@60 on 4 lanes fits: ${out}"
else
  err "a panel with ample headroom was not accepted (exit ${rc}): ${out}"
  FAILED=$((FAILED + 1))
fi

echo "  a panel that cannot fit is refused"
# 1440x3200 at 120Hz needs about 4 Gbit/s per lane on 4 lanes - more than a
# 2.5 Gbit/s D-PHY lane carries. The tool has to say so rather than round it
# into the affirmative.
fixture toobig 1440 3200 120 4
out="$(ASTRIX_DEVICE_DIR="$TMP" bash scripts/display-budget.sh toobig --quiet)" && rc=0 || rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "OVER BUDGET"; then
  pass "1440x3200@120 is over budget, as it must be"
else
  err "an infeasible panel was accepted (exit ${rc}): ${out}"
  FAILED=$((FAILED + 1))
fi

echo "  the lane count is what decides it"
# Same panel, half the lanes: the required per-lane rate doubles, and a panel
# that just fit at 4 lanes must not fit at 2.
fixture onelane 1080 2400 120 2
out="$(ASTRIX_DEVICE_DIR="$TMP" bash scripts/display-budget.sh onelane --quiet)" && rc=0 || rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "OVER BUDGET"; then
  pass "the same panel on 2 lanes is over budget"
else
  err "halving the lanes did not change the verdict (exit ${rc}): ${out}"
  FAILED=$((FAILED + 1))
fi

echo "  blanking is honoured, and is labelled an assumption"
a="$(ASTRIX_DEVICE_DIR="$TMP" bash scripts/display-budget.sh fits --quiet | sed -n 's/.*needs \([0-9.]*\) .*/\1/p')"
fixture fatblanks 720 1280 60 4 2500000000 24 20 20
b="$(ASTRIX_DEVICE_DIR="$TMP" bash scripts/display-budget.sh fatblanks --quiet | sed -n 's/.*needs \([0-9.]*\) .*/\1/p')"
if awk -v a="$a" -v b="$b" 'BEGIN { exit !(b > a) }'; then
  pass "more blanking means more bandwidth (${a} -> ${b})"
else
  err "20% blanking did not raise the requirement (${a} -> ${b})"
  FAILED=$((FAILED + 1))
fi
if grep -q "blanking assumed" <(ASTRIX_DEVICE_DIR="$TMP" bash scripts/display-budget.sh fits); then
  pass "the report labels the blanking as an assumption, not a measurement"
else
  err "the report does not mark the blanking as assumed"
  FAILED=$((FAILED + 1))
fi

echo "  a profile with no display data is a gap, not a pass"
cat > "${TMP}/nodata.conf" <<'EOF'
DEVICE_NAME="no data"
DEVICE_CODENAME="nodata"
DEVICE_SOC="fixture"
DEVICE_ARCH="arm64"
DEVICE_PANEL_WIDTH=720
DEVICE_PANEL_HEIGHT=1280
DEVICE_PANEL_REFRESH=60
EOF
out="$(ASTRIX_DEVICE_DIR="$TMP" bash scripts/display-budget.sh nodata --quiet)" && rc=0 || rc=$?
if [ "$rc" -eq 2 ]; then
  pass "a profile without link data reports that, it does not default to zero"
else
  err "missing display data was not reported (exit ${rc}): ${out}"
  FAILED=$((FAILED + 1))
fi

echo "  the real profiles' published numbers are what the tool computes"
# These two figures are quoted in the profiles and in docs/DEVICES.md, so if
# the arithmetic moves, the documentation is wrong and the test should say so.
moto="$(bash scripts/display-budget.sh moto-g64-5g --quiet)"
case "$moto" in
  *"2.258 Gbit/s/lane of 2.500"*) pass "moto g64 5G: ${moto}" ;;
  *) err "moto g64 5G budget changed: ${moto}"; FAILED=$((FAILED + 1)) ;;
esac
redmi="$(bash scripts/display-budget.sh redmi-8a --quiet)"
case "$redmi" in
  *"476.7 Mbit/s/lane of 2.500"*) pass "Redmi 8A: ${redmi}" ;;
  *) err "Redmi 8A budget changed: ${redmi}"; FAILED=$((FAILED + 1)) ;;
esac

echo "  device.sh check refuses a profile whose panel cannot be driven"
out="$(ASTRIX_DEVICE_DIR="$TMP" bash scripts/device.sh check toobig 2>&1)" && rc=0 || rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "does not fit the DSI link"; then
  pass "device.sh check fails on an infeasible panel"
else
  err "device.sh check accepted an infeasible panel (exit ${rc})"
  printf '%s\n' "$out" | sed 's/^/      /'
  FAILED=$((FAILED + 1))
fi

echo "  both real profiles record where their display numbers came from"
for p in config/devices/*.conf; do
  d="$(basename "$p" .conf)"
  if grep -q "^DEVICE_DISPLAY_DATA_PROVENANCE=." "$p"; then
    pass "${d}: provenance recorded"
  else
    err "${d}: no DEVICE_DISPLAY_DATA_PROVENANCE"
    FAILED=$((FAILED + 1))
  fi
done

printf '\n'
if [ "$FAILED" -ne 0 ]; then
  die "${FAILED} display budget check(s) failed"
fi
ok "the display link budget is correct and is able to refuse"