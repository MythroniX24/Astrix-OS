#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Send real input events into a running Astrix VM.
#
# Why: "the shell mapped a surface" and "a finger on the screen opens an app"
# are very different claims, and on a phone the second one is the whole point.
# QEMU can inject genuine input events over QMP, which travel the entire real
# path:
#
#   QMP input-send-event -> virtio input device -> guest evdev
#     -> libseat -> libinput -> the compositor's forward_* handlers
#     -> wl_pointer/wl_touch -> the shell's gesture recogniser -> navigation
#
# Nothing here pokes at the OS from inside: if a tap opens an app, that is
# because the OS really handled a touch, not because a test called the same
# function the touch would.
#
# Coordinates are NORMALISED (0..1) across the device, not pixels. QEMU's
# virtio input devices use a 0..0x7FFF absolute range (VIRTIO_INPUT_ABS_MAX)
# regardless of the guest's screen size, so (0.5, 0.5) is the middle of
# whatever display the VM ends up with - which is exactly what a test wants
# when the panel resolution may differ from the shell's layout.
#
# Usage:
#   scripts/send-input.sh tap 0.5 0.7
#   scripts/send-input.sh swipe 0.5 0.8 0.5 0.2 [steps]
#   scripts/send-input.sh swipe-hold 0.5 0.8 0.5 0.3 [steps] [seconds]
#   scripts/send-input.sh hold 0.5 0.5 [seconds]
#   scripts/send-input.sh key ret
#
# Set ASTRIX_INPUT_DEVICE to a device name to address the events at one
# specific input device, e.g. ASTRIX_INPUT_DEVICE="QEMU Virtio Tablet".
# Unset, QEMU picks the device itself - which is correct when the VM has only
# one pointing device.
#
# Needs: a VM started by qemu/run.sh (it opens the QMP socket), python3.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib.sh
. "$REPO_ROOT/scripts/lib.sh"

QMP="${ASTRIX_QMP_SOCK:-$REPO_ROOT/build/qmp.sock}"
[ -S "$QMP" ] || die "no QMP socket at $QMP - start the VM with ./run-qemu.sh first"
command -v python3 >/dev/null 2>&1 || die "python3 is required to talk to QMP"

QL="$REPO_ROOT/scripts/qmp_input.py"
D="${ASTRIX_INPUT_DEVICE:-}"

# Not exec: the ok line below is the confirmation that the events were
# accepted, and exec would replace this shell before it printed.
case "${1:-}" in
tap)   python3 "$QL" tap   "${QMP}" "$D" "${2:?x}" "${3:?y}" ;;
swipe) python3 "$QL" swipe "${QMP}" "$D" "${2:?x1}" "${3:?y1}" "${4:?x2}" "${5:?y2}" "${6:-12}" ;;
hold)  python3 "$QL" hold  "${QMP}" "$D" "${2:?x}" "${3:?y}" "${4:-1.2}" ;;
swipe-hold) python3 "$QL" swipe-hold "$QMP" "$D" "${2:?x1}" "${3:?y1}" "${4:?x2}" "${5:?y2}" "${6:-12}" "${7:-0.9}" ;;
key)   python3 "$QL" key   "${QMP}" "$D" "${2:?key}" ;;
*)     die "usage: $0 {tap|swipe|swipe-hold|hold|key} ..." ;;
esac

ok "input sent via $QMP"
