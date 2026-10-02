#!/usr/bin/env bash
# Astrix OS - boot the image in QEMU (ARM64).
#
# Development display only. The guest renders its real framebuffer and QEMU
# exposes it over VNC; this is a development convenience and is not part of how
# Astrix works on hardware. The serial console is logged alongside so the boot
# can be verified without a VNC client.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# shellcheck source=../scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"
trap on_error ERR

astrix_init_paths

IMG="${ASTRIX_BUILD_DIR}/${OS_NAME}.img"
INFO="${ASTRIX_BUILD_DIR}/image-info.env"
LOG="${ASTRIX_BUILD_DIR}/qemu-serial.log"
MEM_MB="${ASTRIX_MEM_MB:-2048}"
VNC_DISPLAY="${ASTRIX_VNC_DISPLAY:-1}"
CPUS="${QEMU_CPUS:-2}"
HEADLESS=0
DAYS=0
WIDTH=0
HEIGHT=0
HEADLESS_DISK=""

while [ $# -gt 0 ]; do
  case "$1" in
    --headless) HEADLESS=1; shift ;;
    --disk) HEADLESS_DISK="$2"; shift 2 ;;
    --mem) MEM_MB="$2"; shift 2 ;;
    --vnc) VNC_DISPLAY="$2"; shift 2 ;;
    --size) WIDTH="$2"; HEIGHT="$3"; shift 3 ;;
    --days) DAYS=1; shift ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
require_cmd "$QEMU_SYSTEM" || die "$QEMU_SYSTEM not found (apt-get install qemu-system-arm)"
[ -f "$IMG" ] || die "no image at $IMG - run ./build.sh first"
[ -f "$INFO" ] && # shellcheck disable=SC1090
  source "$INFO"

KERNEL_PATH="${ASTRIX_KERNEL:-}"
INITRD_PATH="${ASTRIX_INITRD:-}"

log "Booting ${OS_PRETTY_NAME} in QEMU (${DEB_TRIPLET})"
dim "image:   $IMG ($(du -h "$IMG" 2>/dev/null | cut -f1))"
dim "memory:  ${MEM_MB}MB"
dim "cpus:    ${CPUS}"
[ -n "$KERNEL_PATH" ] && dim "kernel:  $KERNEL_PATH"
dim "serial:  $LOG"

# ---------------------------------------------------------------------------
# Machine configuration
# ---------------------------------------------------------------------------
# The 'virt' machine is the closest QEMU comes to a generic ARM64 phone SoC:
# it gives us a real ARM CPU, a virtio-gpu with a KMS-capable DRM device, virtio
# input devices (which is where the touchscreen comes from in this milestone),
# and a PSCI-compatible power interface.
MACHINE_ARGS=(
  -machine virt
  -cpu "$QEMU_CPU"
  -smp "$CPUS"
  -m "$MEM_MB"
)

# virtio-gpu gives a DRM/KMS device, which is what the Wayland stack needs.
# There is no accelerated GPU under TCG, so the guest uses Mesa's software
# rasteriser via WLR_RENDERER=pixman.
GPU_ARGS=(-device virtio-gpu-pci)

# virtio-input-device is an *abstract* device: it only describes the transport
# and QEMU rejects it on the command line ("expects a non-abstract device
# type"). The concrete devices below are the ones that actually attach.
#
# Input devices.
#
# The pointing device is virtio-mouse, and that is a measured limitation, not
# a preference. virtio-tablet-device is available in this QEMU and is attached
# as virtio-pci 0000:00:03.0, but the Debian arm64 guest never binds an evdev
# node to it: the kernel's input: log shows only "gpio-keys" (input0) and
# "QEMU Virtio Keyboard" (input1), and no tablet node appears at any point in
# the boot. With the tablet alone the seat ends up with two keyboards and no
# pointer at all. This QEMU build also ships no usb-tablet, so there is no
# absolute pointing device to fall back to.
#
# What that costs is stated plainly: the tablet/touch path is therefore NOT
# verified. virtio-mouse gives real relative pointer events, which exercise
# evdev -> libinput -> the compositor's forwarding -> the shell's gesture
# recogniser -> navigation, and that whole chain is verified. Multi-touch,
# true absolute tap positions and edge-swipe geometry are not.
#
# Only one pointing device is attached on purpose. QEMU delivers an injected
# QMP event to one input device, and with a mouse and a tablet both present the
# mouse wins, so absolute events are silently discarded and a scripted swipe
# does nothing at all.
INPUT_ARGS=(
  -device virtio-keyboard
  -device virtio-mouse
)
dim "pointer: virtio-mouse (relative; see the note above on virtio-tablet)"

# Storage: attach the image directly. Direct boot avoids needing firmware.
STORAGE_ARGS=()
if [ -n "$HEADLESS_DISK" ]; then
  STORAGE_ARGS=(-drive file="$HEADLESS_DISK",if=none,id=hd0,format=raw -device virtio-blk-device,drive=hd0)
else
  STORAGE_ARGS=(-drive file="$IMG",if=none,id=hd0,format=raw,cache=unsafe -device virtio-blk-device,drive=hd0)
fi

# Networking: user-mode networking gives outbound access for apt/git without
# exposing anything to the host network.
#
# The forwarded SSH port is configurable and checked up front. A fixed port
# makes QEMU fail to start whenever anything else on the host already holds it,
# and the resulting "Could not set up host forwarding rule" is easy to
# misread as a QEMU or image problem.
SSH_FWD_PORT="${ASTRIX_SSH_PORT:-2222}"
# Ask ss about the port directly rather than grepping its output. A grep for
# ":2222$" never matches, because an ss line ends with the peer address
# (0.0.0.0:*), not the local port - so the check silently passed and the real
# failure surfaced as QEMU's much less obvious "Could not set up host
# forwarding rule".
if ss -ltnH "sport = :${SSH_FWD_PORT}" 2>/dev/null | grep -q .; then
  die "host port ${SSH_FWD_PORT} is already in use; set ASTRIX_SSH_PORT to a free port"
fi
NET_ARGS=(-netdev "user,id=net0,hostfwd=tcp::${SSH_FWD_PORT}-:22" -device virtio-net-device,netdev=net0)
dim "ssh:     host 127.0.0.1:${SSH_FWD_PORT} -> guest :22"

# Serial: always log. This is how the boot is verified headlessly.
#
# A QMP socket is exposed alongside it so the *picture* can be checked, not
# just the log. "the shell mapped a surface" and "the home screen is on the
# display" are different claims, and only screendump tells us which one is
# true. Point ASTRIX_QMP_SOCK elsewhere if the default is taken.
QMP_SOCK="${ASTRIX_QMP_SOCK:-${ASTRIX_BUILD_DIR}/qmp.sock}"
rm -f "$QMP_SOCK"
SERIAL_ARGS=(-serial file:"$LOG" -monitor none -qmp "unix:$QMP_SOCK,server=on,wait=off")
dim "qmp:     $QMP_SOCK (screendump /system/screen.ppm)"

DISPLAY_ARGS=()
if [ "$HEADLESS" -eq 1 ]; then
  DISPLAY_ARGS=(-display none -vnc none)
  dim "display: none (headless)"
else
  DISPLAY_ARGS=(-display none -vnc ":${VNC_DISPLAY}")
  dim "display: VNC on :${VNC_DISPLAY} (connect a VNC client to 127.0.0.1:590$((VNC_DISPLAY-1)))"
fi

CMDLINE="root=/dev/vda2 rw rootwait console=tty0 console=ttyAMA0,115200 earlycon=pl011,0x9000000 panic=-1"

DIRECT_BOOT_ARGS=()
if [ -n "$KERNEL_PATH" ] && [ -f "$KERNEL_PATH" ]; then
  DIRECT_BOOT_ARGS=(-kernel "$KERNEL_PATH")
  [ -n "$INITRD_PATH" ] && [ -f "$INITRD_PATH" ] && DIRECT_BOOT_ARGS+=(-initrd "$INITRD_PATH")
  DIRECT_BOOT_ARGS+=(-append "$CMDLINE")
  dim "boot:    direct kernel (no firmware needed)"
else
  # Without a kernel we rely on firmware/bootloader inside the image.
  dim "boot:    image boot path"
fi

EXTRA_ARGS=()
if [ "$WIDTH" -gt 0 ] && [ "$HEIGHT" -gt 0 ]; then
  # The shell reads ASTRIX_WIDTH/HEIGHT; also constrain the display so the
  # guest's panel matches the requested phone resolution.
  EXTRA_ARGS+=(-device virtio-gpu-pci,xres="$WIDTH",yres="$HEIGHT")
fi

CMD=(
  "$QEMU_SYSTEM"
  "${MACHINE_ARGS[@]}"
  "${GPU_ARGS[@]}"
  "${INPUT_ARGS[@]}"
  "${STORAGE_ARGS[@]}"
  "${NET_ARGS[@]}"
  "${SERIAL_ARGS[@]}"
  "${DISPLAY_ARGS[@]}"
  "${DIRECT_BOOT_ARGS[@]}"
  "${EXTRA_ARGS[@]}"
  -device virtio-rng-pci
  -no-reboot
)

if [ "$DAYS" -eq 1 ]; then
  CMD=("$QEMU_SYSTEM" "${MACHINE_ARGS[@]}" -bios "$(command -v qemu-efi-aarch64 || echo /usr/share/AAVMF/AAVMF_CODE.fd)" \
       "${GPU_ARGS[@]}" "${INPUT_ARGS[@]}" "${STORAGE_ARGS[@]}" "${NET_ARGS[@]}" "${SERIAL_ARGS[@]}" \
       "${DISPLAY_ARGS[@]}" -no-reboot)
fi

log "starting QEMU (ctrl-a x to quit)"
"${CMD[@]}" || warn "QEMU exited with status $?"

ok "QEMU exited; serial log: $LOG"
