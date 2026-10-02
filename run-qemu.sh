#!/usr/bin/env bash
# Astrix OS - boot the built image in QEMU.
#
# Thin wrapper around qemu/run.sh so the documented command is short and lives
# at the top of the repository, where a new user will look for it.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"

IMG="${REPO_ROOT}/build/${OS_NAME:-astrix}.img"
if [ ! -f "$IMG" ]; then
  die "no image at $IMG - run ./build.sh first"
fi

exec "${REPO_ROOT}/qemu/run.sh" "$@"
