#!/usr/bin/env bash
# Astrix OS - build the whole operating system.
#
#   ./build.sh              full build (rootfs + GUI + image)
#   ./build.sh --full       also install the developer toolchain
#   ./build.sh --with-kernel  additionally build the in-tree kernel
#   ./build.sh --gui-only   rebuild just the graphical stack
#   ./build.sh --rootfs-only
#
# Each stage is skipped if its output already exists, so a rebuild after a GUI
# change is fast. Use ./clean.sh to force a full rebuild.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"
trap on_error ERR

astrix_init_paths

FULL=0
WITH_KERNEL=0
GUI_ONLY=0
ROOTFS_ONLY=0
FORCE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --full) FULL=1; shift ;;
    --with-kernel) WITH_KERNEL=1; shift ;;
    --gui-only) GUI_ONLY=1; shift ;;
    --rootfs-only) ROOTFS_ONLY=1; shift ;;
    --force) FORCE=1; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

START=$(date +%s)
ROOTFS="${ASTRIX_BUILD_DIR}/rootfs"
IMG="${ASTRIX_BUILD_DIR}/${OS_NAME}.img"
GUI="${ASTRIX_BUILD_DIR}/gui/astrix-shell"

banner() {
  printf '%s\n' "==============================================================="
  printf '  %s %s  (%s / %s)\n' "$OS_PRETTY_NAME" "$OS_VERSION" "$DEBIAN_SUITE" "$DEB_ARCH"
  printf '  target: ARM64   base: minimal Debian   build: reproducible\n'
  printf '%s\n' "==============================================================="
}

if [ "$(id -u)" -ne 0 ]; then
  die "build.sh must run as root: mmdebstrap, mkfs and loopback mounts all need it"
fi

banner
info "build dir: $ASTRIX_BUILD_DIR"
info "source epoch: ${SOURCE_DATE_EPOCH}"
echo

# ---------------------------------------------------------------------------
# 1. Root filesystem
# ---------------------------------------------------------------------------
if [ "$GUI_ONLY" -eq 0 ]; then
  if [ -d "$ROOTFS" ] && [ "$FORCE" -eq 0 ]; then
    log "rootfs already present, skipping (use --force to rebuild)"
    ok "rootfs: $ROOTFS"
  else
    ARGS=()
    [ "$FULL" -eq 1 ] && ARGS+=(--full)
    "${REPO_ROOT}/scripts/build-rootfs.sh" "${ARGS[@]}" || die "rootfs build failed"
  fi
fi

if [ "$ROOTFS_ONLY" -eq 1 ]; then
  ok "rootfs-only build complete ($(($(date +%s) - START))s)"
  exit 0
fi

# ---------------------------------------------------------------------------
# 2. Kernel (optional)
# ---------------------------------------------------------------------------
if [ "$WITH_KERNEL" -eq 1 ]; then
  if [ -f "${ASTRIX_BUILD_DIR}/out/boot/Image" ] && [ "$FORCE" -eq 0 ]; then
    log "kernel already built, skipping"
  else
    "${REPO_ROOT}/scripts/build-kernel.sh" || die "kernel build failed"
  fi
else
  dim "skipping the in-tree kernel (--with-kernel to build it)"
  dim "using the Debian arm64 kernel from the rootfs for now"
fi

# ---------------------------------------------------------------------------
# 3. Graphical stack
# ---------------------------------------------------------------------------
if [ -x "$GUI" ] && [ "$FORCE" -eq 0 ]; then
  log "GUI already built, skipping (use --force to rebuild)"
  ok "GUI: $GUI"
else
  # The GUI is compiled inside the rootfs, which needs a compiler there.
  if [ ! -x "$ROOTFS/usr/bin/gcc" ]; then
    die "the rootfs has no C compiler; rebuild with --full to install the dev packages"
  fi
  "${REPO_ROOT}/scripts/build-gui.sh" || die "GUI build failed"
fi

# ---------------------------------------------------------------------------
# 4. Image
# ---------------------------------------------------------------------------
if [ -f "$IMG" ] && [ "$FORCE" -eq 0 ]; then
  log "image already built, skipping (use --force to rebuild)"
  ok "image: $IMG"
else
  "${REPO_ROOT}/scripts/build-image.sh" || die "image build failed"
fi

# ---------------------------------------------------------------------------
# 5. Host tests
# ---------------------------------------------------------------------------
echo
"${REPO_ROOT}/tests/run-all.sh" || warn "some host tests failed (see above)"

ELAPSED=$(($(date +%s) - START))
banner
ok "BUILD COMPLETE in ${ELAPSED}s"
printf '\n'
info "image:   $IMG ($(du -h "$IMG" 2>/dev/null | cut -f1))"
info "rootfs:  $ROOTFS ($(human_size "$ROOTFS"))"
info "kernel:  ${ASTRIX_BUILD_DIR}/out/boot/Image (if built)"
printf '\n'
printf '  Boot it:   ./run-qemu.sh\n'
printf '  Headless:  ./run-qemu.sh --headless\n'
printf '  Rebuild:   ./build.sh --force\n'
printf '\n'
