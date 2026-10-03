#!/usr/bin/env bash
# Astrix OS - get a real Astrix onto a Redmi 8A (codename "olive").
#
#   ./scripts/port-redmi-8a.sh status              what is real, and what is not
#   ./scripts/port-redmi-8a.sh android-host pack   build the slim userspace bundle
#   ./scripts/port-redmi-8a.sh android-host check  preflight the phone over adb
#   ./scripts/port-redmi-8a.sh android-host push   copy the bundle onto the phone
#   ./scripts/port-redmi-8a.sh android-host start  launch it (prints the command)
#   ./scripts/port-redmi-8a.sh android-host stop   stop it
#   ./scripts/port-redmi-8a.sh boot-test build     build a fastboot boot-test image
#   ./scripts/port-redmi-8a.sh boot-test stage     flash it to the phone's boot
#   ./scripts/port-redmi-8a.sh boot-test log       capture the USB serial proof
#   ./scripts/port-redmi-8a.sh boot-test rollback  how to get Android back
#
# ---------------------------------------------------------------------------
# THE ONE THING TO UNDERSTAND BEFORE USING THIS
# ---------------------------------------------------------------------------
# "Astrix boots on the Redmi 8A" is currently FALSE, and no script here can
# make it true. The GPU is an Adreno 505 and mainline Linux has no driver for
# it at all - not a stub, not an out-of-tree patch, none. There is also no
# upstream device tree for olive. So an Astrix kernel cannot light this screen
# even with the bootloader unlocked and a perfect .config.
#
# That is a missing-driver fact, not a missing-configuration fact, and it is
# why this script has two very different routes instead of pretending there is
# one. They are named for what they actually prove, because the difference
# matters the moment something goes wrong on a phone you paid for.
#
#   android-host  Astrix USERSPACE, on the phone's own Android kernel.
#                 No unlock, no flash, no drivers, nothing destroyed. The
#                 compositor, the shell, the apps and the modern UI - the parts
#                 people actually want to see - all of it, on the real device.
#                 PROVES: the GUI works on this SoC and this panel.
#                 NOT PROVES: anything about Astrix's kernel. Android is still
#                 in charge below it. This is running Astrix, not booting it.
#
#   boot-test     Astrix's KERNEL, flashed to the phone's boot partition,
#                 booted, and verified over the USB serial console.
#                 PROVES: Astrix's kernel starts on olive and reaches
#                 userspace. That is a real, permanent result.
#                 NOT PROVES: a picture. The screen stays dark for the entire
#                 boot - there is no display driver to draw with. Android is
#                 replaced until you flash a stock image back.
#
# So: want to SEE Astrix? android-host. Want to PROVE the kernel boots?
# boot-test. Want both? Run android-host first, and only then decide whether
# the boot test is worth what it costs.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

DEVICE="redmi-8a"
DEVICE_DIR="${ASTRIX_DEVICE_DIR:-${ASTRIX_REPO_ROOT}/config/devices}"
PROFILE="${DEVICE_DIR}/${DEVICE}.conf"
[ -f "$PROFILE" ] || die "no device profile at ${PROFILE}"
# shellcheck source=/dev/null
source "$PROFILE"

ROOTFS="${ASTRIX_BUILD_DIR}/rootfs"
OUT="${ASTRIX_BUILD_DIR}/device/${DEVICE}"
BUNDLE="${OUT}/android-host-rootfs.tar.zst"
MOUNT_DIR="${OUT}/android-host/rootfs"
LAUNCHER_REL="opt/astrix/astrix-android-host.sh"
ADB_SERIAL="${ASTRIX_ADB_SERIAL:-}"

# ---------------------------------------------------------------------------
usage() {
  # Print everything between the shebang and the first real line of code, so
  # the header cannot drift out of sync with the text printed here.
  sed -n '2,/^set -euo/p' "$0" | sed '$d'
  cat <<EOF

Android-host route (no unlock, no flash, nothing destroyed):
  pack    build ${BUNDLE}
  check   preflight the connected phone
  push    copy the bundle to /sdcard/Download/astrix
  start   print (and try to run) the launcher
  stop    stop the session

boot-test route (needs an unlocked bootloader; replaces Android's kernel):
  build   build the boot-test image and ramdisk
  stage   fastboot flash boot
  log    capture the USB serial console
  rollback  print how to put Android back

Override:
  ASTRIX_ALLOW_UNVERIFIED_FLASH=1   required by \`boot-test stage\`
  ASTRIX_ADB_SERIAL=<serial>        target one specific phone
EOF
}

# ---------------------------------------------------------------------------
die_here() { err "$*"; exit 1; }

need_adb() {
  command -v adb >/dev/null || die "adb not found (apt-get install adb)"
}
need_fastboot() {
  command -v fastboot >/dev/null || die "fastboot not found (apt-get install fastboot)"
}

adb_() {
  if [ -n "$ADB_SERIAL" ]; then adb -s "$ADB_SERIAL" "$@"; else adb "$@"; fi
}

# Print the phone's serial, refusing to guess when more than one is plugged in.
adb_device() {
  local list n
  list="$(adb devices | awk 'NR>1 && $2=="device" {print $1}')" || true
  n="$(printf '%s' "$list" | grep -c . || true)"
  case "$n" in
    0) die "no phone in adb mode. Plug it in and allow USB debugging." ;;
    1) printf '%s' "$list" ;;
    *) printf '%s\n' "$list" | sed 's/^/  /'
       die "more than one phone is connected; set ASTRIX_ADB_SERIAL=<serial>" ;;
  esac
}

# ---------------------------------------------------------------------------
# status
# ---------------------------------------------------------------------------
cmd_status() {
  printf '\n  %s (%s) - %s\n' "${DEVICE_NAME}" "${DEVICE_CODENAME}" "${DEVICE_SOC}"
  printf '  Panel: %sx%s @ %sHz   GPU: %s (mainline driver: %s)\n\n' \
    "${DEVICE_PANEL_WIDTH}" "${DEVICE_PANEL_HEIGHT}" "${DEVICE_PANEL_REFRESH}" \
    "${DEVICE_GPU}" "${DEVICE_GPU_DRIVER_MAINLINE}"

  # Printed, not err(): this is a report, and routing the headline through
  # stderr puts it ahead of the body when both are in a terminal.
  printf '  Astrix OS does not boot on this phone, and no .config changes that:\n'
  printf '%s\n' "${DEVICE_BOOT_BLOCKERS}" | tr ',' '\n' | sed 's/^/    - /'
  printf '\n'

  ok "android-host  run the Astrix GUI on the phone's own Android kernel"
  printf '%s\n' "${DEVICE_GUI_PORT_STEPS}" | tr ',' '\n' | sed 's/^/      /'
  dim "proves the compositor, shell and apps work on this hardware"
  dim "does not prove anything about Astrix's kernel - Android is still the kernel"
  printf '\n'

  ok "boot-test     flash Astrix's kernel and read the proof off USB serial"
  printf '%s\n' "${DEVICE_BOOT_TEST_STEPS}" | tr ',' '\n' | sed 's/^/      /'
  dim "proves the kernel boots and reaches userspace on olive"
  dim "the screen stays dark for the whole boot: no Adreno 505 mainline driver"
  dim "console: ${DEVICE_BOOT_TEST_CONSOLE}; Android has to be reflashed afterwards"
  printf '\n'
  dim "docs/REDMI-8A.md has the full write-up, including what has and has not"
  dim "actually been run against a real olive."
  printf '\n'
}

# ---------------------------------------------------------------------------
# android-host pack
# ---------------------------------------------------------------------------
# The full rootfs is 3.2 GB of Debian packages, which is not something you want
# to push down a phone's USB. What Astrix actually runs is eight ELF binaries,
# the scripts behind them, and the libraries those link against. So the bundle
# is the dependency closure of the Astrix binaries and nothing else, resolved
# by reading each ELF's NEEDED table rather than by guessing from ldd (which
# cannot run foreign-architecture binaries on an x86 host).
elf_needed() {
  readelf -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'
}

elf_interp() {
  readelf -l "$1" 2>/dev/null |
    sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p' | tr -d ' '
}

# Directories a glibc ARM64 loader searches, in the order it searches them.
LIB_DIRS="lib/aarch64-linux-gnu usr/lib/aarch64-linux-gnu lib lib64 usr/lib usr/local/lib"

resolve_lib() {
  local name="$1" d
  for d in $LIB_DIRS; do
    if [ -e "${ROOTFS}/${d}/${name}" ]; then printf '%s' "${d}/${name}"; return 0; fi
  done
  # Some libraries only exist with a versioned SONAME next to a symlink.
  for d in $LIB_DIRS; do
    if [ -d "${ROOTFS}/${d}" ] && ls "${ROOTFS}/${d}/${name}"* >/dev/null 2>&1; then
      printf '%s' "${d}/${name}"
      return 0
    fi
  done
  return 1
}

cmd_android_host_pack() {
  [ -x "${ROOTFS}/usr/bin/astrix-compositor" ] ||
    die "no Astrix build in ${ROOTFS}. Run ./build.sh first."
  command -v readelf >/dev/null || die "readelf not found (apt-get install binutils)"
  command -v zstd   >/dev/null || die "zstd not found (apt-get install zstd)"

  printf '\n'
  log "Building the slim Astrix bundle for ${DEVICE_NAME}"
  rm -rf "${MOUNT_DIR}"
  mkdir -p "$MOUNT_DIR"

  # 1. the programs, plus everything they need
  local seeds=()
  local b
  for b in "${ROOTFS}"/usr/bin/astrix-*; do
    [ -f "$b" ] && seeds+=("$b")
  done
  # dash is here because /bin/sh is a symlink to it: seeding the symlink
  # without the file it points at produces a bundle whose very first exec
  # fails, which reads like a broken proot rather than a missing shell.
  for b in "${ROOTFS}"/usr/lib/astrix/astrix-session \
           "${ROOTFS}"/usr/lib/astrix/astrix-boot-report \
           "${ROOTFS}"/bin/sh "${ROOTFS}"/bin/dash "${ROOTFS}"/bin/bash \
           "${ROOTFS}"/usr/bin/env; do
    [ -f "$b" ] && seeds+=("$b")
  done
  [ "${#seeds[@]}" -gt 0 ] || die "found no Astrix binaries under ${ROOTFS}/usr/bin"

  local queue=() cur dep
  for b in "${seeds[@]}"; do queue+=("${b#${ROOTFS}/}"); done

  # The interpreter itself is not in any NEEDED table, and a closure that
  # omits it produces a bundle that fails with "no such file or directory" on
  # the very first exec - which reads like a broken bundle, not a missing loader.
  local interp
  interp="$(elf_interp "${seeds[0]}")"
  [ -n "$interp" ] && queue+=("$interp")

  local copied=0 libs=0 unresolved=""
  while [ "${#queue[@]}" -gt 0 ]; do
    cur="${queue[0]}"
    queue=("${queue[@]:1}")
    [ -e "${ROOTFS}/${cur}" ] || continue
    # Already in the bundle: skip it entirely, including the NEEDED walk.
    # Re-reading every library's dynamic section for each of its dependants
    # turned a few hundred files into tens of thousands of readelf calls.
    [ -e "$MOUNT_DIR/$cur" ] && continue
    mkdir -p "$MOUNT_DIR/$(dirname "$cur")"
    cp -a "${ROOTFS}/${cur}" "$MOUNT_DIR/$cur"
    copied=$((copied + 1))
    case "$cur" in
      *.so|*.so.*) libs=$((libs + 1)) ;;
    esac
    for dep in $(elf_needed "${ROOTFS}/${cur}" 2>/dev/null || true); do
      local p
      if p="$(resolve_lib "$dep")"; then
        queue+=("$p")
        # A versioned SONAME with a symlink needs the siblings resolved from
        # the same directory, or the loader walks off at run time.
        case "$p" in *.so) ;; *)
          local sib
          for sib in "${ROOTFS}/${p}"*; do
            [ -e "$sib" ] && queue+=("${sib#${ROOTFS}/}")
          done
          ;;
        esac
      else
        case "$unresolved" in *"|${dep}|"*) ;; *) unresolved="${unresolved}|${dep}|" ;; esac
      fi
    done
  done
  ok "closure: ${copied} files (${libs} shared objects) from ${#seeds[@]} entry points"

  if [ -n "$unresolved" ]; then
    warn "these libraries were needed but not found in the rootfs:"
    printf '%s' "$unresolved" | tr '|' '\n' | grep -v '^$' | sed 's/^/      /'
    warn "the bundle may fail to start on the device; rebuild with ./build.sh first"
  fi

  # 2. the things that are data, not code
  log "Copying data files"
  # root/ and tmp/ are not data as such, but the launcher chroots with -w /root
  # and proot needs somewhere to write: a bundle without them starts and then
  # dies on "cannot change directory", which reads like a proot problem.
  mkdir -p "$MOUNT_DIR/root/.astrix-run" "$MOUNT_DIR/tmp"
  chmod 1777 "$MOUNT_DIR/tmp"

  # /etc is copied by explicit list rather than wholesale. The whole tree is
  # 4 MB of systemd unit symlinks pointing at services this bundle does not
  # contain, so copying it produces hundreds of dangling links and a warning
  # list nobody can act on. These are the entries a process actually reads.
  local e got=0
  for e in etc/os-release etc/ld.so.cache etc/ld.so.conf etc/ld.so.conf.d \
           etc/passwd etc/group etc/hosts etc/host.conf etc/nsswitch.conf \
           etc/localtime etc/timezone etc/mime.types etc/X11; do
    [ -e "${ROOTFS}/${e}" ] || continue
    mkdir -p "$MOUNT_DIR/$(dirname "$e")"
    cp -a "${ROOTFS}/${e}" "$MOUNT_DIR/${e}"
    got=$((got + 1))
  done
  ok "/etc (${got} entries)"

  for d in usr/share/applications usr/share/wayland usr/lib/astrix; do
    if [ -d "${ROOTFS}/${d}" ]; then
      mkdir -p "$MOUNT_DIR/$(dirname "$d")"
      cp -a "${ROOTFS}/${d}" "$MOUNT_DIR/${d}"
      ok "/$d"
    fi
  done

  # Mesa's software rasteriser. The X11 backend needs GLX, and the phone's
  # Adreno has no userspace GL driver we can reach - llvmpipe is what makes
  # this route render at all, slowly.
  # Mesa's software rasteriser lives under whichever multiarch lib dir this
  # rootfs uses - Debian's merged /usr means it can be either lib/ or usr/lib/.
  local dri_src dri_dst=""
  for dri_src in "${ROOTFS}/usr/lib/aarch64-linux-gnu/dri" \
                  "${ROOTFS}/lib/aarch64-linux-gnu/dri"; do
    [ -d "$dri_src" ] || continue
    dri_dst="usr/lib/aarch64-linux-gnu/dri"
    mkdir -p "${MOUNT_DIR}/${dri_dst}"
    for so in swrast_dri.so kms_swrast_dri.so virtio_gpu_dri.so; do
      [ -f "${dri_src}/${so}" ] && cp -a "${dri_src}/${so}" "${MOUNT_DIR}/${dri_dst}/"
    done
    break
  done
  if [ -n "$dri_dst" ] && [ -f "${MOUNT_DIR}/${dri_dst}/swrast_dri.so" ]; then
    ok "mesa software rasteriser (llvmpipe) at /${dri_dst}"
  else
    warn "no swrast_dri.so found; without it the X11 session may not render"
  fi

  # 3. the launcher
  mkdir -p "$MOUNT_DIR/opt/astrix"
  write_launcher "$MOUNT_DIR/${LAUNCHER_REL}"
  chmod +x "$MOUNT_DIR/${LAUNCHER_REL}"
  ok "launcher at /${LAUNCHER_REL}"

  # 4. repair what cp -a cannot: symlinks whose target was never selected.
  #
  # /bin/sh is a symlink to dash, /etc/os-release points at ../usr/lib/os-release,
  # and almost every library SONAME has versioned siblings. cp -a reproduces the
  # link faithfully and nothing else, so a closure walk that only follows NEEDED
  # edges produces a bundle full of links pointing at files that are not there.
  # The failure this prevents is particularly nasty: it is not "it will not
  # start", it is "it starts and then exec fails on the first command", which
  # reads as a broken proot rather than a missing library.
  local pass link target abs rel src repaired=0
  # Several rounds: a repaired link can be pointing at another link (a SONAME
  # chain), and find's snapshot only sees what was dangling at the start of
  # this round. The total is accumulated across rounds rather than reported
  # from the last one, which is how this reported "repaired 0" while doing
  # work.
  for pass in 1 2 3 4; do
    local fixed=0
    while IFS= read -r link; do
      [ -e "$link" ] && continue
      target="$(readlink "$link")"
      # Resolve the link inside the BUNDLE's namespace, make it bundle-relative,
      # and only then look the same path up in the rootfs. Asking the bundle
      # whether the destination exists is the bug this loop exists to fix: a
      # dangling link's destination is dangling by definition.
      case "$target" in
        /*) abs="${MOUNT_DIR}${target}" ;;
        *)  abs="$(cd "$(dirname "$link")" && pwd)/${target}" ;;
      esac
      rel="$(realpath -m --relative-to="$MOUNT_DIR" "$abs")"
      case "$rel" in
        ../*|*/../*|"") continue ;;   # points outside the bundle; leave it
      esac
      src="${ROOTFS}/${rel}"
      if [ -f "$src" ] || [ -L "$src" ]; then
        mkdir -p "$MOUNT_DIR/$(dirname "$rel")"
        cp -a "$src" "$MOUNT_DIR/${rel}"
        fixed=$((fixed + 1))
      fi
    done < <(find "$MOUNT_DIR" -xtype l 2>/dev/null || true)
    repaired=$((repaired + fixed))
    [ "$fixed" -eq 0 ] && break
  done
  pass="repaired ${repaired} dangling symlink(s)"
  ok "$pass"

  # 5. pack
  log "Packing $(human_size "$MOUNT_DIR") -> ${BUNDLE}"
  mkdir -p "$OUT"
  # -6 rather than -19: this runs on a 1-vCPU developer box, and the
  # difference is a few megabytes on a bundle that crosses a phone's USB.
  tar -C "$MOUNT_DIR" -cf - . | zstd -T0 -6 -q -f -o "$BUNDLE"
  printf '%s\n' "${DEVICE_NAME}" "${DEVICE_CODENAME}" > "${OUT}/android-host-info.txt"
  cat "${OUT}/android-host-info.txt" | tr '\n' ' ' | sed 's/^ */  /'
  printf '\n'

  printf '\n'
  # The last honest check: a bundle with a dangling symlink is a bundle that
  # fails on the phone, and the phone is where that is most annoying to debug.
  local dangling
  dangling="$(find "$MOUNT_DIR" -xtype l 2>/dev/null | wc -l)"
  if [ "$dangling" -eq 0 ]; then
    pass "no dangling symlinks in the bundle"
  else
    # No pipe into head here: with pipefail, head closing the pipe early kills
    # find and the whole script exits 141 instead of printing a warning.
    warn "${dangling} dangling symlink(s) remain; the session may fail to start:"
    find "$MOUNT_DIR" -xtype l 2>/dev/null -printf '      %P\n' || true
  fi

  ok "bundle: ${BUNDLE} ($(human_size "$BUNDLE"))"
  dim "next: ./scripts/port-redmi-8a.sh android-host check"
  printf '\n'
}

write_launcher() {
  cat > "$1" <<'LAUNCHER'
#!/data/data/com.termux/files/usr/bin/sh
# Astrix OS - run Astrix on this phone, on Android's own kernel.
#
# Generated by scripts/port-redmi-8a.sh. Run it from Termux:
#   chmod +x /sdcard/Download/astrix/astrix-android-host.sh
#   /sdcard/Download/astrix/astrix-android-host.sh
#
# Read this before assuming you are booting Astrix. You are not. Android's
# kernel, its drivers and its bootloader are still underneath, and they are
# what is drawing. What runs here is the Astrix userspace - compositor, shell,
# apps - inside a proot chroot, with its output bridged out to SurfaceFlinger
# through Termux:X11. That is a real Astrix session on real hardware; it is not
# Astrix OS running on its own kernel, and nothing here says it is.
set -e

BUNDLE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$BUNDLE/rootfs"
RUN="$HOME/.astrix-run"
mkdir -p "$RUN"
chmod 700 "$RUN"

# Termux:X11 serves VNC on 6000 and is reached through the loopback it sets
# up. adb forward tcp:6000 tcp:6000 must be active; the packer's `push`
# command sets it up for you.
export DISPLAY=127.0.0.1:6000
export XDG_RUNTIME_DIR="$RUN"
export WLR_BACKEND=x11
# No GPU GL driver is reachable from inside the chroot, and asking for one
# produces a black window with a GLX error instead of a slow window.
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export WLR_RENDERER=pixman
export ASTRIX_POINTER_SCALE=2.0

# Termux ships proot as a static binary; it is what lets a glibc ARM64 tree
# run on Android's bionic userspace boundary.
PROOT=proot
command -v "$PROOT" >/dev/null 2>&1 || PROOT=./proot

run_in_astrix() {
  "$PROOT" -r "$ROOT" \
    -b /dev -b /proc -b /sys -b /sdcard -b "$HOME" -w /root \
    /usr/bin/env -i \
      HOME=/root TERM=xterm \
      PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin \
      DISPLAY="$DISPLAY" XDG_RUNTIME_DIR="$RUN" \
      WLR_BACKEND="$WLR_BACKEND" LIBGL_ALWAYS_SOFTWARE=1 \
      GALLIUM_DRIVER="$GALLIUM_DRIVER" WLR_RENDERER="$WLR_RENDERER" \
      ASTRIX_POINTER_SCALE="$ASTRIX_POINTER_SCALE" \
      "$@"
}

case "${1:-start}" in
  start)
    echo "astrix: starting the compositor (log: $HOME/astrix-compositor.log)"
    run_in_astrix /usr/bin/astrix-compositor >"$HOME/astrix-compositor.log" 2>&1 &
    CPID=$!
    echo "astrix: waiting for the Wayland socket"
    i=0
    while [ $i -lt 100 ] && [ ! -S "$RUN/astrix-0" ]; do sleep 0.1; i=$((i + 1)); done
    if [ ! -S "$RUN/astrix-0" ]; then
      echo "astrix: the compositor never created its socket; last lines:"
      tail -20 "$HOME/astrix-compositor.log"
      exit 1
    fi
    echo "astrix: socket ready, starting the shell"
    run_in_astrix /usr/bin/astrix-shell
    ;;
  stop)
    pkill -f astrix-shell || true
    pkill -f astrix-compositor || true
    echo "astrix: stopped"
    ;;
  *)
    echo "usage: $0 [start|stop]" >&2
    exit 2
    ;;
esac
LAUNCHER
}

# ---------------------------------------------------------------------------
# android-host check
# ---------------------------------------------------------------------------
cmd_android_host_check() {
  need_adb
  printf '\n'
  log "Checking the phone"

  local serial abi model rel
  serial="$(adb_device)"
  ok "phone ${serial}"

  abi="$(adb_ shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')"
  if [ "${abi}" = "arm64-v8a" ]; then
    ok "arm64-v8a, which is what the Astrix build is"
  else
    err "phone reports ABI '${abi}'; Astrix is arm64-v8a only"
    die "this bundle will not run on this phone"
  fi

  model="$(adb_ shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
  info "model: ${model:-unknown}"

  local pkg
  for pkg in com.termux com.termux.x11 com.termux.proot com.termux.api; do
    if adb_ shell pm list packages "$pkg" 2>/dev/null | grep -q "^package:${pkg}\$"; then
      pass "$pkg installed"
    else
      warn "$pkg not installed"
    fi
  done

  local need=""
  adb_ shell pm list packages com.termux.proot 2>/dev/null | grep -q package || need="Termux:PROOT "
  adb_ shell pm list packages com.termux.x11  2>/dev/null | grep -q package || need="${need}Termux:X11 "
  if [ -n "$need" ]; then
    printf '\n'
    err "the android-host route cannot run without:${need}"
    dim "  install them from F-Droid, then re-run this check:"
    dim "    Termux      https://f-droid.org/packages/com.termux/"
    dim "    Termux:X11  https://f-droid.org/packages/com.termux.x11/"
    dim "    Termux:PROOT https://f-droid.org/packages/com.termux.proot/"
    dim "  (Termux is not on the Play Store and must not be installed from it)"
    exit 1
  fi

  # The phone has to grant Termux access to its own storage before the bundle
  # can be read out of /sdcard. This is checked, not assumed, because the
  # failure otherwise shows up as an empty directory much later.
  if adb_ shell "ls /sdcard/Android/data/com.termux/files >/dev/null 2>&1" >/dev/null 2>&1; then
    pass "Termux storage permission looks granted"
  else
    warn "Termux has no storage permission yet"
    dim "  open Termux once and run: termux-setup-storage, accept the prompt"
  fi

  local size
  size="$(adb_ shell wm size 2>/dev/null | tr -d '\r' | sed -n 's/.*: *//p')"
  info "display: ${size:-unknown} (Astrix will render at ${DEVICE_PANEL_WIDTH}x${DEVICE_PANEL_HEIGHT})"
  dim "the shell scales to whatever it is given; this is the panel's native size"

  printf '\n'
  ok "the phone can run the android-host route"
  dim "next: ./scripts/port-redmi-8a.sh android-host pack && ... android-host push"
  printf '\n'
}

# ---------------------------------------------------------------------------
# android-host push / start / stop
# ---------------------------------------------------------------------------
cmd_android_host_push() {
  need_adb
  [ -f "$BUNDLE" ] || die "no bundle at ${BUNDLE}. Run 'android-host pack' first."
  local serial
  serial="$(adb_device)"
  mkdir -p "$OUT"
  printf '%s\n' "${BUNDLE}" > "${OUT}/android-host-bundle.txt"

  local remote="/sdcard/Download/astrix"
  log "Copying $(human_size "$BUNDLE") to ${remote}/"
  # --sync is not just faster here: `adb push` on a FAT-formatted sdcard has
  # to rewrite whole clusters, and without it a 300 MB tarball takes the
  # better part of an hour over USB 2.
  adb_ push -a --sync "$BUNDLE" "${remote}/android-host-rootfs.tar.zst"
  adb_ push "$MOUNT_DIR/${LAUNCHER_REL}" "${remote}/astrix-android-host.sh"
  adb_ shell "chmod 755 ${remote}/astrix-android-host.sh" >/dev/null 2>&1 || true

  # Termux:X11's VNC server listens on 6000 on the device. Forwarding it is
  # the whole display path for this route.
  log "Forwarding the X11 display"
  adb_ forward tcp:6000 tcp:6000 >/dev/null || warn "adb forward failed; the session may not start"

  cat <<EOF

$(ok "bundle is on the phone at ${remote}/")

  On the phone, open Termux:X11 and start its VNC server
  (the "Start VNC" button in the notification), then open Termux and run:

      bash ${remote}/astrix-android-host.sh start

  The Astrix session appears in the Termux:X11 window. Touch input reaches
  the shell through the X11 pointer, so the on-screen keyboard and the app
  grid both work.

$(dim "to stop it:  bash ${remote}/astrix-android-host.sh stop")

$(warn "This is Astrix running on Android's kernel. It is not Astrix OS booted")
$(dim "on its own kernel, and no part of this touched the bootloader or any")
$(dim "partition. Nothing on the phone was changed except ${remote}/.")

EOF
}

cmd_android_host_start() {
  need_adb
  adb_ forward tcp:6000 tcp:6000 >/dev/null || true
  local remote="/sdcard/Download/astrix"
  if ! adb_ shell "test -f ${remote}/astrix-android-host.sh" >/dev/null 2>&1; then
    die "no launcher on the phone. Run 'android-host push' first."
  fi

  # Termux:API's RunCommandService is the only supported way to start a shell
  # command in Termux from adb. Without it the launcher has to be typed, which
  # is a perfectly good outcome - this just says so instead of failing oddly.
  if ! adb_ shell pm list packages com.termux.api 2>/dev/null | grep -q package; then
    cat <<EOF

$(err "Termux:API is not installed, so this cannot be started from the host.")

  Open Termux on the phone and run, by hand:

      bash ${remote}/astrix-android-host.sh start

  That is the whole command. Installing Termux:API from F-Droid makes the
  next run work from here instead.

EOF
    exit 1
  fi

  log "Starting the session on ${serial:-the phone}"
  adb_ shell am startservice \
    -n com.termux/.app.RunCommandService \
    -a com.termux.RUN_COMMAND \
    --es com.termux.RUN_COMMAND_PATH "/data/data/com.termux/files/usr/bin/bash" \
    --esa com.termux.RUN_COMMAND_ARGUMENTS "-lc,sh ${remote}/astrix-android-host.sh start" \
    --ez com.termux.RUN_COMMAND_BACKGROUND false \
    >/dev/null
  ok "session starting; watch the Termux:X11 window"
  dim "logs: on the phone, cat \$HOME/astrix-compositor.log"
}

cmd_android_host_stop() {
  need_adb
  adb_ shell am startservice \
    -n com.termux/.app.RunCommandService \
    -a com.termux.RUN_COMMAND \
    --es com.termux.RUN_COMMAND_PATH "/data/data/com.termux/files/usr/bin/bash" \
    --esa com.termux.RUN_COMMAND_ARGUMENTS "-lc,sh /sdcard/Download/astrix/astrix-android-host.sh stop" \
    >/dev/null 2>&1 || warn "could not reach Termux:API; stop it from the Termux session"
  ok "stop requested"
}

# ---------------------------------------------------------------------------
# boot-test build
# ---------------------------------------------------------------------------
# What is flashed is a normal Android boot image containing Astrix's kernel and
# the initramfs this build already produced. There is no special format and no
# vendor code: `fastboot flash boot` on an unlocked bootloader takes it.
cmd_boot_test_build() {
  need_fastboot
  local kernel
  kernel=""
  for k in Image Image.gz; do
    [ -f "${ASTRIX_BUILD_DIR}/out/boot/${k}" ] && kernel="${ASTRIX_BUILD_DIR}/out/boot/${k}"
  done
  [ -n "$kernel" ] ||
    die "no kernel at ${ASTRIX_BUILD_DIR}/out/boot/. Run ./build.sh first."
  local initrd="${ROOTFS}/initrd.img"
  [ -f "$initrd" ] ||
    die "no initramfs at ${initrd}. Run ./build.sh first."

  mkdir -p "$OUT/boot-test"
  local dest="${OUT}/boot-test/boot-test.img"

  # abootimg builds a boot image around an existing (stock) one. If no stock
  # image is available it creates one from scratch, which is the case that
  # matters here because we must not depend on a particular ROM.
  local stock="${ASTRIX_STOCK_BOOT_IMAGE:-}"
  if [ -z "$stock" ] && command -v adb >/dev/null && adb devices | grep -qq device; then
    # Not pulled automatically: `adb pull /dev/block/...` needs root and quietly
    # failing on a locked phone is worse than saying so.
    dim "no ASTRIX_STOCK_BOOT_IMAGE set; building the boot header from scratch."
    dim "If this image fails to boot, the fix is usually a stock boot.img from"
    dim "your exact ROM version - flash it with the vendor tool, not fastboot."
  fi

  log "Building the boot-test image"
  local cmdline="console=ttyMSM0,115200,8n1 earlycon=msm_geni_serial,0x994000 androidboot.hardware=qcom"
  rm -f "$dest"

  if command -v mkbootimg >/dev/null; then
    # mkbootimg is AOSP's own tool and gets the header versions right.
    # Preferred when present; the abootimg path is the fallback.
    if mkbootimg --kernel "$kernel" --ramdisk "$initrd" --output "$dest" \
                  --pagesize 4096 --cmdline "$cmdline" --board astrix; then
      ok "boot image built with mkbootimg"
    else
      rm -f "$dest"
      err "mkbootimg failed"
      die "no boot image was written."
    fi
  elif command -v abootimg >/dev/null; then
    if [ -n "$stock" ] && [ -f "$stock" ]; then
      cp "$stock" "$dest"
      if abootimg -u "$dest" -k "$kernel" -r "$initrd" -c "cmdline=${cmdline}"; then
        ok "boot image built from the stock header of ${stock}"
      else
        rm -f "$dest"
        err "abootimg -u failed"
        die "no boot image was written."
      fi
    else
      # abootimg 0.6 (what Debian ships) has no long options and parses its
      # config file with a line-oriented reader that rejects ANY line it does
      # not recognise - a comment line is enough to abort the whole build.
      local cfg="${OUT}/boot-test/bootimg.cfg"
      {
        printf 'cmdline=%s\n' "$cmdline"
        printf 'pagesize=4096\n'
        printf 'name=astrix\n'
      } > "$cfg"
      if abootimg --create "$dest" -f "$cfg" -k "$kernel" -r "$initrd"; then
        ok "boot image built with abootimg (0.6 interface)"
      else
        # abootimg leaves a zero-length file behind when it gives up. Without
        # this check the next line would cheerfully print "ok" over a boot
        # image that does not exist, which is the exact lie this project does
        # not tell.
        rm -f "$dest"
        err "abootimg failed"
        dim "  abootimg -h for its interface; it differs from 1.0's long options."
        die "no boot image was written."
      fi
    fi
  else
    die "neither mkbootimg nor abootimg is installed (apt-get install abootimg)"
  fi

  # The tool said it worked. Check the artefact anyway: a boot image that is
  # zero-length, or that does not contain this kernel, is not a boot image.
  [ -s "$dest" ] || die "${dest} is empty after the build; refusing to call it done."
  local ksize
  ksize="$(stat -c %s "$dest")"
  [ "$ksize" -gt "$(( $(stat -c %s "$kernel") + $(stat -c %s "$initrd") ))" ] ||
    die "${dest} is ${ksize} bytes, too small to contain the kernel and initramfs"
  if command -v abootimg >/dev/null && abootimg -i "$dest" >/dev/null 2>&1; then
    pass "abootimg reads the result back as a valid Android boot image"
  fi

  cp "$kernel" "${OUT}/boot-test/Image"
  cp "$initrd" "${OUT}/boot-test/initrd.img"
  cat > "${OUT}/boot-test/README.txt" <<'EOF'
This is a boot TEST, not a working OS.

The screen will stay dark. There is no Adreno 505 driver in mainline Linux, so
there is nothing in this kernel able to draw to the phone's panel. What this
image proves is narrower and still worth having: that Astrix's kernel starts on
olive, initialises the board, and reaches userspace.

Read the proof on the serial console, not on the screen:

  console: ttyMSM0, 115200 8n1

To get Android back, flash the stock boot.img for your exact ROM with the
vendor tool, or with `fastboot flash boot stock-boot.img` from an unlocked
fastboot. Nothing else Astrix wrote was touched.
EOF

  printf '\n'
  ok "boot image: ${dest} ($(human_size "$dest"))"
  dim "stage it with: ./scripts/port-redmi-8a.sh boot-test stage"
  dim "read the proof with: ./scripts/port-redmi-8a.sh boot-test log"
  printf '\n'
}

# ---------------------------------------------------------------------------
# boot-test stage
# ---------------------------------------------------------------------------
cmd_boot_test_stage() {
  # The consent gate comes before the input check on purpose. Whether the image
  # exists is not the question that matters here; whether this person has said
  # out loud that they understand they are replacing a phone's kernel is.
  if [ "${ASTRIX_ALLOW_UNVERIFIED_FLASH:-0}" != "1" ]; then
    err "Refusing to stage a boot image for a device Astrix has never booted."
    printf '\n'
    dim "  Nothing in this repository has booted on ${DEVICE_NAME}. Flashing this"
    dim "  replaces Android's kernel on a phone, and the screen will stay dark"
    dim "  for the whole boot because there is no Adreno 505 mainline driver."
    printf '\n'
    dim "  If you understand that and want the boot test anyway:"
    dim "      ASTRIX_ALLOW_UNVERIFIED_FLASH=1 \\"
    dim "        ./scripts/port-redmi-8a.sh boot-test stage"
    printf '\n'
    exit 2
  fi

  printf '\n'
  warn "ASTRIX_ALLOW_UNVERIFIED_FLASH=1 - staging an unverified boot image."
  dim "  ${DEVICE_NAME} has never booted Astrix. Nothing about that changes"
  dim "  because the override was set."

  need_fastboot
  local img="${OUT}/boot-test/boot-test.img"
  [ -f "$img" ] || die "no boot image at ${img}. Run 'boot-test build' first."

  fastboot devices | grep -q . || die "no phone in fastboot mode (power off, hold vol+vol-, or 'adb reboot bootloader')"

  local unlocked
  unlocked="$(fastboot getvar unlocked 2>&1 | sed -n 's/.*unlocked: *//p' | head -1)"
  case "$unlocked" in
    yes|true) ok "bootloader is unlocked" ;;
    *) err "the bootloader is locked; fastboot cannot write to boot."
       dim "  ${DEVICE_BOOTLOADER_UNLOCK}"
       dim "  Unlocking erases the phone. Nothing in Astrix shortens that wait."
       exit 3 ;;
  esac

  printf '\n'
  warn "About to REPLACE Android's kernel on ${DEVICE_NAME}."
  dim "  only 'boot' is written. No userdata, no system, no dtbo."
  dim "  the phone will not come back without a stock boot image (see rollback)."
  printf '\n'
  printf '  Type the device codename (%s) to continue: ' "${DEVICE_CODENAME}"
  read -r confirm
  [ "$confirm" = "${DEVICE_CODENAME}" ] || { ok "aborted; nothing was written."; exit 0; }

  fastboot flash boot "$img"
  fastboot reboot
  printf '\n'
  ok "boot partition written; the phone is rebooting into Astrix's kernel"
  warn "A clean flash is not a boot. The proof is on the serial console:"
  dim "  ./scripts/port-redmi-8a.sh boot-test log"
  printf '\n'
}

# ---------------------------------------------------------------------------
# boot-test log
# ---------------------------------------------------------------------------
# The Redmi 8A's UART comes out as a USB serial gadget. This reads whatever
# serial device the phone enumerates as and says plainly which one it used, so
# that an empty capture is never mistaken for a failed boot.
cmd_boot_test_log() {
  local secs="${1:-120}" out="${ASTRIX_BUILD_DIR}/boot-test-serial.log"
  mkdir -p "$OUT"
  dev=""
  for d in /dev/ttyUSB* /dev/ttyACM*; do
    [ -e "$d" ] && { dev="$d"; break; }
  done
  if [ -z "$dev" ]; then
    printf '\n'
    err "no serial device found (/dev/ttyUSB*, /dev/ttyACM*)."
    dim "  plug the phone in from fastboot, or enable USB debugging and run"
    dim "  'adb reboot bootloader' - the console appears once the kernel starts"
    dim "  enumerating it. Without the phone there is nothing to read."
    printf '\n'
    exit 1
  fi

  ok "reading ${dev} for ${secs}s"
  command -v stty >/dev/null && stty -F "$dev" 115200 raw -echo 2>/dev/null || true
  sudo -n timeout "$secs" cat "$dev" 2>/dev/null | tee "$out" || true
  printf '\n'
  if [ -s "$out" ]; then
    ok "captured $(wc -l < "$out") lines -> ${out}"
    dim "what to look for: 'Linux version', then systemd, then astrix-boot-report"
  else
    warn "the capture is empty"
    warn "an empty capture is not proof of a failed boot: the console may not have"
    warn "enumerated yet, or this kernel's earlycon is not wired to the USB gadget."
  fi
  printf '\n'
}

cmd_boot_test_rollback() {
  cat <<EOF

$(ok "putting Android back")

  ${DEVICE_NAME} uses A/B, so the kernel you overwrote is usually still there:

    adb reboot bootloader
    fastboot --set-active=a          # or b
    fastboot reboot

  If the phone still lands on Astrix, flash the stock boot.img for your exact
  ROM version and put it back the same way:

    fastboot flash boot stock-boot.img
    fastboot --set-active=a
    fastboot reboot

  Nothing outside the 'boot' partition was written, so no partition wipe or
  full ROM reflash should be needed. If it looks like one is, stop and say so
  rather than erasing anything else.

EOF
}

# ---------------------------------------------------------------------------
CMD="${1:-}"
SUB="${2:-}"
case "$CMD" in
  status)            cmd_status ;;
  android-host)
    case "$SUB" in
      pack)  cmd_android_host_pack ;;
      check) cmd_android_host_check ;;
      push)  cmd_android_host_push ;;
      start) cmd_android_host_start ;;
      stop)  cmd_android_host_stop ;;
      *) err "android-host needs one of: pack check push start stop"; exit 1 ;;
    esac ;;
  boot-test)
    case "$SUB" in
      build)    cmd_boot_test_build ;;
      stage)    cmd_boot_test_stage ;;
      log)      shift 2 || true; cmd_boot_test_log "${1:-120}" ;;
      rollback) cmd_boot_test_rollback ;;
      *) err "boot-test needs one of: build stage log rollback"; exit 1 ;;
    esac ;;
  -h|--help|help|"") usage ;;
  *) err "unknown command: $CMD"; usage; exit 1 ;;
esac