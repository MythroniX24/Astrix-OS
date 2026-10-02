#!/usr/bin/env bash
# Astrix OS - validate that every package named in config/*.txt exists for the
# target architecture in the configured Debian suite.
#
# This exists because a typo like "liblcms2-1" (trixie ships liblcms2-2) only
# fails much later, deep inside a rootfs build. Catching it here is cheap.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
# shellcheck source=../config/os.conf
source config/os.conf

MIRROR="${DEBIAN_MIRROR}"
SUITE="${DEBIAN_SUITE}"
ARCH="${DEB_ARCH}"
CACHE="${ASTRIX_CACHE_DIR:-/tmp/astrix-cache}"
mkdir -p "$CACHE"

INDEX="$CACHE/${SUITE}-${ARCH}-Packages"

fetch_index() {
  if [ -s "$INDEX" ]; then
    return 0
  fi
  echo "==> Fetching ${SUITE}/${ARCH} package index (this is a large download)"
  local xz="$INDEX.xz"
  if command -v xz >/dev/null 2>&1; then
    curl -fsSL -o "$xz" "${MIRROR}/dists/${SUITE}/main/binary-${ARCH}/Packages.xz"
    xz -df "$xz"
  else
    curl -fsSL -o "$INDEX" "${MIRROR}/dists/${SUITE}/main/binary-${ARCH}/Packages"
  fi
  [ -s "$INDEX" ] || { echo "FATAL: could not obtain package index" >&2; exit 2; }
}

if [ "${1:-}" = "--offline" ] && [ ! -s "$INDEX" ]; then
  echo "SKIP: no cached index and --offline requested"
  exit 0
fi

fetch_index

# Collect every "Package:" stanza name once, into a lookup file.
NAMES="$CACHE/${SUITE}-${ARCH}-names"
awk '/^Package: /{print $2}' "$INDEX" | sort -u > "$NAMES"

total=0
missing=0
for list in config/packages.txt config/packages-dev.txt config/packages-optional.txt; do
  [ -f "$list" ] || continue
  echo "==> Checking $list"
  while read -r pkg; do
    case "$pkg" in ''|'#'*) continue ;; esac
    total=$((total + 1))
    if ! grep -qxF "$pkg" "$NAMES"; then
      echo "    MISSING: $pkg"
      missing=$((missing + 1))
    fi
  done < <(sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$list" | grep -v '^$')
done

echo
if [ "$missing" -gt 0 ]; then
  echo "FAIL: $missing of $total packages do not exist in ${SUITE}/${ARCH}"
  exit 1
fi
echo "PASS: all $total packages exist in ${SUITE}/${ARCH}"
