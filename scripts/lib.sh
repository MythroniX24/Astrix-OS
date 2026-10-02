#!/usr/bin/env bash
# Astrix OS - shared build helpers.
#
# Sourced by every build script. Provides consistent logging, error handling
# and host preflight checks so that individual scripts stay readable.

# Guard against double-sourcing.
[ -n "${ASTRIX_COMMON_SOURCED:-}" ] && return 0
ASTRIX_COMMON_SOURCED=1

# --- colours (disabled when not a tty) -------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  C_RESET=$'\033[0m'; C_DIM=$'\033[2m'; C_RED=$'\033[31m'
  C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_BLUE=$'\033[34m'; C_BOLD=$'\033[1m'
else
  C_RESET=""; C_DIM=""; C_RED=""; C_GREEN=""; C_YELLOW=""; C_BLUE=""; C_BOLD=""
fi

log()   { printf '%s==>%s %s\n' "$C_BLUE$C_BOLD" "$C_RESET" "$*"; }
info()  { printf '    %s\n' "$*"; }
dim()   { printf '%s    %s%s\n' "$C_DIM" "$*" "$C_RESET"; }
ok()    { printf '%s  ok%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
# An individual assertion inside a test that made several. Distinct from ok(),
# which marks a whole completed step, so a long test reads as a checklist.
pass()  { printf '%s  +%s %s\n' "$C_GREEN" "$C_RESET" "$*"; }
warn()  { printf '%swarn:%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
err()   { printf '%serror:%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; }
die()   { err "$*"; exit 1; }

# Show the real cause of a failure instead of a generic message. Rule 11/12 of
# the project: never hide a build error.
on_error() {
  local code=$?
  err "build step failed (exit ${code}) at line ${BASH_LINENO[0]}: ${BASH_COMMAND}"
  exit "$code"
}

# --- repository layout ------------------------------------------------------
astrix_init_paths() {
  ASTRIX_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  # shellcheck source=../config/os.conf
  source "${ASTRIX_REPO_ROOT}/config/os.conf"
  ASTRIX_BUILD_DIR="${ASTRIX_REPO_ROOT}/build"
  ASTRIX_CACHE_DIR="${ASTRIX_CACHE_DIR:-${ASTRIX_BUILD_DIR}/cache}"
  export ASTRIX_REPO_ROOT ASTRIX_BUILD_DIR ASTRIX_CACHE_DIR
  mkdir -p "$ASTRIX_BUILD_DIR" "$ASTRIX_CACHE_DIR"
}

# --- host preflight ---------------------------------------------------------
# Fails early with a precise message instead of dying 200 lines into a build.
require_cmd() {
  local missing=0
  for c in "$@"; do
    command -v "$c" >/dev/null 2>&1 || { err "required tool not found: $c"; missing=1; }
  done
  return $missing
}

require_host_tools() {
  require_cmd mmdebstrap qemu-system-aarch64 wayland-scanner || true
}

# Confirm we can actually execute foreign-architecture binaries. Without this,
# mmdebstrap fails deep inside with a confusing chroot error.
#
# Note the name mapping: Debian's architecture name for ARM64 is "arm64", but
# binfmt_misc registers the interpreter under "qemu-aarch64" (the upstream
# kernel name). Checking for "qemu-arm64" always fails even on a host where
# cross-execution works perfectly.
binfmt_arch_name() {
	case "$1" in
		arm64) echo "aarch64" ;;
		armhf) echo "arm" ;;
		*)     echo "$1" ;;
	esac
}

require_binfmt() {
	local deb_arch="$1"
	local bfd
	bfd="$(binfmt_arch_name "$deb_arch")"

	if [ -e "/proc/sys/fs/binfmt_misc/qemu-${bfd}" ]; then
		return 0
	fi
	if [ -e /proc/sys/fs/binfmt_misc/register ]; then
		warn "binfmt_misc is not registering ${bfd}; enabling it"
		if command -v update-binfmts >/dev/null 2>&1; then
			update-binfmts --enable "qemu-${bfd}" >/dev/null 2>&1 || true
		fi
	fi
	if [ ! -e "/proc/sys/fs/binfmt_misc/qemu-${bfd}" ]; then
		err "cannot execute ${deb_arch} binaries (binfmt_misc not registered for ${bfd})."
		err "On Debian/Ubuntu: apt-get install qemu-user-static binfmt-support"
		return 1
	fi
}

# --- chroot helpers ---------------------------------------------------------
# Build a rootfs needs /proc, /sys and /dev for dpkg postinst scripts
# (python3-minimal's postinst fails without them). Always unmount on exit.
ASTRIX_MOUNTED=()

mount_rootfs() {
  local r="$1"
  mkdir -p "$r/proc" "$r/sys" "$r/dev" "$r/run"
  mountpoint -q "$r/proc" || { mount -t proc proc "$r/proc" && ASTRIX_MOUNTED+=("$r/proc"); }
  mountpoint -q "$r/sys"  || { mount --rbind /sys "$r/sys" && ASTRIX_MOUNTED+=("$r/sys"); }
  mountpoint -q "$r/dev"  || { mount --rbind /dev "$r/dev" && ASTRIX_MOUNTED+=("$r/dev"); }
}

umount_rootfs() {
  local r="$1" m
  # Unmount in reverse order.
  for (( idx=${#ASTRIX_MOUNTED[@]}-1; idx>=0; idx-- )); do
    m="${ASTRIX_MOUNTED[idx]}"
    umount -lf "$m" 2>/dev/null || true
  done
  ASTRIX_MOUNTED=()
}

# Run a command inside the target rootfs with the right DNS.
in_rootfs() {
  local root="$1"; shift
  # systemd-nspawn is overkill; chroot + explicit env is enough and has no
  # dependency on the host having systemd-nspawn.
  chroot "$root" /usr/bin/env \
    DEBIAN_FRONTEND=noninteractive \
    DEBCONF_NONINTERACTIVE_SEEN=true \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    TERM="${TERM:-dumb}" \
    http_proxy="${http_proxy:-}" https_proxy="${https_proxy:-}" \
    "$@"
}

# in_rootfs_path <rootfs> <host-path>
#
# Print the path as it is seen *inside* a chroot of <rootfs>. in_rootfs() does a
# plain chroot with no path rewriting, so a host path passed to a command that
# runs inside the chroot has to be translated first. Getting this wrong fails
# with a confusing "No such file or directory" that looks like a missing
# dependency rather than a path bug.
in_rootfs_path() {
  local root="$1" path="$2"
  case "$path" in
    "$root"/*) printf '/%s\n' "${path#"$root"/}" ;;
    "$root")   printf '/\n' ;;
    *)         printf '%s\n' "$path" ;;
  esac
}

# Copy a file/dir into the rootfs, creating parents.
install_into_rootfs() {
  local root="$1" src="$2" dst="$3" mode="${4:-}"
  mkdir -p "$root$(dirname "$dst")"
  cp -a "$src" "$root$dst"
  if [ -n "$mode" ]; then chmod "$mode" "$root$dst"; fi
}

# --- misc -------------------------------------------------------------------
# Human-readable size, used for image reporting.
human_size() {
  du -sh "$1" 2>/dev/null | cut -f1
}
