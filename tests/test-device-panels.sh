#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Every device profile's panel, checked with no phone attached.
#
# Why this exists
# ---------------
# Neither target phone has ever been booted - Adreno 505 has no mainline
# driver, MT6855's display pipeline has no mainline driver - so "does the UI
# work on the moto g64 5G?" has no honest answer that involves the phone. What
# does have an answer is the part that is pure arithmetic about the panel, and
# that is exactly the part most likely to be wrong: 1080x2400 is twice the
# width and 1.5x the height of what the QEMU dev environment provides, and a
# shell that lays out perfectly at 1024x768 can put its dock's bottom edge off
# a 2400-pixel screen.
#
# The geometry is read from config/devices/*.conf rather than hardcoded here.
# A test that duplicates the numbers it is checking against is a test that
# silently keeps passing when the hardware spec changes.
#
# Host test: no phone, no bootloader, no flash, no QEMU, no compositor.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
# shellcheck source=../scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"

OUT="${ASTRIX_BUILD_DIR:-build}/tests"
mkdir -p "$OUT"

if ! command -v cc >/dev/null && ! command -v gcc >/dev/null; then
  printf '\n'
  warn "no C compiler; cannot check device panel geometry"
  exit 0
fi
CC="${CC:-$(command -v cc || command -v gcc)}"

# shellcheck source=../scripts/lib.sh
printf '\n'
banner_msg="every device panel lays out and accepts taps, with no phone present"
printf '  %s\n' "$banner_msg"

# input.c is #included by the test for the real hit-test; shell.c, keyboard.c,
# gesture.c and the canvas are linked because input.c calls into all of them.
CFLAGS="-std=c11 -O1 -Igui/shell/include -Igui/ui/include"
SOURCES="gui/ui/src/astrix_canvas.c gui/ui/src/astrix_font.c gui/shell/src/render.c \
         gui/shell/src/gesture.c gui/shell/src/shell.c gui/shell/src/keyboard.c"
# shellcheck disable=SC2086
if ! "$CC" $CFLAGS $SOURCES tests/device-panels.c -o "$OUT/device-panels" -lm \
      > "$OUT/device-panels.log" 2>&1; then
  err "device-panels failed to compile"
  sed 's/^/      /' "$OUT/device-panels.log" | head -20
  exit 1
fi

FAILED=0
CHECKED=0
for p in config/devices/*.conf; do
  [ -e "$p" ] || continue
  dev="$(basename "$p" .conf)"
  # Read the panel geometry out of the profile in a subshell, so no variable
  # leaks from one device into the next.
  geom="$( ( source "$p"; printf '%s %s %s' \
        "${DEVICE_PANEL_WIDTH}" "${DEVICE_PANEL_HEIGHT}" "${DEVICE_PANEL_REFRESH}" ) )"
  w="$(echo "$geom" | cut -d' ' -f1)"
  h="$(echo "$geom" | cut -d' ' -f2)"
  hz="$(echo "$geom" | cut -d' ' -f3)"

  printf '\n'
  printf '  %s (%sx%s @ %sHz)\n' "$dev" "$w" "$h" "${hz:-?}"
  CHECKED=$((CHECKED + 1))
  if "$OUT/device-panels" "$w" "$h" "$dev" > "$OUT/device-panels-${dev}.out" 2>&1; then
    sed 's/^/      /' "$OUT/device-panels-${dev}.out"
  else
    sed 's/^/      /' "$OUT/device-panels-${dev}.out"
    FAILED=$((FAILED + 1))
  fi
done

# Render the screens at each device's real geometry too, so the layout can be
# looked at rather than only asserted about. This is the closest thing to
# "seeing it on the phone" that is possible without a phone.
printf '\n'
if command -v cc >/dev/null || command -v gcc >/dev/null; then
  if "$CC" -std=c11 -O1 $CFLAGS $SOURCES tests/render-screens.c \
        -o "$OUT/render-screens" -lm >> "$OUT/device-panels.log" 2>&1; then
    for p in config/devices/*.conf; do
      [ -e "$p" ] || continue
      dev="$(basename "$p" .conf)"
      geom="$( ( source "$p"; printf '%s %s' "${DEVICE_PANEL_WIDTH}" "${DEVICE_PANEL_HEIGHT}" ) )"
      pw="$(echo "$geom" | cut -d' ' -f1)"
      ph="$(echo "$geom" | cut -d' ' -f2)"
      dir="build/screens/${dev}-${pw}x${ph}"
      mkdir -p "$dir"
      if "$OUT/render-screens" "$dir" "$pw" "$ph" >/dev/null 2>&1; then
        pass "rendered ${dev} at ${pw}x${ph} -> ${dir}"
      else
        warn "could not render ${dev} at ${pw}x${ph}"
      fi
    done
  else
    warn "render-screens would not compile; skipping the per-device renders"
  fi
fi

printf '\n'
if [ "$CHECKED" -eq 0 ]; then
  die "no device profiles found; nothing was checked"
fi
if [ "$FAILED" -ne 0 ]; then
  die "${FAILED} device panel(s) fail to lay out"
fi
ok "all ${CHECKED} device panel(s) lay out and accept taps, with no phone present"