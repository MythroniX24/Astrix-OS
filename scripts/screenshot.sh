#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Screenshot a running Astrix VM.
#
# Why this exists: the serial log can prove the compositor came up and that the
# shell mapped a surface, but neither of those says what is actually on the
# screen. QEMU's `screendump` writes a guest surface straight to a PPM over
# QMP, with no guest-side agent and no extra packages.
#
# KNOWN LIMITATION - read before trusting a black picture. On virtio-gpu,
# screendump captures the device's *console scanout* (the 1024x768 text console
# showing the boot log), not the DRM primary plane that the Astrix compositor
# scans its Wayland output out to. So this tool proves the VM is alive and
# records the console; it does NOT photograph the Astrix UI. The UI itself is
# rendered and inspected through tests/render-screens.c, which writes every
# screen to build/screens/*.ppm at the real 720x1600 and 1080x2400 sizes.
#
# Usage:  scripts/screenshot.sh [output.png]
# Needs:  a VM started by qemu/run.sh (it opens the QMP socket), python3.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib.sh
. "$REPO_ROOT/scripts/lib.sh"

OUT="${1:-$REPO_ROOT/build/astrix-screenshot.png}"
QMP="${ASTRIX_QMP_SOCK:-$REPO_ROOT/build/qmp.sock}"
PPM="$REPO_ROOT/build/astrix-screenshot.ppm"

[ -S "$QMP" ] || die "no QMP socket at $QMP - start the VM with ./run-qemu.sh first"
command -v python3 >/dev/null 2>&1 || die "python3 is required to talk to QMP"

mkdir -p "$(dirname "$PPM")"
rm -f "$PPM"

log "Requesting a framebuffer screendump via $QMP"
python3 - "$QMP" "$PPM" <<'PY'
import json, socket, sys

sock_path, ppm_path = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(30)
s.connect(sock_path)
f = s.makefile("rw", encoding="utf-8", newline="\n")

# QMP negotiates capabilities before it accepts any command.
f.readline()
f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
f.flush()
f.readline()

# QEMU's screendump only understands the filename argument; the format is
# implied by the extension, and passing "format" is a GenericError.
f.write(json.dumps({"execute": "screendump",
                    "arguments": {"filename": ppm_path}}) + "\n")
f.flush()
reply = json.loads(f.readline())
if "error" in reply:
    sys.exit("QMP screendump failed: " + json.dumps(reply["error"]))
print("QMP screendump: " + json.dumps(reply.get("return", {})))
PY

[ -s "$PPM" ] || die "QMP returned no screendump at $PPM"

# PPM -> PNG in pure python: zlib and struct are in the standard library, so
# this needs no imagemagick or netpbm on the build host.
log "Converting $PPM -> $OUT"
python3 - "$PPM" "$OUT" <<'PY'
import struct, sys, zlib

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = f.read()

# Parse the P6 header: magic, width, height, maxval - whitespace separated,
# with '#' comments allowed.
fields, pos = [], 2
while len(fields) < 3:
    while pos < len(data) and data[pos:pos + 1].isspace():
        pos += 1
    if data[pos:pos + 1] == b"#":
        while pos < len(data) and data[pos:pos + 1] != b"\n":
            pos += 1
        continue
    start = pos
    while pos < len(data) and not data[pos:pos + 1].isspace():
        pos += 1
    fields.append(int(data[start:pos]))
pos += 1  # single whitespace byte before the raster
w, h, maxval = fields
if maxval != 255:
    sys.exit(f"unsupported PPM maxval {maxval}")
rgb = data[pos:pos + w * h * 3]

raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

def chunk(tag, payload):
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw, 6))
       + chunk(b"IEND", b""))
with open(dst, "wb") as f:
    f.write(png)
print(f"wrote {dst} ({w}x{h})")
PY

ok "screenshot: $OUT"
