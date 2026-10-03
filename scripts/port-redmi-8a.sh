#!/usr/bin/env bash
# Astrix OS - get a real Astrix onto a Redmi 8A (codename "olive").
#
#   ./scripts/port-redmi-8a.sh status              what is real, and what is not
#   ./scripts/port-redmi-8a.sh android-host pack   build the slim userspace bundle
#   ./scripts/port-redmi-8a.sh android-host check  preflight the phone over adb
#   ./scripts/port-redmi-8a.sh android-host push   copy the bundle onto the phone
#   ./scripts/port-redmi-8a.sh android-host start  launch it (prints the command)
#   ./scripts/port-redmi-8a.sh android-host stop   stop it
#   ./scripts/port-redmi-8a.sh boot-test build     build a fastboot boot-test image
#   ./scripts/port-redmi-8a.sh boot-test stage     flash it to the phone's boot
#   ./scripts/port-redmi-8a.sh boot-test log       capture the USB serial proof
#   ./scripts/port-redmi-8a.sh boot-test rollback  how to get Android back
#
# ---------------------------------------------------------------------------
# THE ONE THING TO UNDERSTAND BEFORE USING THIS
# ---------------------------------------------------------------------------
# "Astrix boots on the Redmi 8A" is currently FALSE, and no script here can
# make it true. The GPU is an Adreno 505 and mainline Linux has no driver for
# it at all - not a stub, not an out-of-tree patch, none. There is also no
# upstream device tree for olive. So an Astrix kernel cannot light this screen
# even with the bootloader unlocked and a perfect .config.
#
# That is a missing-driver fact, not a missing-configuration fact, and it is
# why there are two very different routes instead of pretending there is one.
# They are named for what they actually prove, because the difference matters
# the moment something goes wrong on a phone you paid for.
#
#   android-host  Astrix USERSPACE, on the phone's own Android kernel.
#                 No unlock, no flash, no drivers, nothing destroyed. The
#                 compositor, the shell, the apps and the modern UI - the parts
#                 people actually want to see - all of it, on the real device.
#                 PROVES: the GUI works on this SoC and this panel.
#                 NOT PROVES: anything about Astrix's kernel. Android is still
#                 in charge below it. This is running Astrix, not booting it.
#
#   boot-test     Astrix's KERNEL, flashed to the phone's boot partition,
#                 booted, and verified over the USB serial console.
#                 PROVES: Astrix's kernel starts on olive and reaches
#                 userspace. That is a real, permanent result.
#                 NOT PROVES: a picture. The screen stays dark for the entire
#                 boot - there is no display driver to draw with. Android is
#                 replaced until you flash a stock image back.
#
# So: want to SEE Astrix? android-host. Want to PROVE the kernel boots?
# boot-test. Want to check the layout before either? tests/test-device-panels.sh
# needs no phone at all.
#
# The machinery both routes share lives in scripts/port-common.sh; only what is
# specific to this board is here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

DEVICE="redmi-8a"
DEVICE_DIR="${ASTRIX_DEVICE_DIR:-${ASTRIX_REPO_ROOT}/config/devices}"
PROFILE="${DEVICE_DIR}/${DEVICE}.conf"
[ -f "$PROFILE" ] || die "no device profile at ${PROFILE}"
# shellcheck source=/dev/null
source "$PROFILE"

# shellcheck source=port-common.sh
source "${SCRIPT_DIR}/port-common.sh"

usage() {
  # Print everything between the shebang and the first real line of code, so
  # the header cannot drift out of sync with the text printed here.
  sed -n '2,/^set -euo/p' "$0" | sed '$d'
  port_usage_tail
}

port_dispatch "$@"