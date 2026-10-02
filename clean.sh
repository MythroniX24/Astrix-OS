#!/usr/bin/env bash
# Astrix OS - remove build artefacts.
#
#   ./clean.sh            remove build outputs, keep the download cache
#   ./clean.sh --all      also remove the cache (kernel tarball, package index)
#   ./clean.sh --dist     remove everything including the rootfs
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"

ALL=0
DIST=0
while [ $# -gt 0 ]; do
  case "$1" in
    --all) ALL=1; shift ;;
    --dist) DIST=1; shift ;;
    -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

BUILD="${REPO_ROOT}/build"

# Refuse to delete anything unexpected: this script removes a directory tree.
if [ ! -d "$BUILD" ]; then
  info "nothing to clean (no $BUILD directory)"
  exit 0
fi

# Only ever remove the known build directory.
case "$BUILD" in
  */build) ;;
  *) die "refusing to clean unexpected path: $BUILD" ;;
esac

if [ "$DIST" -eq 1 ]; then
  log "Removing everything (rootfs, image, kernel, cache)"
  rm -rf "$BUILD"
elif [ "$ALL" -eq 1 ]; then
  log "Removing build outputs and the download cache"
  rm -rf "$BUILD"/*
else
  log "Removing build outputs (keeping the download cache)"
  for item in rootfs stage-root mnt kernel out image-info.env "${OS_NAME:-astrix}.img" \
              qemu-serial.log gui screens; do
    if [ -e "${BUILD}/${item}" ]; then
      info "  rm ${item}"
      rm -rf "${BUILD:?}/${item}"
    fi
  done
  # Anything else that is clearly a build artefact.
  find "$BUILD" -maxdepth 1 -name '.packages*' -delete 2>/dev/null || true
fi

ok "clean complete"
[ -d "$BUILD" ] && dim "remaining: $(ls -A "$BUILD" | tr '\n' ' ')"
exit 0
