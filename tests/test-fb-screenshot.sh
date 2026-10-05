#!/usr/bin/env bash
# -----------------------------------------------------------------------
# scripts/fb-screenshot.sh has to decode a raw framebuffer correctly.
#
# Why this test exists
# -------------------
# This tool is the only thing standing between "the Redmi 8A's display might be
# working" and a picture of it. Once it runs on a phone, a wrong decode does not
# look like a tool bug - it looks like a broken display driver, a dead DSI link
# or a panel that is simply dark. The three bugs below each produce a
# plausible-looking but wrong image:
#
#   1. Channel order. Linux fbdev 32bpp is BGRA in memory. Reading it as RGBA
#      swaps red and blue, so correct pixels come out magenta-and-green and get
#      blamed on the panel.
#   2. Stride. sysfs reports stride in BYTES and it is routinely larger than
#      width*bpp/8. Ignoring it shears every row after the first - which is the
#      classic "the driver is broken" screenshot.
#   3. Truncation. A framebuffer shorter than stride*height is not a small
#      screenshot, it is a blank panel. Reading it anyway produces a valid PNG
#      of mostly noise.
#
# Each is asserted in both directions where that is possible: the correct decode
# is compared against hand-computed pixels, and the wrong stride is shown to
# produce a materially different (black) image.
# -----------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
# shellcheck source=../scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"

WORK="${ASTRIX_BUILD_DIR:-build}/fbtest"
mkdir -p "$WORK"
FAILED=0

# --- helpers ---------------------------------------------------------------

# Decode a P6 PPM into rows of (r,g,b) tuples. Deliberately not using the PNG
# path here: PPM is the uncompressed ground truth, so a bug in the zlib/PNG
# wrapper cannot hide a bug in the decode.
read_ppm() {
  python3 - "$1" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
parts = d.split(b"\n", 3)
w, h = (int(x) for x in parts[1].split())
px = parts[3]
rows = [[tuple(px[(y*w+x)*3:(y*w+x)*3+3]) for x in range(w)] for y in range(h)]
print(w, h)
for r in rows:
    print(" ".join("%d,%d,%d" % p for p in r))
PY
}

make_fixtures() {
  python3 - "$WORK" <<'PY'
import struct, sys, os
d = sys.argv[1]

# 32bpp: left half BGRA-red, right half BGRA-green, 8x4.
raw = bytearray()
for y in range(4):
    for x in range(8):
        raw += bytes((0, 0, 255, 255) if x < 4 else (0, 255, 0, 255))
open(os.path.join(d, "a32.raw"), "wb").write(raw)

# 16bpp RGB565: four red then four blue, 8x2.
raw = bytearray()
for y in range(2):
    for x in range(8):
        raw += struct.pack("<H", 0xF800 if x < 4 else 0x001F)
open(os.path.join(d, "a16.raw"), "wb").write(raw)

# Padded stride: 4x3 at 32bpp where each 16-byte row is followed by 48 bytes
# of padding. Three distinct shades so a sheared read is obvious.
raw = bytearray()
shades = [(0, 0, 255), (0, 0, 128), (0, 0, 64)]
for y in range(3):
    for x in range(4):
        raw += bytes(shades[y] + (255,))
    raw += b"\x00" * 48
open(os.path.join(d, "pad.raw"), "wb").write(raw)
PY
}

make_fixtures

printf '\n'
printf '  %s\n' "fb screenshot: 32bpp BGRA channel order"

# BGRA in memory: byte0=B, byte1=G, byte2=R. A reader that treats it as RGBA
# turns the red half blue and the green half magenta.
if ./scripts/fb-screenshot.sh --input "$WORK/a32.raw" --width 8 --height 4 \
      --bpp 32 --ppm "$WORK/a32.ppm" >/dev/null 2>&1; then
  out="$(read_ppm "$WORK/a32.ppm")"
  if printf '%s\n' "$out" | tail -n +2 | head -1 | grep -q '255,0,0 255,0,0 255,0,0 255,0,0 0,255,0 0,255,0 0,255,0 0,255,0'; then
    pass "32bpp: BGRA red decodes to red, green half stays green (no channel swap)"
  else
    err "32bpp: decoded row is wrong"
    printf '%s\n' "$out" | sed 's/^/      /'
    FAILED=$((FAILED + 1))
  fi
  if [ "$(printf '%s\n' "$out" | head -1)" = "8 4" ]; then
    pass "32bpp: dimensions preserved (8x4)"
  else
    err "32bpp: wrong dimensions: $(printf '%s\n' "$out" | head -1)"
    FAILED=$((FAILED + 1))
  fi
else
  err "32bpp: the tool failed on a valid 32bpp framebuffer"
  FAILED=$((FAILED + 1))
fi

printf '\n'
printf '  %s\n' "fb screenshot: 16bpp RGB565"

if ./scripts/fb-screenshot.sh --input "$WORK/a16.raw" --width 8 --height 2 \
      --bpp 16 --ppm "$WORK/a16.ppm" >/dev/null 2>&1; then
  out="$(read_ppm "$WORK/a16.ppm")"
  if printf '%s\n' "$out" | tail -n +2 | head -1 | grep -q '255,0,0 255,0,0 255,0,0 255,0,0 0,0,255 0,0,255 0,0,255 0,0,255'; then
    pass "16bpp: RGB565 0xF800 decodes to red and 0x001F to blue (5/6/5 bits)"
  else
    err "16bpp: decoded row is wrong"
    printf '%s\n' "$out" | sed 's/^/      /'
    FAILED=$((FAILED + 1))
  fi
else
  err "16bpp: the tool failed on a valid RGB565 framebuffer"
  FAILED=$((FAILED + 1))
fi

printf '\n'
printf '  %s\n' "fb screenshot: padded stride is honoured"

# Row bytes are 4*4 = 16, but the real stride is 64. Reading at stride 16 loses
# the 48 padding bytes per row and every row after the first becomes black -
# the exact symptom a driver investigation would chase for a week.
if ./scripts/fb-screenshot.sh --input "$WORK/pad.raw" --width 4 --height 3 \
      --bpp 32 --stride 64 --ppm "$WORK/pad-right.ppm" >/dev/null 2>&1; then
  out="$(read_ppm "$WORK/pad-right.ppm")"
  if printf '%s\n' "$out" | tail -n +2 | sed -n '2p' | grep -q '128,0,0 128,0,0 128,0,0 128,0,0' \
     && printf '%s\n' "$out" | tail -n +2 | sed -n '3p' | grep -q '64,0,0 64,0,0 64,0,0 64,0,0'; then
    pass "stride 64: rows 2 and 3 carry their own data, not padding"
  else
    err "stride 64: rows were sheared"
    printf '%s\n' "$out" | sed 's/^/      /'
    FAILED=$((FAILED + 1))
  fi
else
  err "stride 64: the tool failed"
  FAILED=$((FAILED + 1))
fi

# The same bytes at the packed stride. This must come out visibly different, or
# the check above proves nothing: if both strides produced the same image then
# stride would not be being read at all.
if ./scripts/fb-screenshot.sh --input "$WORK/pad.raw" --width 4 --height 3 \
      --bpp 32 --stride 16 --ppm "$WORK/pad-wrong.ppm" >/dev/null 2>&1; then
  if cmp -s "$WORK/pad-right.ppm" "$WORK/pad-wrong.ppm"; then
    err "stride is being ignored: packed and padded strides produced identical images"
    FAILED=$((FAILED + 1))
  else
    wrong_row2="$(read_ppm "$WORK/pad-wrong.ppm" | tail -n +2 | sed -n '2p')"
    if printf '%s\n' "$wrong_row2" | grep -q '0,0,0'; then
      pass "stride 16 (wrong) shears rows to black - the bug this avoids is real"
    else
      err "stride 16 produced an unexpected result: $wrong_row2"
      FAILED=$((FAILED + 1))
    fi
  fi
else
  err "stride 16: the tool failed on a readable buffer"
  FAILED=$((FAILED + 1))
fi

printf '\n'
printf '  %s\n' "fb screenshot: refuses impossible input"

# A framebuffer shorter than stride*height is a dark panel, not a small
# screenshot. Producing a PNG from it anyway would be the tool inventing a
# result the hardware never gave.
head -c 100 "$WORK/pad.raw" > "$WORK/short.raw"
if ./scripts/fb-screenshot.sh --input "$WORK/short.raw" --width 4 --height 3 \
      --bpp 32 --stride 64 --ppm "$WORK/short.ppm" >/dev/null 2>&1; then
  err "a truncated framebuffer was accepted; it should be refused"
  FAILED=$((FAILED + 1))
else
  pass "truncated framebuffer is refused, not turned into a picture of noise"
fi
[ -e "$WORK/short.ppm" ] && { err "a refused capture still wrote $WORK/short.ppm"; FAILED=$((FAILED + 1)); }

# An impossible geometry: stride smaller than one row.
if ./scripts/fb-screenshot.sh --input "$WORK/pad.raw" --width 4 --height 3 \
      --bpp 32 --stride 8 --ppm "$WORK/bogus.ppm" >/dev/null 2>&1; then
  err "a stride smaller than one row was accepted"
  FAILED=$((FAILED + 1))
else
  pass "impossible geometry (stride < one row) is refused"
fi

# An unsupported bpp must be named, not silently mangled.
if ./scripts/fb-screenshot.sh --input "$WORK/a32.raw" --width 8 --height 4 \
      --bpp 12 --ppm "$WORK/bpp12.ppm" 2>"$WORK/bpp12.err"; then
  err "12bpp was accepted; only 16/24/32 are implemented"
  FAILED=$((FAILED + 1))
elif grep -q 'unsupported bpp 12' "$WORK/bpp12.err"; then
  pass "unsupported bpp is refused and the message names the value"
else
  err "12bpp refused but without naming the value:"
  sed 's/^/      /' "$WORK/bpp12.err"
  FAILED=$((FAILED + 1))
fi

# A raw dump carries no geometry. Guessing it from the byte count would be
# guessing: 720x1520x4 and 760x1440x4 are the same number of bytes.
if ./scripts/fb-screenshot.sh --input "$WORK/a32.raw" --ppm "$WORK/nogeom.ppm" >/dev/null 2>&1; then
  err "a dump with no geometry was accepted; it must demand --width/--height/--bpp"
  FAILED=$((FAILED + 1))
else
  pass "a geometry-less dump is refused instead of guessed"
fi

printf '\n'
printf '  %s\n' "fb screenshot: a real panel-sized framebuffer round-trips exactly"

# The fixtures above are 8 pixels wide, which proves the decode arithmetic but
# says nothing about a real buffer. This one is the Redmi 8A's actual geometry
# with a deliberately padded stride (4096 bytes for a 2880-byte row, which is
# what a real tiled scanout looks like), built from the shell's own rendered home
# screen. Then the PNG is decoded back and compared byte for byte with the
# source. A decode that is off by one bit anywhere in a megapixel, or that loses
# the row padding once, shows up here as a mismatching row.
#
# Skip rather than fail if the shell has not been rendered: on a cold checkout
# this test would otherwise demand a build it has no business triggering.
REAL_PPM="${REPO_ROOT}/build/screens/redmi-8a-720x1520/01-home.ppm"
if [ ! -s "$REAL_PPM" ]; then
  info "no rendered panel at build/screens/redmi-8a-720x1520 - skipping the round-trip"
  info "render it with ./tests/run-all.sh (render-screens) first"
else
  python3 - "$REAL_PPM" "$WORK" <<'PY'
import os, sys
src, work = sys.argv[1], sys.argv[2]
parts = open(src, "rb").read().split(b"\n", 3)
assert parts[0] == b"P6", "source is not a P6 PPM"
w, h = (int(x) for x in parts[1].split())
px = parts[3]
stride = 4096          # 720*4 = 2880 used, 1216 padding bytes per row
raw = bytearray()
for y in range(h):
    # Trailing pad, not leading: fbdev row n starts at byte n*stride, so the
    # pixels come first and the alignment padding is at the end of the row.
    row = px[y * w * 3:(y + 1) * w * 3]
    for x in range(w):
        raw += bytes((row[x * 3 + 2], row[x * 3 + 1], row[x * 3], 255))  # BGRA
    raw += b"\x00" * (stride - w * 4)
open(os.path.join(work, "real-panel.raw"), "wb").write(raw)
print(f"built a {w}x{h} 32bpp BGRA framebuffer, stride {stride}")
PY
  if ./scripts/fb-screenshot.sh --input "$WORK/real-panel.raw" --width 720 \
        --height 1520 --bpp 32 --stride 4096 --ppm "$WORK/real-panel-out.ppm" >/dev/null 2>&1; then
    if python3 - "$REAL_PPM" "$WORK/real-panel-out.ppm" <<'PY'
import sys
ref_parts = open(sys.argv[1], "rb").read().split(b"\n", 3)
w, h = (int(x) for x in ref_parts[1].split())
ref = ref_parts[3]
got_parts = open(sys.argv[2], "rb").read().split(b"\n", 3)
gw, gh = (int(x) for x in got_parts[1].split())
got = got_parts[3]
sys.exit(0 if (w, h) == (gw, gh) and got == ref else 1)
PY
    then
      pass "720x1520 with 1216 pad bytes/row: all $((720 * 1520)) pixels round-trip byte-identical"
    else
      err "a megapixel framebuffer did not round-trip; the decode is wrong somewhere"
      FAILED=$((FAILED + 1))
    fi
  else
    err "the tool failed on a real panel-sized framebuffer"
    FAILED=$((FAILED + 1))
  fi
fi

printf '\n'
if [ "$FAILED" -ne 0 ]; then
  die "${FAILED} framebuffer decode check(s) failed"
fi
ok "the framebuffer screenshot tool decodes real framebuffers, and refuses the ones that cannot exist"