#!/usr/bin/env bash
# Astrix OS - display link budget for a device profile.
#
#   ./scripts/display-budget.sh <device>          full report
#   ./scripts/display-budget.sh --all            every profile
#   ./scripts/display-budget.sh <device> --quiet  one-line verdict (exit code)
#
# WHY THIS EXISTS
# ---------------
# "Make it boot with a display" is not one problem, it is a chain, and the
# chain starts with arithmetic. A panel of W x H at R Hz has a fixed pixel
# rate; a MIPI DSI link has a fixed number of lanes at a fixed per-lane rate;
# and if pixels*bpp does not fit in lanes*rate, the panel cannot be driven at
# all, no matter how good the driver is. Getting that number wrong first wastes
# days of driver work on a link that was never fast enough.
#
# So this script computes the budget from the profile's panel numbers, which
# come from vendor specification sheets, and prints what the link has to carry.
# It is the first thing to run before writing a single line of panel driver.
#
# HONESTY
# -------
# The blanking percentages are ASSUMPTIONS, not measurements, and are labelled
# as such in every profile (DEVICE_DISPLAY_DATA_PROVENANCE). Pixel rate scales
# linearly with them, so the report says so rather than implying a precision it
# does not have. What is NOT an assumption is the panel's resolution and
# refresh rate, which come from the vendor's own specification.
#
# This script does not claim a phone has ever lit up its panel. It computes
# numbers.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

# Overridable so the test suite can point it at fixture profiles; the real
# profiles are the default and the only ones used outside tests.
DEVICE_DIR="${ASTRIX_DEVICE_DIR:-${ASTRIX_REPO_ROOT}/config/devices}"

usage() {
  sed -n '2,30p' "$0"
  cat <<'EOF'

usage:
  display-budget.sh <device>           print the link budget for one device
  display-budget.sh --all              print it for every known profile
  display-budget.sh <device> --quiet   one line, exit 0 ok / 1 over budget
EOF
}

# --------------------------------------------------------------------------
# budget <profile-path> [quiet]
# Prints the report and returns 0 when the link can carry the panel,
# 1 when it cannot, 2 when the profile lacks the data.
# --------------------------------------------------------------------------
budget() {
  local path="$1" quiet="${2:-no}"
  local name
  name="$(basename "$path" .conf)"
  # shellcheck source=/dev/null
  source "$path"

  local missing=""
  local k
  for k in DEVICE_PANEL_WIDTH DEVICE_PANEL_HEIGHT DEVICE_PANEL_REFRESH \
           DEVICE_DSI_LANES DEVICE_DSI_MAX_LINK_RATE \
           DEVICE_PANEL_HBLANK_PERCENT DEVICE_PANEL_VBLANK_PERCENT; do
    [ -n "${!k:-}" ] || missing="${missing} ${k}"
  done
  if [ -n "$missing" ]; then
    if [ "$quiet" = "quiet" ]; then
      printf '%s: no display budget data (%s)\n' "$name" "$missing"
    else
      err "${name}: missing display budget data:${missing}"
      dim "  add DEVICE_DSI_LANES, DEVICE_DSI_MAX_LINK_RATE and the blanking"
      dim "  assumptions to ${path}. Until then no budget can be computed."
    fi
    return 2 || true
  fi

  local bpp="${DEVICE_DSI_BPP:-24}"
  case "$bpp" in
    16|18|24|30|36) : ;;
    *)
      err "${name}: DEVICE_DSI_BPP=${bpp} is not a valid bits-per-pixel"
      return 2 || true
      ;;
  esac

  # All arithmetic in one awk pass so the shell cannot mangle integers and so
  # the numbers printed are exactly the numbers compared.
  local out rc=0
  out="$(awk -v w="${DEVICE_PANEL_WIDTH}" \
             -v h="${DEVICE_PANEL_HEIGHT}" \
             -v r="${DEVICE_PANEL_REFRESH}" \
             -v hb="${DEVICE_PANEL_HBLANK_PERCENT}" \
             -v vb="${DEVICE_PANEL_VBLANK_PERCENT}" \
             -v lanes="${DEVICE_DSI_LANES}" \
             -v maxr="${DEVICE_DSI_MAX_LINK_RATE}" \
             -v bpp="${bpp}" -v quiet="$quiet" -v name="$name" '
    function fmt(x) {
      if (x >= 1e9) return sprintf("%.3f Gbit/s", x / 1e9)
      if (x >= 1e6) return sprintf("%.1f Mbit/s", x / 1e6)
      return sprintf("%.0f bit/s", x)
    }
    function mhz(x) {
      if (x >= 1e6) return sprintf("%.1f MHz", x / 1e6)
      if (x >= 1e3) return sprintf("%.1f kHz", x / 1e3)
      return sprintf("%.0f Hz", x)
    }
    BEGIN {
      htotal = w + int(w * hb / 100 + 0.5)
      vtotal = h + int(h * vb / 100 + 0.5)
      active = w * h
      total  = htotal * vtotal
      pixclk = total * r
      agg    = pixclk * bpp
      perlane= agg / lanes
      head   = (maxr - perlane) / maxr * 100.0
      ok     = (perlane <= maxr)

      if (quiet != "quiet") {
        printf "\n  %s (%s) - %sx%s @ %sHz\n", name, \
               (w > h ? "landscape" : "portrait"), w, h, r
        printf "  %-26s %s\n", "active pixels", sprintf("%d", active)
        printf "  %-26s %d x %d\n", "total pixels/frame", htotal, vtotal
        printf "  %-26s %s\n", "blanking assumed", sprintf("h%d%% v%d%%", hb, vb)
        printf "  %-26s %s\n", "pixel clock", mhz(pixclk)
        printf "  %-26s %d bpp", "DSI payload", bpp
        printf " over %d lane(s)\n", lanes
        printf "  %-26s %s\n", "aggregate link rate", fmt(agg)
        printf "  %-26s %s\n", "required per lane", fmt(perlane)
        printf "  %-26s %s\n", "per-lane capability", fmt(maxr)
        if (ok) {
          printf "  %-26s %.0f%%\n", "headroom", head
        } else {
          printf "  %-26s %.0f%% (OVER BUDGET)\n", "headroom", head
        }
      }

      # The verdict is emitted as a machine-readable line and rendered by the
      # shell below, so the human-facing wording lives in one place and the
      # numbers still come only from this arithmetic.
      if (ok) {
        return_code = 0
        v = (head < 20.0) ? "tight" : "ok"
        if (quiet == "quiet") {
          printf "%s: %s  needs %s/lane of %s (%s headroom)\n", name, \
                 (v == "tight" ? "OK but tight" : "OK"), \
                 fmt(perlane), fmt(maxr), sprintf("%.0f%%", head)
        } else {
          printf "__verdict__ %s\n", v
        }
      } else {
        return_code = 1
        if (quiet == "quiet") {
          printf "%s: OVER BUDGET  needs %s/lane, link can do %s\n", \
                 name, fmt(perlane), fmt(maxr)
        } else {
          printf "__verdict__ over\n"
        }
      }
      exit return_code
    }
  ')" || rc=$?

  printf '%s\n' "$out" | grep -v '^__verdict__ ' || true

  if [ "$quiet" != "quiet" ]; then
    verdict="$(printf '%s\n' "$out" | sed -n 's/^__verdict__ //p')"
    dim "  provenance: ${DEVICE_DISPLAY_DATA_PROVENANCE:-not recorded}"
    printf '\n'
    case "$verdict" in
      ok)
        pass "the DSI link can carry this panel at ${bpp} bpp on ${DEVICE_DSI_LANES} lanes"
        ;;
      tight)
        pass "the DSI link can carry this panel at ${bpp} bpp on ${DEVICE_DSI_LANES} lanes"
        warn "but with very little headroom"
        dim "  Above roughly 80% of a link's capability, link training becomes"
        dim "  fragile: ribbon cable length, temperature and part-to-part D-PHY"
        dim "  variation all eat into the margin. A 120Hz panel here is a real"
        dim "  risk on real hardware; 60Hz halves the requirement and is the"
        dim "  mode to bring up first."
        ;;
      over)
        err "this panel does not fit on this link at ${bpp} bpp"
        dim "  Fix this before writing a driver: fewer bits per pixel, more lanes,"
        dim "  or a lower refresh rate. No driver can outrun the wire."
        ;;
      *)
        err "no verdict produced (profile data problem?)"
        ;;
    esac
  fi
  # A non-zero exit here is a finding, not a crash: "this panel does not fit"
  # is exactly what the caller asked for, so the ERR trap is suppressed.
  return "${rc}" || true
}

# --------------------------------------------------------------------------
CMD="${1:-}"
QUIET="no"
case "$CMD" in
  --all) ALL="yes" ;;
  "") usage; exit 1 ;;
  -*) usage; exit 1 ;;
  *) ALL="no" ;;
esac
if [ "${2:-}" = "--quiet" ]; then QUIET="quiet"; fi

if [ "${ALL:-no}" = "yes" ]; then
  rc=0
  for f in "${DEVICE_DIR}"/*.conf; do
    [ -e "$f" ] || continue
    budget "$f" "$QUIET" || rc=$?
  done
  exit "$rc"
fi

PROFILE="${DEVICE_DIR}/${CMD}.conf"
if [ ! -f "$PROFILE" ]; then
  err "no such device profile: ${CMD}"
  dim "known: $(ls "${DEVICE_DIR}" | sed 's/\.conf$//' | tr '\n' ' ')"
  exit 1
fi

rc=0
budget "$PROFILE" "$QUIET" || rc=$?
exit "$rc"