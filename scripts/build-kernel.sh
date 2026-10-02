#!/usr/bin/env bash
# Astrix OS - build the Astrix kernel.
#
# Downloads a pinned mainline Linux, merges config/kernel/config on top of the
# arm64 defconfig, and builds Image + modules.
#
# The kernel is NOT required for the first bootable milestone: build-rootfs.sh
# installs Debian's linux-image-arm64, which boots under QEMU today. This
# script produces the in-tree kernel that will eventually be the real one, and
# is the step a physical device port depends on.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"
DEBUG=0
MODULES=1
JOBS_SET=""

while [ $# -gt 0 ]; do
  case "$1" in
    -j) JOBS="$2"; JOBS_SET=1; shift 2 ;;
    --debug) DEBUG=1; shift ;;
    --no-modules) MODULES=0; shift ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

KVER="${KERNEL_VERSION}"
KTAR="linux-${KVER}.tar.xz"
KSRC="${ASTRIX_CACHE_DIR}/linux-${KVER}"
KBUILD="${ASTRIX_BUILD_DIR}/kernel"
OUT="${ASTRIX_BUILD_DIR}/out"

log "Building Astrix kernel ${KVER} for arm64 with ${JOBS} job(s)"

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
require_cmd make || die "make not found"
if [ "$(id -u)" -ne 0 ]; then
  warn "not running as root: module signing and some targets will be skipped"
fi
for t in flex bison bc; do
  command -v "$t" >/dev/null || die "$t is required to build the kernel (apt-get install flex bison bc)"
done

# ---------------------------------------------------------------------------
# Fetch the source
# ---------------------------------------------------------------------------
if [ ! -d "$KSRC" ]; then
  mkdir -p "$ASTRIX_CACHE_DIR"
  if [ ! -f "${ASTRIX_CACHE_DIR}/${KTAR}" ]; then
    log "Downloading linux ${KVER}"
    curl -fL --retry 3 -o "${ASTRIX_CACHE_DIR}/${KTAR}.part" \
      "${KERNEL_URL}/${KTAR}" ||
      die "could not download ${KTAR} from ${KERNEL_URL}"
    mv "${ASTRIX_CACHE_DIR}/${KTAR}.part" "${ASTRIX_CACHE_DIR}/${KTAR}"
  fi
  log "Extracting kernel source"
  tar -C "$ASTRIX_CACHE_DIR" -xf "${ASTRIX_CACHE_DIR}/${KTAR}"
  [ -d "$KSRC" ] || die "expected extracted source at $KSRC"
else
  dim "reusing kernel source at $KSRC"
fi

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
mkdir -p "$KBUILD"
log "Configuring (arm64 defconfig + config/kernel/config)"

# Start from the architecture baseline, then merge our fragment on top. Doing
# it in this order means we never accidentally drop a driver a device tree
# references.
make -C "$KSRC" O="$KBUILD" ARCH=arm64 CROSS_COMPILE="${DEB_TRIPLET}-" defconfig

if [ -f "${ASTRIX_REPO_ROOT}/config/kernel/config" ]; then
  # merge_config.sh merges a fragment into the current .config.
  "$KSRC/scripts/kconfig/merge_config.sh" -m -O "$KBUILD" \
    "$KBUILD/.config" "${ASTRIX_REPO_ROOT}/config/kernel/config" > "$KBUILD/merge.log" 2>&1 ||
    {
      # Fall back to the in-tree tool when the host copy is stale.
      "$KSRC/scripts/kconfig/merge_config.sh" -m -O "$KBUILD" \
        "$KBUILD/.config" "${ASTRIX_REPO_ROOT}/config/kernel/config" ||
        { cat "$KBUILD/merge.log" >&2; die "merge_config.sh failed"; }
    }
  make -C "$KSRC" O="$KBUILD" ARCH=arm64 CROSS_COMPILE="${DEB_TRIPLET}-" olddefconfig
  ok "configuration merged"
else
  warn "config/kernel/config not found; building with the plain defconfig"
fi

if [ "$DEBUG" -eq 1 ]; then
  log "Enabling kernel debug options (--debug)"
  scripts/config --file "$KBUILD/.config" -e DEBUG_KERNEL -e KALLSYMS \
    -e CONFIG_KALLSYMS_ALL -d DEBUG_INFO_NONE
  make -C "$KSRC" O="$KBUILD" ARCH=arm64 CROSS_COMPILE="${DEB_TRIPLET}-" olddefconfig
fi

# Report the important choices so a surprise in a config diff is visible.
log "Selected kernel configuration:"
for opt in CONFIG_ARM64 CONFIG_DRM CONFIG_DRM_VIRTIO_GPU CONFIG_DRM_MEDIATEK \
           CONFIG_INPUT_TOUCHSCREEN CONFIG_ANDROID_BINDER_IPC CONFIG_ASHMEM \
           CONFIG_SECURITY_APPARMOR CONFIG_EXT4_FS CONFIG_F2FS_FS \
           CONFIG_PM_SLEEP CONFIG_BT CONFIG_MMC_SDHCI; do
  val=$(grep -E "^${opt}=" "$KBUILD/.config" 2>/dev/null | head -1 | cut -d= -f2-)
  [ -z "$val" ] && val="n"
  dim "$(printf '  %-28s %s' "$opt" "$val")"
done

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
MAKE_ARGS=(-C "$KSRC" O="$KBUILD" ARCH=arm64 CROSS_COMPILE="${DEB_TRIPLET}-" "-j${JOBS}")

# Signing: a phone that accepts unsigned modules is not a phone. The key is
# generated once into the build dir and reused so images stay reproducible
# within a build tree. For a shipping device this key must be protected and
# paired with UEFI Secure Boot; see docs/SECURITY.md.
CERT="${ASTRIX_BUILD_DIR}/certs"
if [ ! -f "${CERT}/astrix.pem" ]; then
  mkdir -p "$CERT"
  log "Generating kernel module signing key (development key)"
  openssl req -new -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout "${CERT}/astrix.key" -out "${CERT}/astrix.pem" \
    -subj "/CN=Astrix OS Development Signing Key/O=Astrix" >/dev/null 2>&1
  ok "signing key at ${CERT}/astrix.pem (DO NOT use this for a shipping device)"
fi
if [ "$DEBUG" -eq 1 ]; then
  "${MAKE_ARGS[@]}" CONFIG_MODULE_SIG=n
else
  "${MAKE_ARGS[@]}" \
    CONFIG_MODULE_SIG=y \
    CONFIG_MODULE_SIG_ALL=y \
    CONFIG_MODULE_SIG_KEY="astrix" \
    CONFIG_MODULE_SIG_KEY_PATH="$CERT" \
    -j"${JOBS}"
fi

# Modules
if [ "$MODULES" -eq 1 ]; then
  log "Building modules"
  "${MAKE_ARGS[@]}" modules
fi

# ---------------------------------------------------------------------------
# Collect the output
# ---------------------------------------------------------------------------
mkdir -p "$OUT/boot"
install -m 0644 "$KBUILD/arch/arm64/boot/Image" "$OUT/boot/Image" 2>/dev/null ||
  die "kernel Image not found at $KBUILD/arch/arm64/boot/Image"
ok "kernel image: $OUT/boot/Image ($(du -h "$OUT/boot/Image" | cut -f1))"

# System.map and the config are kept: without the config a kernel panic report
# is almost impossible to act on.
[ -f "$KBUILD/System.map" ] && install -m 0644 "$KBUILD/System.map" "$OUT/boot/System.map"
[ -f "$KBUILD/.config" ] && install -m 0644 "$KBUILD/.config" "$OUT/boot/config"
[ -f "$KBUILD/vmlinux" ] && install -m 0644 "$KBUILD/vmlinux" "$OUT/boot/vmlinux" 2>/dev/null || true

if [ "$MODULES" -eq 1 ]; then
  log "Installing modules into $OUT/modules"
  rm -rf "$OUT/modules"
  mkdir -p "$OUT/modules"
  make -C "$KSRC" O="$KBUILD" ARCH=arm64 CROSS_COMPILE="${DEB_TRIPLET}-" \
    INSTALL_MOD_PATH="$OUT/modules" modules_install >/dev/null
  # Strip the debug sections: modules are the largest part of a kernel install
  # and the debug info is not needed at runtime.
  find "$OUT/modules/lib/modules" -name "*.ko" -exec strip --strip-debug {} \; 2>/dev/null || true
  ok "modules: $(du -sh "$OUT/modules" 2>/dev/null | cut -f1)"
fi

ok "kernel build complete -> $OUT"
dim "next: ./scripts/build-image.sh"
