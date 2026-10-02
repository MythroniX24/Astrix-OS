#!/usr/bin/env bash
# Astrix OS - assemble a bootable ARM64 disk image.
#
# Produces a GPT-partitioned image with:
#   p1  ESP   - EFI System Partition (systemd-boot / U-Boot on real hardware)
#   p2  root  - the Debian userspace built by build-rootfs.sh
#
# For QEMU the image is booted with a direct kernel command line, so the ESP is
# optional there; it is created anyway so the same image is flashable to real
# hardware without being rebuilt.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

ROOTFS="${ASTRIX_BUILD_DIR}/rootfs"
OUT="${ASTRIX_BUILD_DIR}/out"
IMG="${ASTRIX_BUILD_DIR}/${OS_NAME}.img"
MOUNT="${ASTRIX_BUILD_DIR}/mnt"
ESP_STAGE="${ASTRIX_BUILD_DIR}/esp-stage"

# Image sizing. A phone OS must fit a phone, but the Debian userspace with the
# full dev toolchain is larger; 4G is a development default.
IMG_SIZE_MB="${ASTRIX_IMAGE_SIZE_MB:-4096}"
ESP_SIZE_MB=128
NO_KERNEL=0
SKIP_FSTAB=0

while [ $# -gt 0 ]; do
  case "$1" in
    --no-kernel) NO_KERNEL=1; shift ;;
    --size-mb) IMG_SIZE_MB="$2"; shift 2 ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
[ -d "$ROOTFS" ] || die "no rootfs at $ROOTFS - run ./scripts/build-rootfs.sh first"
[ "$(id -u)" -eq 0 ] || die "must run as root (loopback mounts and mkfs)"

for t in sgdisk mkfs.ext4 mkfs.vfat parted losetup; do
  command -v "$t" >/dev/null || die "$t not found (apt-get install gdisk dosfstools parted util-linux)"
done
# mtools is the fallback FAT writer, needed whenever the host kernel has no FAT
# driver. Without it the ESP can only be written where FAT is mountable.
command -v mcopy >/dev/null || warn "mtools not installed; the ESP can only be written on a host with FAT support"

# Kernel: prefer the in-tree build, fall back to the distro kernel from the
# rootfs (which is what makes the first milestone bootable).
KERNEL_IMG=""
INITRD_IMG=""
if [ "$NO_KERNEL" -eq 0 ]; then
  if [ -f "${OUT}/boot/Image" ]; then
    KERNEL_IMG="${OUT}/boot/Image"
    dim "using the in-tree Astrix kernel"
  else
    # Kernel and initramfs are resolved as a *pair*, never independently.
    #
    # Picking each with its own `find ... | head -1` looks harmless and is not:
    # /boot accumulates kernels (apt upgrades linux-image-arm64, an in-tree
    # build drops a new one beside the old), and two independent globs will
    # happily pair vmlinuz-6.12.107 with initrd.img-6.12.111. The guest then
    # loads an initramfs full of modules for a kernel that is not running, so
    # virtio_blk never loads, /dev/vda never appears, and the boot dies with
    # "ALERT! /dev/vda2 does not exist". That is a confusing, unreproducible-
    # looking failure for a plain build ordering mistake, and it happened here.
    #
    # So: choose one kernel, then require the initramfs that belongs to that
    # exact version, and fail the build if it is not there.
    for candidate in $(find "$ROOTFS/boot" -name 'vmlinuz-*' 2>/dev/null | sort); do
      suffix="${candidate##*/vmlinuz-}"
      if [ -f "$ROOTFS/boot/initrd.img-${suffix}" ]; then
        KERNEL_IMG="$candidate"
        INITRD_IMG="$ROOTFS/boot/initrd.img-${suffix}"
        break
      fi
      warn "ignoring ${candidate##*/}: no matching initrd.img-${suffix}"
    done
    if [ -n "$KERNEL_IMG" ]; then
      dim "using the distro kernel ${KERNEL_IMG##*/} (run build-kernel.sh for the in-tree one)"
    fi
  fi
  [ -n "$KERNEL_IMG" ] || die "no kernel with a matching initramfs found in $ROOTFS/boot; \
run build-kernel.sh or keep linux-image-arm64 in the rootfs"
  if [ -n "$INITRD_IMG" ] && [ ! -f "$INITRD_IMG" ]; then
    die "initramfs $INITRD_IMG does not exist; the image would boot a kernel \
whose modules it cannot load"
  fi
fi

# ---------------------------------------------------------------------------
# Copy the userspace
# ---------------------------------------------------------------------------
log "Copying rootfs ($(human_size "$ROOTFS"))"
STAGE="${ASTRIX_BUILD_DIR}/stage-root"
rm -rf "$STAGE"
mkdir -p "$STAGE"
# -a preserves device nodes, ownership and permissions. Using tar as the
# transport avoids cp complaining about /dev and /proc inside the source.
tar -C "$ROOTFS" -cf - . | tar -C "$STAGE" -xf -

# The stage must not carry the host's mounts or a stale machine-id.
umount -R "$STAGE" 2>/dev/null || true
rm -f "$STAGE/etc/machine-id" "$STAGE/var/lib/dbus/machine-id"
mkdir -p "$STAGE/proc" "$STAGE/sys" "$STAGE/dev" "$STAGE/run" "$STAGE/tmp" "$STAGE/boot"
chmod 1777 "$STAGE/tmp"
mkdir -p "$STAGE/dev/pts" "$STAGE/dev/shm"
chmod 1777 "$STAGE/dev/shm"

# Point fstab at the real root device (p2) unless the caller manages it.
if [ "$SKIP_FSTAB" -eq 0 ] && [ ! -f "$STAGE/etc/fstab.astrix" ]; then
  sed -i 's|^/dev/root .*|/dev/vda2            /               ext4    defaults,noatime        0      1|' \
    "$STAGE/etc/fstab" 2>/dev/null || true
fi

# The GUI binaries need no separate install step: build-gui.sh compiles them
# directly into the rootfs at /usr/bin, so the tar copy above has already
# carried them into the stage. What still has to be installed is the session
# launcher, which is a script in the source tree rather than a build product.
log "Installing the session launcher"
install -D -m 0755 "${ASTRIX_REPO_ROOT}/system/astrix-session" "$STAGE/usr/bin/astrix-session"

# Enable the graphical session at boot.
#
# From basic.target, not multi-user.target: multi-user.target is ordered after
# network.target, so enabling the session there makes the phone's home screen
# wait for the network manager. See astrix-session.service for the measurement.
mkdir -p "$STAGE/etc/systemd/system/basic.target.wants"
ln -sf ../astrix-session.service "$STAGE/etc/systemd/system/basic.target.wants/astrix-session.service"
# The boot report is what makes a headless boot diagnosable: without it a
# failing graphical session is completely silent, because a phone whose shell
# did not start has nothing on screen to read an error from. Same target as the
# session itself, so it is not waiting on the network either.
ln -sf ../astrix-boot-report.service "$STAGE/etc/systemd/system/basic.target.wants/astrix-boot-report.service"
# Units that live in /usr/lib must be linked from /etc.
mkdir -p "$STAGE/etc/systemd/system"
if [ -d "$STAGE/usr/lib/astrix/services" ]; then
  cp -a "$STAGE/usr/lib/astrix/services/." "$STAGE/etc/systemd/system/" 2>/dev/null || true
fi

# Do not block the boot on the network.
#
# Both *-wait-online units are ordered *before* multi-user.target and hold it
# open until a network is up. On a phone that is simply wrong: the screen must
# come on regardless of whether there is a network, and on a QEMU virtio NIC
# that has no carrier it added over ten minutes to every boot - a boot-time
# regression that is invisible on a host with a working connection and is the
# difference between "the compositor starts" and "the compositor is still
# starting" on the machine that has to test it.
#
# Masked rather than disabled, so the intent is recorded and `systemctl status`
# says why instead of looking like a missing unit.
log "Masking the wait-online units (a phone must boot without a network)"
for u in NetworkManager-wait-online.service systemd-networkd-wait-online.service; do
  mkdir -p "$STAGE/etc/systemd/system/$u.d"
  ln -sf /dev/null "$STAGE/etc/systemd/system/$u"
done

# Likewise systemd-networkd: NetworkManager is the network manager on this
# system, and two of them configuring the same interface is a race, not a
# redundancy. Debian can enable networkd alongside NM, so it is masked here to
# make the choice explicit.
log "Masking systemd-networkd (NetworkManager is the network manager)"
mkdir -p "$STAGE/etc/systemd/system/systemd-networkd.service.d"
ln -sf /dev/null "$STAGE/etc/systemd/system/systemd-networkd.service"
# ...but its socket unit activates it on demand, so that has to go too.
mkdir -p "$STAGE/etc/systemd/system/systemd-networkd.socket.d"
ln -sf /dev/null "$STAGE/etc/systemd/system/systemd-networkd.socket"

# Same reasoning for the filesystem scrubber: e2scrub_reap walks the whole
# root filesystem on a timer, on a device whose flash does not benefit from it
# and whose battery cannot afford it. It was measured at over four minutes of
# a QEMU boot for no benefit whatsoever.
log "Masking e2scrub (a phone does not need a periodic filesystem scrub)"
for u in e2scrub_reap.service e2scrub_all.service e2scrub_all.timer e2scrub_reap.timer; do
  mkdir -p "$STAGE/etc/systemd/system/$u.d"
  ln -sf /dev/null "$STAGE/etc/systemd/system/$u"
done

# ---------------------------------------------------------------------------
# Build the image
# ---------------------------------------------------------------------------
log "Creating ${IMG_SIZE_MB}MB image"
rm -f "$IMG"
truncate -s "${IMG_SIZE_MB}M" "$IMG"

LOOP="$(losetup --show -f -P "$IMG")"
cleanup() {
  sync 2>/dev/null || true
  umount "$MOUNT" 2>/dev/null || true
  losetup -d "$LOOP" 2>/dev/null || true
}
trap cleanup EXIT
ok "loop device: $LOOP"

# The tar copy of the rootfs picks up whatever the test suite left in /tmp.
# A stray Wayland socket from a previous smoke test must not end up in the
# shipped filesystem.
rm -rf "${STAGE:?}/tmp/astrix-smoke" "${STAGE:?}/tmp/astrix-smoke-logs"
rm -f "$STAGE/tmp/astrix-wayland-"* 2>/dev/null || true

# Partition layout: 1MiB alignment, which is what U-Boot and systemd-boot
# both prefer.
sgdisk --clear \
  --new=1:2048:+${ESP_SIZE_MB}MB --typecode=1:EF00 --change-name=1:"ESP" \
  --new=2:0:0 --typecode=2:8300 --change-name=2:"astrix-root" \
  "$LOOP" >/dev/null
partprobe "$LOOP" 2>/dev/null || true
udevadm settle 2>/dev/null || sleep 1
ok "partition table written"

# EFI System Partition
ESP_PART="${LOOP}p1"
ROOT_PART="${LOOP}p2"
[ -b "$ESP_PART" ] || ESP_PART="$LOOP"1
[ -b "$ROOT_PART" ] || ROOT_PART="$LOOP"2

if [ ! -b "$ESP_PART" ] || [ ! -b "$ROOT_PART" ]; then
  die "partitions did not appear as expected ($ESP_PART / $ROOT_PART)"
fi

log "Creating the ESP"
mkfs.vfat -F 32 -n ASTRIX_EFI "$ESP_PART" >/dev/null

# Build the ESP contents in a staging directory first, then get them onto the
# partition. Two transports are tried, in order:
#
#   1. mount -t vfat. Normal, but needs the host kernel to have the FAT driver
#      compiled in or loaded. Containers frequently do not, and there is often
#      no modprobe to load it, so this can fail for environmental reasons that
#      have nothing to do with the image.
#   2. mtools, which reads and writes FAT in userspace and needs no kernel
#      support at all.
#
# The staging directory means both paths write identical content, so the
# result does not depend on which one the host happens to allow.
mkdir -p "$ESP_STAGE"
rm -rf "${ESP_STAGE:?}"/*
mkdir -p "$ESP_STAGE/EFI/BOOT" "$ESP_STAGE/loader/entries" "$ESP_STAGE/boot"
# systemd-boot: the U-Boot-free path to booting on real ARM64 hardware.
if [ -f "$ROOTFS/usr/lib/systemd/boot/efi/systemd-bootx64.efi" ]; then
  cp "$ROOTFS/usr/lib/systemd/boot/efi/systemd-bootx64.efi" "$ESP_STAGE/EFI/BOOT/BOOTX64.EFI" 2>/dev/null || true
fi
# A minimal loader entry; the kernel path is filled in for the QEMU milestone.
if [ "$NO_KERNEL" -eq 0 ] && [ -n "$KERNEL_IMG" ]; then
  KREL="$(basename "$(dirname "$KERNEL_IMG")")/$(basename "$KERNEL_IMG")"
  cat > "$ESP_STAGE/loader/entries/astrix.conf" <<EOF
title Astrix OS
version ${OS_VERSION}
linux /$KREL
initrd /${KREL%/*}/initrd.img-$(basename "${INITRD_IMG:-initrd.img}" | sed 's/^initrd.img-//')
options console=tty0 console=ttyAMA0 root=PARTUUID=$(sgdisk -i 2 "$LOOP" | awk '/Partition unique GUID/{print $5}') rootwait rw quiet
EOF
  cp "$KERNEL_IMG" "$ESP_STAGE/boot/" 2>/dev/null || true
  [ -n "$INITRD_IMG" ] && cp "$INITRD_IMG" "$ESP_STAGE/boot/" 2>/dev/null || true
fi

esp_transport=""
mkdir -p "$MOUNT"
if mount -t vfat "$ESP_PART" "$MOUNT" 2>/dev/null; then
  cp -a "$ESP_STAGE/." "$MOUNT/"
  umount "$MOUNT"
  esp_transport="mount"
else
  warn "kernel FAT mount unavailable; writing the ESP with mtools instead"
  # mtools addresses a filesystem by drive letter, and it cannot derive one
  # from a loop partition name, so the drive is defined explicitly. Writing the
  # config to a file and pointing MTOOLSRC at it is more predictable than
  # relying on the host's /etc/mtools.conf.
  MT_CFG="${ASTRIX_BUILD_DIR}/mtools.conf"
  cat > "$MT_CFG" <<EOF
drive z: file="$ESP_PART"
mtools_skip_check=1
EOF
  export MTOOLSRC="$MT_CFG"
  # Targets use the "z:/path" form rather than "::/path": the latter is only
  # valid without -i, and mixing the two makes mtools report
  # "Can't open z::" rather than anything that points at the real cause.
  for d in z:/EFI z:/EFI/BOOT z:/loader z:/loader/entries z:/boot; do
    mmd -i z: "$d" 2>/dev/null || true
  done
  ( cd "$ESP_STAGE" && find . -type f -print0 | while IFS= read -r -d '' f; do
      mcopy -i z: -o -Q "$f" "z:/${f#./}"
    done )
  esp_transport="mtools"
fi

# Verify the ESP really got its loader entry. An ESP that was created and
# formatted but silently left empty produces an image that boots on QEMU
# (which uses a direct kernel command line) and then fails on real hardware,
# which is the worst time to find out.
if mdir -i z: z:/loader/entries 2>/dev/null | grep -q astrix.conf; then
  ok "ESP populated via $esp_transport (loader entry present)"
else
  die "ESP has no loader entry after populating it via $esp_transport"
fi

log "Creating the root filesystem"
# -O ^has_journal would be faster but a phone filesystem needs a journal: an
# unclean shutdown after a battery pull is normal, not exceptional.
mkfs.ext4 -F -L astrix-root -m 0 -E lazy_itable_init=1,lazy_journal_init=1 \
  -d "$STAGE" "$ROOT_PART" >/dev/null 2>&1 ||
  mkfs.ext4 -F -L astrix-root -d "$STAGE" "$ROOT_PART" >/dev/null
ok "root filesystem written"

# Record the image's layout for run-qemu.sh and for flashing.
cat > "${ASTRIX_BUILD_DIR}/image-info.env" <<EOF
ASTRIX_IMAGE="$IMG"
ASTRIX_LOOP_DEV="$LOOP"
ASTRIX_ROOT_PARTUUID="$(sgdisk -i 2 "$LOOP" | awk '/Partition unique GUID/{print $5}')"
ASTRIX_KERNEL="$KERNEL_IMG"
ASTRIX_INITRD="$INITRD_IMG"
EOF

# Verify the image really contains an OS, rather than trusting mkfs.
log "Verifying image contents"
if mount -o ro "$ROOT_PART" "$MOUNT" 2>/dev/null; then
  # Check PID 1 by following /sbin/init, not by looking for a fixed path.
  # Debian trixie uses a merged /usr, so systemd lives at /lib/systemd/systemd
  # with /lib a symlink to /usr/lib, and /sbin/init is a symlink to it. There is
  # no /usr/bin/systemd to look for, and asserting one made a correct image
  # look broken.
  [ -e "$MOUNT/sbin/init" ] || die "image has no /sbin/init - the rootfs is incomplete"
  [ -x "$MOUNT/sbin/init" ] || die "image /sbin/init is not executable (does not resolve to systemd)"
  [ -f "$MOUNT/etc/os-release" ] || die "image has no /etc/os-release"
  grep -q "Astrix" "$MOUNT/etc/os-release" || warn "os-release does not identify as Astrix"
  # Assert the GUI stack really landed, rather than trusting that the earlier
  # steps happened to run. A bootable image that boots to a black screen
  # because the compositor is missing is exactly the failure worth catching
  # here.
  for b in astrix-compositor astrix-shell astrix-session; do
    [ -x "$MOUNT/usr/bin/$b" ] || die "image is missing /usr/bin/$b"
  done
  ok "verified: init resolves to systemd, os-release identifies Astrix, GUI stack present"
  umount "$MOUNT"
else
  warn "could not mount the image back for verification"
fi

trap - EXIT
cleanup

ok "image: $IMG ($(du -h "$IMG" | cut -f1))"
dim "next: ./run-qemu.sh"
