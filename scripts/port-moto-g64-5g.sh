#!/usr/bin/env bash
# Astrix OS - get a real Astrix onto a moto g64 5G (codename "retin").
#
#   ./scripts/port-moto-g64-5g.sh status              what is real, and what is not
#   ./scripts/port-moto-g64-5g.sh android-host pack   build the slim userspace bundle
#   ./scripts/port-moto-g64-5g.sh android-host check  preflight the phone over adb
#   ./scripts/port-moto-g64-5g.sh android-host push   copy the bundle onto the phone
#   ./scripts/port-moto-g64-5g.sh android-host start  launch it (prints the command)
#   ./scripts/port-moto-g64-5g.sh android-host stop   stop it
#   ./scripts/port-moto-g64-5g.sh boot-test build     build a fastboot boot-test image
#   ./scripts/port-moto-g64-5g.sh boot-test stage     flash it to the phone's boot
#   ./scripts/port-moto-g64-5g.sh boot-test log       capture the USB serial proof
#   ./scripts/port-moto-g64-5g.sh boot-test rollback  how to get Android back
#
# ---------------------------------------------------------------------------
# THIS BOARD IS CLOSER TO BOOTING THAN ANY OTHER, AND STILL DOES NOT BOOT
# ---------------------------------------------------------------------------
# The GPU is a Mali-G57 MC2, and **Panfrost is a real open-source DRM driver in
# mainline Linux for it**. That is a genuine GPU path, and no other device
# profile here has one. It is also, on its own, not a display.
#
# What is missing is the other half: MediaTek's MT6855 display pipeline has no
# mainline driver at all - the DSI controller, the panel driver, and the GPIO
# sequence for panel reset and enable are all downstream-only. A GPU with
# nothing to present a scanout to is not a screen. There is also no upstream
# device tree for retin, and an unlocked Motorola bootloader only knows how to
# chainload an Android boot image, so Astrix's boot chain has to be built.
#
# That makes this board the better port of the two, and it does not make it a
# port that has happened. The three routes below are the honest summary.
#
#   android-host  Astrix USERSPACE, on the phone's own Android kernel.
#                 No unlock, no flash, nothing destroyed.
#                 PROVES: the GUI works on this SoC and this 1080x2400 panel.
#                 NOT PROVES: anything about Astrix's kernel.
#
#   boot-test     Astrix's KERNEL, flashed to boot, verified over USB serial.
#                 PROVES: Astrix's kernel starts on retin and reaches
#                 userspace. Motorola's unlock token is friendlier than
#                 Xiaomi's, which makes this the cheaper of the two boot
#                 tests - but the screen still stays dark.
#                 NOT PROVES: a picture. Same missing display driver.
#
#   no-boot       tests/test-device-panels.sh renders every screen at this
#                 panel's exact 1080x2400 geometry and asserts the dock, the
#                 keyboard and the status bar all fit and all accept a tap.
#                 No phone, no bootloader, no flash. This one is a pass/fail
#                 test today, not a plan.
#
# One warning specific to this panel, and it is arithmetic rather than opinion:
# 1080x2400 at 120Hz needs 2.258 Gbit/s per lane against a 2.5 Gbit/s lane -
# 10% headroom, which is where DSI link training starts failing in ways that
# look like driver bugs. Bring the panel up at **60Hz first** (55% headroom).
# scripts/display-budget.sh computes both.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

DEVICE="moto-g64-5g"
DEVICE_DIR="${ASTRIX_DEVICE_DIR:-${ASTRIX_REPO_ROOT}/config/devices}"
PROFILE="${DEVICE_DIR}/${DEVICE}.conf"
[ -f "$PROFILE" ] || die "no device profile at ${PROFILE}"
# shellcheck source=/dev/null
source "$PROFILE"

# shellcheck source=port-common.sh
source "${SCRIPT_DIR}/port-common.sh"

usage() {
  sed -n '2,/^set -euo/p' "$0" | sed '$d'
  port_usage_tail
}

port_dispatch "$@"