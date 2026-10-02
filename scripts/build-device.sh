#!/usr/bin/env bash
# Astrix OS - assemble a flash bundle for a device.
#
#   ./scripts/build-device.sh <device>            build the bundle
#   ./scripts/build-device.sh <device> --dry-run  print the plan, build nothing
#
# The bundle is what scripts/flash-device.sh consumes:
#
#   build/device/<device>/boot.img    kernel + initramfs, flashable boot image
#   build/device/<device>/dtb.img     the device tree, when one exists
#   build/device/<device>/rootfs.img  the userspace
#
# WHY THE GATE IS AT THE TOP
# --------------------------
# Both target phones are unsupported, and this script therefore refuses both of
# them. That is not a stub and it is not an oversight - it is the same rule the
# flasher applies, in the same direction: a bundle that nobody has booted from
# is not a bundle, it is a guess with a filename on it. The gates below are
# ordered so that the cheapest, most damning one runs first.
#
#   Gate 1  device.sh check must pass (profile, kernel config, link budget)
#   Gate 2  DEVICE_BOOT_STATUS=supported AND DEVICE_VERIFIED_BOOT=yes
#   Gate 3  every input artefact must exist
#
# Gate 2 cannot be passed today, for any device, because no physical device has
# ever booted Astrix. When somebody does observe a boot, they set
# DEVICE_VERIFIED_BOOT=yes in the profile with evidence in docs/STATUS.md, and
# this script stops refusing. Until then it is a checklist that is honest about
# being a checklist.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

DEVICE_DIR="${ASTRIX_REPO_ROOT}/config/devices"
KERNEL_IMAGE="${ASTRIX_BUILD_DIR}/out/boot/Image"
ROOTFS_TREE="${ASTRIX_BUILD_DIR}/rootfs"
# Overhead for an ext4 image of the userspace: metadata, journal and the
# slack that a filesystem always ends up with. 512 MiB is not an estimate, it
# is what the current tree needs plus room to grow without a reflash.
ROOTFS_IMAGE_MIB=3072
MODE="build"

usage() {
  sed -n '2,30p' "$0"
  cat <<'EOF'

usage:
  build-device.sh <device>             assemble build/device/<device>/
  build-device.sh <device> --dry-run   print the plan and stop

Devices:
EOF
  ls "${DEVICE_DIR}" 2>/dev/null | sed 's/\.conf$//' | sed 's/^/  /'
  printf '\n'
}

DEVICE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) MODE="dry-run"; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) err "unknown option: $1"; usage; exit 1 ;;
    *)  DEVICE="$1"; shift ;;
  esac
done
[ -n "$DEVICE" ] || { usage; exit 1; }

PROFILE="${DEVICE_DIR}/${DEVICE}.conf"
if [ ! -f "$PROFILE" ]; then
  err "no such device profile: ${DEVICE}"
  dim "known: $(ls "${DEVICE_DIR}" | sed 's/\.conf$//' | tr '\n' ' ')"
  exit 1
fi
# shellcheck source=/dev/null
source "$PROFILE"

printf '\n  Device:  %s (%s)\n' "${DEVICE_NAME}" "${DEVICE_CODENAME}"
printf '  SoC:     %s\n' "${DEVICE_SOC}"
printf '  Panel:   %sx%s @ %sHz\n' "${DEVICE_PANEL_WIDTH}" "${DEVICE_PANEL_HEIGHT}" \
  "${DEVICE_PANEL_REFRESH}"
printf '  Bundle:  %s\n\n' "${ASTRIX_BUILD_DIR}/device/${DEVICE}"

# ---------------------------------------------------------------------------
# Gate 1: the profile and the kernel config must be coherent.
# ---------------------------------------------------------------------------
log "Checking the device profile"
if ! bash "${SCRIPT_DIR}/device.sh" check "$DEVICE"; then
  err "device.sh check ${DEVICE} failed; refusing to build a bundle."
  dim "  the profile asks for something the kernel config does not provide."
  exit 4
fi

# ---------------------------------------------------------------------------
# Gate 2: someone must have actually booted this.
# ---------------------------------------------------------------------------
if [ "${DEVICE_BOOT_STATUS}" != "supported" ] || [ "${DEVICE_VERIFIED_BOOT}" != "yes" ]; then
  err "Refusing to build a flash bundle: this device has not booted Astrix."
  printf '\n'
  dim "  status=${DEVICE_BOOT_STATUS} verified-boot=${DEVICE_VERIFIED_BOOT}"
  dim "  A bundle is a bootable system, not a directory of images. Producing"
  dim "  one for hardware that has never been booted would only make it easier"
  dim "  to brick a phone."
  if [ -n "${DEVICE_BOOT_BLOCKERS:-}" ]; then
    printf '\n'
    dim "  Blockers:"
    printf '%s\n' "${DEVICE_BOOT_BLOCKERS}" | tr ',' '\n' | sed 's/^/    - /'
  fi
  if [ -n "${DEVICE_PORT_STEPS:-}" ]; then
    printf '\n'
    dim "  Ordered path to a boot:"
    printf '%s\n' "${DEVICE_PORT_STEPS}" | tr ',' '\n' | sed 's/^/    /'
    printf '\n'
    dim "  docs/PORTING.md has the concrete bring-up order for each step."
  fi
  printf '\n'
  exit 2
fi

# ---------------------------------------------------------------------------
# Gate 3: the inputs have to exist.
# ---------------------------------------------------------------------------
require_input() {
  if [ -e "$1" ]; then
    ok "$1"
  else
    err "missing build input: $1"
    dim "  run ./build.sh first (it produces both the kernel and the rootfs)"
    return 1
  fi
}

log "Checking build inputs"
missing=0
require_input "$KERNEL_IMAGE" || missing=$((missing + 1))
if [ -d "$ROOTFS_TREE" ] && [ -d "${ROOTFS_TREE}/usr" ]; then
  ok "${ROOTFS_TREE} ($(du -sh "$ROOTFS_TREE" 2>/dev/null | cut -f1))"
else
  err "missing build input: the rootfs tree at ${ROOTFS_TREE}"
  dim "  run ./build.sh first (it produces both the kernel and the rootfs)"
  missing=$((missing + 1))
fi
if [ "$missing" -ne 0 ]; then
  err "${missing} build input(s) missing; nothing was assembled."
  exit 1
fi

DTB="${ASTRIX_REPO_ROOT}/config/devices/${DEVICE}.dtb"
have_dtb="no"
[ -f "$DTB" ] && have_dtb="yes"

plan() {
  printf '  %s\n' "$*"
}

printf '\n  Plan:\n'
plan "mkdir -p ${ASTRIX_BUILD_DIR}/device/${DEVICE}"
if command -v mkbootimg >/dev/null; then
  plan "mkbootimg --kernel ${KERNEL_IMAGE} --ramdisk ... --output boot.img"
else
  warn "mkbootimg not found (apt-get install android-sdk-libsparse-utils / android-tools)"
  warn "boot.img would have to be produced by it; the bundle is incomplete without it"
fi
if [ "$have_dtb" = "yes" ]; then
  plan "dtb.img <- ${DTB}"
else
  warn "no device tree at ${DTB}; there is nothing to describe this board's"
  warn "regulators, clocks and panel, so no dtb.img can be produced"
fi
plan "rootfs.img <- ext4 image of ${ROOTFS_TREE} (${ROOTFS_IMAGE_MIB} MiB)"

if [ "$MODE" = "dry-run" ]; then
  printf '\n'
  ok "dry run complete; nothing was built."
  exit 0
fi

# ---------------------------------------------------------------------------
# Assembly. Unreachable until a boot has been observed - see Gate 2.
# ---------------------------------------------------------------------------
command -v mkbootimg >/dev/null || die "mkbootimg not found; see the plan above"
OUT="${ASTRIX_BUILD_DIR}/device/${DEVICE}"
mkdir -p "$OUT"
log "Assembling the bundle in ${OUT}"
mkbootimg --kernel "$KERNEL_IMAGE" \
          --ramdisk "${ROOTFS_TREE}/boot/initrd.img" \
          --output "${OUT}/boot.img"
[ "$have_dtb" = "yes" ] && cp "$DTB" "${OUT}/dtb.img"

require_cmd mkfs.ext4 || die "mkfs.ext4 not found (apt-get install e2fsprogs)"
ROOTFS_SRC="${ASTRIX_BUILD_DIR}/rootfs-device-${DEVICE}.ext4"
rm -f "$ROOTFS_SRC"
truncate -s "${ROOTFS_IMAGE_MIB}M" "$ROOTFS_SRC"
mkfs.ext4 -q -F -L astrixroot "$ROOTFS_SRC" >/dev/null
# Populating an ext4 image needs a loop mount; that is a privileged operation
# and deliberately not done implicitly. The image is built and left empty for
# the caller to populate, rather than being silently produced half-finished.
ok "rootfs image scaffolded: ${ROOTFS_SRC}"
warn "it is EMPTY until populated (needs a loop mount):"
dim "  sudo mount -o loop ${ROOTFS_SRC} /mnt/astrix && cp -a ${ROOTFS_TREE}/. /mnt/astrix/ && sudo umount /mnt/astrix"
cp "$ROOTFS_SRC" "${OUT}/rootfs.img"

printf '\n'
ok "bundle written to ${OUT}"
warn "It has never been booted. Flashing it is still gated:"
dim "  ./scripts/flash-device.sh ${DEVICE} --dry-run"
printf '\n'