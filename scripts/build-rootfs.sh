#!/usr/bin/env bash
# Astrix OS - build the minimal Debian ARM64 root filesystem.
#
# Produces build/rootfs/ containing a real Debian trixie arm64 userspace with
# the Astrix configuration, services and userspace files applied.
#
# The rootfs is the userspace only. The kernel is separate (see build-kernel.sh)
# and is placed into boot/ when the image is assembled.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

FULL=0
KERNEL_PKG=1
# --resume re-applies the Astrix configuration stages to an existing rootfs
# instead of re-bootstrapping it. See the resume block below.
RESUME=0
while [ $# -gt 0 ]; do
  case "$1" in
    --full)     FULL=1; shift ;;
    --no-kernel) KERNEL_PKG=0; shift ;;
    --resume)   RESUME=1; shift ;;
    --clean)    rm -rf "${ASTRIX_BUILD_DIR}/rootfs"; die "removed ${ASTRIX_BUILD_DIR}/rootfs" ;;
    -h|--help)  sed -n '2,12p' "$0"; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

ROOTFS="${ASTRIX_BUILD_DIR}/rootfs"
STAGING="${ASTRIX_BUILD_DIR}/staging"
PKG_LIST="${ASTRIX_REPO_ROOT}/config/packages.txt"
DEV_LIST="${ASTRIX_REPO_ROOT}/config/packages-dev.txt"

# ---------------------------------------------------------------------------
# 1. Preflight
# ---------------------------------------------------------------------------
log "Building ${DEBIAN_SUITE}/${DEB_ARCH} rootfs"

if [ "$(id -u)" -ne 0 ]; then
  die "must run as root (mmdebstrap needs root for the mount namespace)"
fi

require_cmd mmdebstrap || die "mmdebstrap not found: apt-get install mmdebstrap"
require_cmd curl xz || die "curl and xz are required"
require_binfmt "$DEB_ARCH" || die "cannot execute ${DEB_ARCH} binaries"

if [ -f "${DEB_ARCH}/dev/null" ]; then :; fi
if [ -d "$ROOTFS" ] && [ "${RESUME:-0}" -ne 1 ]; then
  die "${ROOTFS} already exists. Run ./clean.sh, or pass --resume to re-apply the Astrix configuration to the existing tree."
fi

if [ "${RESUME:-0}" -eq 1 ]; then
  # --resume re-runs the Astrix configuration stages (apt sources, the
  # package set, /etc overlay, user accounts, hardening, cache cleanup)
  # against a rootfs that already exists, skipping the mmdebstrap bootstrap
  # and the Debian keyring fetch. It exists so that changing a file in
  # rootfs/etc or fixing a postinst failure does not cost a full
  # re-bootstrap. It deliberately does NOT skip the package install stage:
  # the resulting image is still the full configured package set.
  log "Resuming: re-applying Astrix configuration to the existing rootfs"
fi

# mmdebstrap needs the real Debian archive keys. Ubuntu's keyring package is
# often too old for the current suite, so we fetch Debian's own keyring.
ensure_debian_keyring() {
  local kr=/usr/share/keyrings/debian-archive-keyring.gpg
  [ -s "$kr" ] && return 0
  log "Installing Debian archive keyring"
  local tmp; tmp="$(mktemp -d)"
  curl -fsSL -o "$tmp/k.deb" \
    "http://deb.debian.org/debian/pool/main/d/debian-archive-keyring/debian-archive-keyring_2025.1_all.deb" \
    || { rm -rf "$tmp"; die "could not download debian-archive-keyring"; }
  dpkg-deb -x "$tmp/k.deb" "$tmp/x"
  cp "$tmp/x/usr/share/keyrings/debian-archive-keyring.gpg" "$kr"
  rm -rf "$tmp"
}
ensure_debian_keyring
# ---------------------------------------------------------------------------
# 2. Bootstrap the base userspace
# ---------------------------------------------------------------------------
# --variant=minbase gives us Debian's own minimal set: glibc, apt, coreutils,
# bash and systemd. It is emphatically NOT a desktop environment.
log "Bootstrapping minbase rootfs (this takes a few minutes)"
if [ "${RESUME:-0}" -ne 1 ]; then
  rm -rf "$ROOTFS"
  mkdir -p "$ROOTFS"

  mmdebstrap \
    --architectures="$DEB_ARCH" \
    --variant="$DEBIAN_VARIANT" \
    "$DEBIAN_SUITE" \
    "$ROOTFS" \
    "$DEBIAN_MIRROR"
  ok "base userspace bootstrapped ($(human_size "$ROOTFS"))"
else
  ok "reusing bootstrapped rootfs ($(human_size "$ROOTFS"))"
fi

# ---------------------------------------------------------------------------
# 3. Configure apt inside the rootfs
# ---------------------------------------------------------------------------
log "Configuring apt sources"
mkdir -p "$ROOTFS/etc/apt/sources.list.d" "$ROOTFS/etc/apt/apt.conf.d" \
         "$ROOTFS/etc/apt/preferences.d" "$ROOTFS/var/lib/apt/lists/partial" \
         "$ROOTFS/var/cache/apt/archives/partial" "$ROOTFS/var/log/apt"

cat > "$ROOTFS/etc/apt/sources.list" <<EOF
deb ${DEBIAN_MIRROR} ${DEBIAN_SUITE} main contrib non-free non-free-firmware
deb ${DEBIAN_MIRROR} ${DEBIAN_SUITE}-updates main contrib non-free non-free-firmware
deb ${DEBIAN_SECURITY_MIRROR} ${DEBIAN_SECURITY_SUITE:-${DEBIAN_SUITE}-security} main contrib non-free non-free-firmware
EOF

# Phones and QEMU are both disk-backed; keep the cache small.
cat > "$ROOTFS/etc/apt/apt.conf.d/02astrix" <<'EOF'
// Astrix OS: keep the package cache small on a phone-sized flash device.
Binary::apt::APT::Keep-Downloaded-Packages "false";
Acquire::Languages "none";
APT::Install-Recommends "false";
APT::Install-Suggests "false";
EOF

# mmdebstrap leaves APT::Install-Recommends enabled; match it in our config.
sed -i 's/APT::Install-Recommends "true";/APT::Install-Recommends "false";/' \
  "$ROOTFS/etc/apt/apt.conf.d/70debconf" 2>/dev/null || true
sed -i 's/APT::Install-Suggests "true";/APT::Install-Suggests "false";/' \
  "$ROOTFS/etc/apt/apt.conf.d/70debconf" 2>/dev/null || true

# ---------------------------------------------------------------------------
# 4. Install the Astrix package set
# ---------------------------------------------------------------------------
# Build the combined package list. Blank lines and comments are stripped.
collect_packages() {
  local out="$1"; shift
  : > "$out"
  for list in "$@"; do
    [ -f "$list" ] || continue
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$list" | grep -v '^$' >> "$out" || true
  done
  # de-duplicate while preserving order
  awk '!seen[$0]++' "$out" > "$out.tmp" && mv "$out.tmp" "$out"
}

PKGS="${ASTRIX_BUILD_DIR}/.packages"
collect_packages "$PKGS" "$PKG_LIST"
if [ "$FULL" -eq 1 ]; then
  log "Including developer tooling (--full)"
  collect_packages "$PKGS" "$PKG_LIST" "$DEV_LIST"
fi

if [ "$KERNEL_PKG" -eq 0 ]; then
  log "Excluding distribution kernel (--no-kernel)"
  grep -vxF "$DEBIAN_KERNEL_PACKAGE" "$PKGS" > "$PKGS.tmp" && mv "$PKGS.tmp" "$PKGS"
fi

info "$(wc -l < "$PKGS") packages to install"

# dpkg postinst scripts need /proc /dev /sys to behave (python3-minimal's
# postinst reads /proc/self/mountinfo). Without these mounts the whole install
# aborts part-way with a confusing error.
log "Installing packages"
mount_rootfs "$ROOTFS"
trap 'umount_rootfs "$ROOTFS"' EXIT

in_rootfs "$ROOTFS" apt-get update -qq

# Install in batches so a single enormous argv cannot blow past ARG_MAX, and
# so a failure names the batch that actually broke.
install_batches() {
  local batch=() rc=0
  while read -r p; do
    batch+=("$p")
    if [ ${#batch[@]} -ge 60 ]; then
      in_rootfs "$ROOTFS" apt-get install -y -qq --no-install-recommends \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold \
        "${batch[@]}" || rc=$?
      batch=()
      [ "$rc" -ne 0 ] && return "$rc"
    fi
  done < "$PKGS"
  if [ ${#batch[@]} -gt 0 ]; then
    in_rootfs "$ROOTFS" apt-get install -y -qq --no-install-recommends \
      -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold \
      "${batch[@]}" || rc=$?
  fi
  return "$rc"
}

if ! install_batches; then
  warn "a package batch failed; running 'apt-get -f install' to reconcile"
  in_rootfs "$ROOTFS" apt-get -f install -y -qq \
    -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold \
    || die "dependency resolution failed after batch install"
fi

# Any package the first pass pulled in may itself need another pass.
in_rootfs "$ROOTFS" apt-get install -y -qq --no-install-recommends \
  -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold \
  $(grep -vxF "$DEBIAN_KERNEL_PACKAGE" "$PKGS" | tr '\n' ' ') 2>/dev/null || true

ok "packages installed ($(human_size "$ROOTFS"))"

# ---------------------------------------------------------------------------
# 5. Astrix overlay: /etc configuration
# ---------------------------------------------------------------------------
log "Applying system configuration overlay"
if [ -d "${ASTRIX_REPO_ROOT}/rootfs/etc" ]; then
  cp -a "${ASTRIX_REPO_ROOT}/rootfs/etc/." "$ROOTFS/etc/"
fi
cp -a "${ASTRIX_REPO_ROOT}/system/." "$ROOTFS/usr/lib/astrix/" 2>/dev/null || {
  mkdir -p "$ROOTFS/usr/lib/astrix"
  cp -a "${ASTRIX_REPO_ROOT}/system/." "$ROOTFS/usr/lib/astrix/"
}

# os-release: identify the system as Astrix while keeping Debian's ABI.
cat > "$ROOTFS/etc/os-release" <<EOF
NAME="${OS_PRETTY_NAME}"
VERSION="${OS_VERSION} (${DEBIAN_SUITE})"
ID=${OS_ID}
ID_LIKE=debian
PRETTY_NAME="${OS_PRETTY_NAME} ${OS_VERSION}"
VERSION_ID="${OS_VERSION}"
HOME_URL="https://github.com/astrix-os/astrix"
SUPPORT_URL="https://github.com/astrix-os/astrix/issues"
EOF

# hostname
echo "${OS_NAME}" > "$ROOTFS/etc/hostname"

# fstab. The root device is filled in by build-image.sh for QEMU; the entry
# below is the generic one used on physical devices.
mkdir -p "$ROOTFS/etc"
cat > "$ROOTFS/etc/fstab" <<'EOF'
# <file system>   <mount point>  <type>  <options>                <dump> <pass>
proc              /proc           proc   defaults                 0      0
/dev/root         /               ext4    defaults,noatime         0      1
tmpfs             /tmp            tmpfs  defaults,nosuid,nodev,size=256M 0 0
tmpfs             /run            tmpfs  defaults,nosuid,nodev,size=64M  0  0
tmpfs             /dev/shm        tmpfs  defaults,nosuid,nodev     0      0
EOF

# Locale
cat > "$ROOTFS/etc/locale.conf" <<'EOF'
LANG=C.UTF-8
EOF

# ---------------------------------------------------------------------------
# 6. Astrix user accounts
# ---------------------------------------------------------------------------
# Security requirement: the GUI must NOT run as root. We create three users:
#   astrix  - the phone user, owns the graphical session
#   android - the Waydroid container user (Phase 7)
#   astrix-pkg - a restricted helper used only for privileged package work
log "Creating system users"
# useradd's -g takes a group *name*, not a numeric gid. Passing the number
# fails with "group '1000' does not exist" on a minimal image, because the
# group has to be created first. Creating it explicitly also pins the gid, so
# the uid/gid pair is stable across rebuilds.
in_rootfs "$ROOTFS" groupadd -f -g "$ASTRIX_GID" "$ASTRIX_USER" 2>/dev/null || true
if ! in_rootfs "$ROOTFS" getent passwd "$ASTRIX_USER" >/dev/null 2>&1; then
  in_rootfs "$ROOTFS" useradd -m -u "$ASTRIX_UID" -g "$ASTRIX_USER" \
    -G "$ASTRIX_USER" -s /bin/bash -c "Astrix OS user" "$ASTRIX_USER"
fi
# If the user already existed but under a different uid/gid, force them back
# to the values in config/os.conf so file ownership inside the image is
# deterministic.
in_rootfs "$ROOTFS" usermod -u "$ASTRIX_UID" -g "$ASTRIX_USER" "$ASTRIX_USER" 2>/dev/null || true
in_rootfs "$ROOTFS" groupadd -f astrix-media >/dev/null 2>&1 || true
# The 'seat' group is referenced by astrix-compositor.service and by
# usermod below, but is normally shipped by the seatd/systemd packages. When it
# is absent, systemd aborts the compositor with
#   status=216/GROUP  Failed to determine supplementary groups
# and no log line from the compositor itself, which makes it look like a
# compositor bug rather than a missing group. Create it explicitly so the
# session does not depend on which of those packages happened to be installed.
in_rootfs "$ROOTFS" groupadd -f -r seat >/dev/null 2>&1 || true
in_rootfs "$ROOTFS" usermod -aG audio,video,input,render,seat "$ASTRIX_USER" 2>/dev/null || true
# The shell's state directory. astrix-shell.service lists this exact path in
# ReadWritePaths, and systemd fails the unit at namespace setup if it does not
# exist, so it is created here rather than left to the first run.
in_rootfs "$ROOTFS" /bin/bash -c "
	set -e
	install -d -m 0700 -o '$ASTRIX_USER' -g '$ASTRIX_USER' \
		/home/$ASTRIX_USER/.local/share/astrix
	install -d -m 0700 -o '$ASTRIX_USER' -g '$ASTRIX_USER' \
		/home/$ASTRIX_USER/.config/astrix
"

if [ "$ASTRIX_ANDROID_ENABLED" = "true" ]; then
  if ! in_rootfs "$ROOTFS" getent group android >/dev/null 2>&1; then
    in_rootfs "$ROOTFS" groupadd -f -r android
  fi
fi

# ---------------------------------------------------------------------------
# 7. Filesystem permissions and hardening
# ---------------------------------------------------------------------------
log "Applying permissions and hardening"

# The graphical units carry a literal XDG_RUNTIME_DIR=/run/user/<uid>, because
# in a system unit Environment= is expanded in the *manager's* context and %U
# would silently resolve to 0 - the compositor then looked for its Wayland
# socket in /run/user/0 and failed with a permission error that named the
# wrong directory entirely.
#
# A literal is only safe while something checks it against the real uid, so
# check it here. config/os.conf is the single source of truth; a unit that
# disagrees is a build failure, not a boot failure.
for unit in astrix-compositor.service astrix-shell.service; do
  unit_path="$ROOTFS/usr/lib/astrix/services/$unit"
  [ -f "$unit_path" ] || continue
  if grep -q "^Environment=XDG_RUNTIME_DIR=/run/user/${ASTRIX_UID}\$" "$unit_path"; then
    ok "$unit: XDG_RUNTIME_DIR matches ASTRIX_UID=$ASTRIX_UID"
  else
    die "$unit does not set XDG_RUNTIME_DIR=/run/user/${ASTRIX_UID}; \
it must be a literal matching config/os.conf, not a %U specifier"
  fi
done

# The compositor runs with ProtectSystem=strict, which remounts / read-only -
# /run included. So XDG_RUNTIME_DIR is the one directory that must be
# re-opened for writing or the compositor can never bind its own Wayland
# socket. The failure looks unrelated to hardening:
#   unable to open lockfile /run/user/1000/astrix-0.lock check permissions
#   [ERROR] [astrix] failed to create wayland socket 'astrix-0'
# and it only shows up at runtime, after the DRM backend already came up.
#
# ProtectHome=yes is the same trap with a worse disguise: it hides /run/user
# behind an empty read-only mount, so the socket directory is not merely
# read-only, it is gone. ReadWritePaths cannot rescue a hidden directory.
comp_unit="$ROOTFS/usr/lib/astrix/services/astrix-compositor.service"
if [ -f "$comp_unit" ]; then
  if grep -qx "ProtectSystem=strict" "$comp_unit" \
     && grep -qx "ReadWritePaths=/run/user/${ASTRIX_UID}" "$comp_unit"; then
    ok "astrix-compositor: XDG_RUNTIME_DIR is writable under ProtectSystem=strict"
  else
    die "astrix-compositor.service must set ReadWritePaths=/run/user/${ASTRIX_UID} \
alongside ProtectSystem=strict, or it cannot create its Wayland socket"
  fi
  if grep -qx "ProtectHome=yes" "$comp_unit"; then
    die "astrix-compositor.service uses ProtectHome=yes, which hides /run/user - \
and the compositor's Wayland socket lives in /run/user/${ASTRIX_UID}. \
Use ProtectHome=read-only."
  fi
fi

# The shell/compositor must not be writable by the user: a compromised app
# must not be able to replace the compositor's own binaries.
chmod 0755 "$ROOTFS/usr/bin" "$ROOTFS/usr/lib" "$ROOTFS/usr/lib/astrix" 2>/dev/null || true
if [ -x "$ROOTFS/usr/bin/astrix-compositor" ]; then
  chown root:root "$ROOTFS/usr/bin/astrix-compositor"
  chmod 0755 "$ROOTFS/usr/bin/astrix-compositor"
fi
if [ -x "$ROOTFS/usr/bin/astrix-shell" ]; then
  chown root:root "$ROOTFS/usr/bin/astrix-shell"
  chmod 0755 "$ROOTFS/usr/bin/astrix-shell"
fi

# systemd: never drop to a shell on failure, and log to the journal.
mkdir -p "$ROOTFS/etc/systemd/system.conf.d"
cat > "$ROOTFS/etc/systemd/system.conf.d/astrix.conf" <<'EOF'
[Manager]
DefaultTimeoutStartSec=120s
# NOTE: StartLimitIntervalSec/StartLimitBurst are deliberately NOT set here.
# They are [Unit] directives, not [Manager] ones; systemd 257 logs
# "Unknown key ... in section [Manager], ignoring" for them and the limit
# silently never applies. The per-unit limits live in the units themselves.
EOF

# ---------------------------------------------------------------------------
# 8. Clean up
# ---------------------------------------------------------------------------
log "Cleaning package cache"
in_rootfs "$ROOTFS" apt-get clean
rm -rf "$ROOTFS/var/lib/apt/lists/"*
rm -f "$ROOTFS/var/log"/*.log 2>/dev/null || true
# Machine-id must be generated on first boot, not baked into the image.
rm -f "$ROOTFS/etc/machine-id" "$ROOTFS/var/lib/dbus/machine-id" 2>/dev/null || true

umount_rootfs "$ROOTFS"
trap - EXIT

ok "rootfs ready: $ROOTFS ($(human_size "$ROOTFS"))"
dim "next: ./build-image.sh (needs boot/vmlinuz)"
