#!/usr/bin/env bash
# -----------------------------------------------------------------------
# Screenshot a Linux framebuffer device (/dev/fb0) as a PNG.
#
# Why this exists
# ---------------
# The Astrix compositor draws to whatever the kernel gives it. On QEMU that is
# virtio-gpu and scripts/screenshot.sh (QMP screendump) can photograph it. On a
# real phone it is not: the Redmi 8A vendor kernel drives the panel through the
# Android framebuffer stack (FB_MSM_MDSS), not DRM/KMS, so there is no KMS
# device for wlroots to scan out of and no DRM debugfs to read.
#
# That leaves exactly one place the pixels actually are while the panel is
# being lit: the framebuffer itself. Reading /dev/fb0 and writing a PNG is what
# turns "is the display working?" from a squint into a file a human can look
# at - and, more usefully, a file a test can assert pixels against.
#
# It also settles the panel-resolution question empirically. The Redmi 8A's
# device tree advertises two hx8399c panels (1080x2160 and 720x1440) while the
# hardware is 720x1520, and no amount of reading source code picks between
# them. Reading back the framebuffer's own virtual_size does.
#
# What it does NOT prove: that the DSI link is carrying those pixels to the
# panel. A framebuffer can be full of correct pixels with the panel dead, and
# with the wrong resolution it will be full of correct pixels cut off at the
# edges. A screenshot is necessary, not sufficient.
#
# Usage:
#   scripts/fb-screenshot.sh [options] [out.png]
#
#     --fb DEV      framebuffer device        (default /dev/fb0)
#     --sysfs DIR   sysfs dir for that device (default derived from --fb)
#     --input FILE  read a raw dump instead of the device; geometry then has
#                   to be given with --width/--height/--bpp/--stride, because
#                   a raw file carries no header. This is the mode that works
#                   over adb, where /dev/fb0 is not on the host filesystem.
#     --width N --height N --bpp N --stride N    override sysfs
#     --ppm         write a P6 PPM instead of a PNG (no zlib needed)
#     --crop-only   do not convert; only report the geometry sysfs claims
#
# Reading a real device needs root. Reading a dump needs nothing.
# -----------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib.sh
. "$REPO_ROOT/scripts/lib.sh"

FB=/dev/fb0
SYSFS=""
INPUT=""
OUT=""
W=""
H=""
BPP=""
STRIDE=""
AS_PPM=0
CROP_ONLY=0

while [ $# -gt 0 ]; do
  case "$1" in
    --fb)      FB="$2"; shift 2 ;;
    --sysfs)   SYSFS="$2"; shift 2 ;;
    --input)   INPUT="$2"; shift 2 ;;
    --width)   W="$2"; shift 2 ;;
    --height)  H="$2"; shift 2 ;;
    --bpp)     BPP="$2"; shift 2 ;;
    --stride)  STRIDE="$2"; shift 2 ;;
    --ppm)     AS_PPM=1; shift ;;
    --crop-only) CROP_ONLY=1; shift ;;
    -h|--help) sed -n '2,42p' "$0"; exit 0 ;;
    --)        shift; break ;;
    -*)        die "unknown option: $1" ;;
    *)         OUT="$1"; shift ;;
  esac
done

[ -n "$OUT" ] || OUT="${ASTRIX_BUILD_DIR:-$REPO_ROOT/build}/fb-screenshot.png"

# /dev/fb0 -> /sys/class/graphics/fb0. The class symlink is what carries the
# geometry attributes; the device node itself has none.
if [ -z "$SYSFS" ]; then
  SYSFS="/sys/class/graphics/$(basename "$FB")"
fi

# --- geometry --------------------------------------------------------------
# virtual_size is "<w>x<h>" and stride is in BYTES, not pixels. Getting that
# second one wrong is the classic fbdev bug: every row looks fine and every row
# after the first is sheared, which reads as "the display driver is broken".
fb_attr() {
  local name="$1" default="$2"
  if [ -r "${SYSFS}/${name}" ]; then
    cat "${SYSFS}/${name}"
  else
    printf '%s' "$default"
  fi
}

if [ -n "$INPUT" ]; then
  [ -r "$INPUT" ] || die "cannot read ${INPUT}"
  # A raw dump has no header, so geometry cannot be guessed from its size: many
  # geometries share a byte count (720x1520x4 == 760x1440x4). Refuse rather
  # than guess.
  for v in W H BPP; do
    eval "val=\$$v"
    [ -n "$val" ] || die "--input needs an explicit --width/--height/--bpp (and --stride)"
  done
  SRC="$INPUT"
else
  [ -r "${SYSFS}/virtual_size" ] || die "no framebuffer geometry at ${SYSFS}/virtual_size
    is ${FB} a framebuffer? On this device check /sys/class/graphics/ and
    dmesg for the FB_MSM_MDSS probe. Nothing has been written."
  vs="$(cat "${SYSFS}/virtual_size")"
  W="${W:-${vs%x*}}"
  H="${H:-${vs#*x}}"
  BPP="${BPP:-$(fb_attr bits_per_pixel 0)}"
  STRIDE="${STRIDE:-$(fb_attr stride 0)}"
  [ -r "$FB" ] || die "cannot read ${FB} (need root, or use --input with a dump)"
  SRC="$FB"
fi

# stride defaults to the minimum for the geometry, which is what an unpacked
# framebuffer looks like. An explicit 0 from sysfs means "fbdev did not say".
if [ -z "$STRIDE" ] || [ "$STRIDE" -eq 0 ] 2>/dev/null; then
  STRIDE=$(( W * BPP / 8 ))
fi

MIN_STRIDE=$(( W * BPP / 8 ))
[ "$STRIDE" -ge "$MIN_STRIDE" ] || die "stride ${STRIDE} is smaller than one row (${MIN_STRIDE} bytes)
    that geometry is impossible; refusing to read a framebuffer that cannot exist"

log "geometry ${W}x${H} at ${BPP}bpp, stride ${STRIDE} bytes (needs $(( STRIDE * H )) bytes)"
info "source: ${SRC}"

if [ "$CROP_ONLY" -eq 1 ]; then
  ok "reported geometry only (--crop-only); no pixels read"
  exit 0
fi

SIZE=$(wc -c < "$SRC")
EXPECTED=$(( STRIDE * H ))
[ "$SIZE" -ge "$EXPECTED" ] || die "short read: ${SRC} has ${SIZE} bytes, ${EXPECTED} needed for ${W}x${H} stride ${STRIDE}
    a truncated framebuffer is a blank panel, not a small screenshot"

# --- convert ---------------------------------------------------------------
# Pure python: zlib/struct are stdlib, so this needs no imagemagick or netpbm,
# and it runs identically on the build host and on the device.
mkdir -p "$(dirname "$OUT")"

python3 - "$SRC" "$OUT" "$W" "$H" "$BPP" "$STRIDE" "$AS_PPM" <<'PY'
import struct, sys, zlib

src, dst = sys.argv[1], sys.argv[2]
w, h, bpp, stride = (int(x) for x in sys.argv[3:7])
as_ppm = sys.argv[7] == "1"

with open(src, "rb") as f:
    raw = f.read(stride * h)

if bpp == 32:
    # Linux fbdev 32bpp is BGRA in memory (DRM's DRM_FORMAT_ARGB8888 little
    # endian), which is why a naive "copy 4 bytes as RGBA" reader comes out
    # with red and blue swapped - green wallpaper looking magenta.
    b, g, r = 0, 1, 2
elif bpp == 16:
    # RGB565, packed little-endian.
    b, g, r = None, None, None
elif bpp == 24:
    b, g, r = 0, 1, 2
else:
    sys.exit(f"unsupported bpp {bpp}: this tool handles 16 (RGB565), 24 and 32 (BGRA)")

row_out = bytearray(w * 3)
rgb = bytearray(w * h * 3)

for y in range(h):
    base = y * stride
    o = y * w * 3
    if bpp == 16:
        for x in range(w):
            p = base + x * 2
            v = raw[p] | (raw[p + 1] << 8)
            r8 = (v >> 11) & 0x1F
            g8 = (v >> 5) & 0x3F
            b8 = v & 0x1F
            rgb[o] = (r8 << 3) | (r8 >> 2)
            rgb[o + 1] = (g8 << 2) | (g8 >> 4)
            rgb[o + 2] = (b8 << 3) | (b8 >> 2)
            o += 3
    else:
        step = bpp // 8
        for x in range(w):
            p = base + x * step
            rgb[o] = raw[p + r]
            rgb[o + 1] = raw[p + g]
            rgb[o + 2] = raw[p + b]
            o += 3

if as_ppm:
    with open(dst, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(rgb)
    print(f"wrote {dst} ({w}x{h} P6)")
else:
    lines = bytearray()
    for y in range(h):
        lines.append(0)  # PNG filter type 0 (None) for each scanline
        lines += rgb[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(lines), 6))
           + chunk(b"IEND", b""))
    with open(dst, "wb") as f:
        f.write(png)
    print(f"wrote {dst} ({w}x{h})")
PY

ok "framebuffer captured to ${OUT}"