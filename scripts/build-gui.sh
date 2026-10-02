#!/usr/bin/env bash
# Astrix OS - build the graphical stack (compositor, shell, apps).
#
# The GUI is compiled *inside the target rootfs* rather than with a host
# cross-compiler. This matters:
#   - the rootfs is the same Debian suite the OS will run, so glibc, wlroots
#     and every header match exactly at runtime;
#   - a host cross-toolchain would mix a different glibc's headers with the
#     target's libraries, which produces binaries that compile cleanly and
#     then fail at runtime.
#
# Requires: build-rootfs.sh to have run at least once.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

# Single-writer lock on the staging tree.
#
# This build stages the sources into ${ROOTFS}/astrix-build and then `rm -rf`s
# and re-creates that directory. Two concurrent runs therefore delete each
# other's sources out from under the compiler, and the symptom is a wall of
# "No such file or directory" for files that were demonstrably there a second
# earlier - which is indistinguishable from a real build error and cost a
# debugging cycle to tell apart. mkdir is atomic, so it is both the lock and
# its own staleness check.
ASTRIX_GUI_LOCK="${ASTRIX_BUILD_DIR}/.gui-build.lock"
mkdir -p "$(dirname "$ASTRIX_GUI_LOCK")"
if ! mkdir "$ASTRIX_GUI_LOCK" 2>/dev/null; then
  die "another build-gui.sh is already running (lock: ${ASTRIX_GUI_LOCK});
     if you are sure it is not, remove that directory and retry"
fi
gui_lock_release() { rmdir "$ASTRIX_GUI_LOCK" 2>/dev/null || true; }
trap 'gui_lock_release' EXIT

ROOTFS="${ASTRIX_BUILD_DIR}/rootfs"
GUI_STAGE="${ASTRIX_BUILD_DIR}/gui"
PROTO_DIR="${ASTRIX_REPO_ROOT}/protocol"
GUI_SRC="${ASTRIX_REPO_ROOT}/gui"

JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 1)}"
NATIVE=0
VERBOSE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --native)  NATIVE=1; shift ;;   # build for the host, for tests
    -j)        JOBS="$2"; shift 2 ;;
    -v)        VERBOSE=1; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
[ -d "$ROOTFS" ] || die "no rootfs at $ROOTFS - run ./scripts/build-rootfs.sh first"
[ -x "$ROOTFS/usr/bin/gcc" ] || die "the rootfs has no C compiler - rebuild with --full (dev packages)"

# Wayland protocol XML. Debian's libwlroots-dev does not ship the wlr-*
# protocol definitions, so the project vendors them; see protocol/README.md.
[ -f "${PROTO_DIR}/wlr-layer-shell-unstable-v1.xml" ] || die "missing protocol XML in ${PROTO_DIR}"

if [ "$NATIVE" -eq 1 ]; then
  log "Building GUI natively (host) into $GUI_STAGE"
  BUILD_IN="$GUI_STAGE"
  # Host builds are throwaway artifacts used by the test suite; keep them out
  # of the image.
  BIN_OUT="$GUI_STAGE"
  LIB_OUT="$GUI_STAGE/lib"
  mkdir -p "$BUILD_IN"
  # The host toolchain uses its own wlroots if present; otherwise this fails
  # loudly rather than silently producing nothing.
  command -v gcc >/dev/null || die "gcc not found for --native build"
  PKGENV=""
else
  log "Building GUI for ${DEB_TRIPLET} inside $ROOTFS"
  BUILD_IN="${ROOTFS}/astrix-build"
  rm -rf "$BUILD_IN"
  mkdir -p "$BUILD_IN"
  # Stage sources into the rootfs. The apps go in too: they are part of the
  # graphical stack and are built against the same headers as the shell.
  cp -a "${GUI_SRC}/." "$BUILD_IN/gui/"
  cp -a "${ASTRIX_REPO_ROOT}/apps" "$BUILD_IN/apps"
  mkdir -p "$BUILD_IN/protocol"
  cp -a "${PROTO_DIR}/." "$BUILD_IN/protocol/"
  PKGENV="export PKG_CONFIG_PATH=/usr/lib/${DEB_TRIPLET}/pkgconfig"
  # Install straight into the rootfs, at the paths the systemd units execute.
  #
  # This has to be inside the rootfs, and that is not cosmetic. in_rootfs()
  # is a plain chroot with no path rewriting, and in_rootfs_path() returns
  # paths outside the rootfs unchanged - so writing a target build to a host
  # staging directory does not write it there. It writes to
  # <rootfs>/<host-absolute-path>, a nested directory that looks plausible and
  # is silently wrong. Building into the rootfs makes the two paths line up
  # and leaves the binaries correctly installed for the image.
  #
  # astrix-compositor/astrix-shell are launched from /usr/bin by
  # system/services/*.service; the apps take the same names under astrix-*.
  BIN_OUT="${ROOTFS}/usr/bin"
  LIB_OUT="${ROOTFS}/usr/lib/astrix/gui"
fi

# Paths as the code inside the chroot sees them.
BUILD_IN_CHROOT="$(in_rootfs_path "$ROOTFS" "$BUILD_IN")"
BIN_OUT_CHROOT="$(in_rootfs_path "$ROOTFS" "$BIN_OUT")"
LIB_OUT_CHROOT="$(in_rootfs_path "$ROOTFS" "$LIB_OUT")"

# The output directories have to exist *inside* the chroot too; creating them
# on the host is not enough, because the two are different trees here.
in_rootfs "$ROOTFS" mkdir -p "$BIN_OUT_CHROOT" "$LIB_OUT_CHROOT"
mkdir -p "$LIB_OUT"

# ---------------------------------------------------------------------------
# Generate Wayland protocol sources
# ---------------------------------------------------------------------------
log "Generating Wayland protocol bindings"
if [ "$NATIVE" -eq 1 ]; then
  mkdir -p "$BUILD_IN/pg"
  for xml in "$PROTO_DIR"/*.xml; do
    n=$(basename "$xml" .xml)
    wayland-scanner server-header "$xml" "$BUILD_IN/pg/${n}-protocol.h"
    wayland-scanner private-code  "$xml" "$BUILD_IN/pg/${n}-protocol.c"
  done
  # xdg-shell comes from the distribution's wayland-protocols package.
  for f in xdg-shell; do
    src="/usr/share/wayland-protocols/stable/$f/$f.xml"
    [ -f "$src" ] || die "wayland-protocols not installed (missing $src)"
    wayland-scanner server-header "$src" "$BUILD_IN/pg/${f}-protocol.h"
    wayland-scanner private-code  "$src" "$BUILD_IN/pg/${f}-protocol.c"
  done
  # The compositor needs the server side; the shell and every app are
  # clients and need the client header. Generating only one of the two is how
  # a build ends up with a compositor that compiles and a shell that does not,
  # so both are produced here.
  wayland-scanner client-header \\
    /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \\
    "$BUILD_IN/pg/xdg-shell-client-header.h"
  # The shell is a client of the virtual keyboard protocol too: it is the only
  # way an on-screen keyboard can deliver a key to the focused application.
  wayland-scanner client-header \\
    "$PROTO_DIR/virtual-keyboard-unstable-v1.xml" \\
    "$BUILD_IN/pg/virtual-keyboard-unstable-v1-client-header.h"
else
  in_rootfs "$ROOTFS" /bin/bash -euxo pipefail -c "
    cd '$BUILD_IN_CHROOT'
    mkdir -p pg
    for xml in protocol/*.xml; do
      n=\$(basename \"\$xml\" .xml)
      wayland-scanner server-header \"\$xml\" pg/\${n}-protocol.h
      wayland-scanner private-code  \"\$xml\" pg/\${n}-protocol.c
    done
    for f in xdg-shell; do
      src=/usr/share/wayland-protocols/stable/\$f/\$f.xml
      test -f \"\$src\" || { echo \"wayland-protocols missing: \$src\" >&2; exit 1; }
      wayland-scanner server-header \"\$src\" pg/\${f}-protocol.h
      wayland-scanner private-code  \"\$src\" pg/\${f}-protocol.c
    done
    wayland-scanner client-header \\
      /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \\
      pg/xdg-shell-client-header.h
    wayland-scanner client-header \\
      protocol/virtual-keyboard-unstable-v1.xml \\
      pg/virtual-keyboard-unstable-v1-client-header.h
    echo 'generated:' && ls pg
  "
fi
ok "protocol bindings generated"

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------
# Every target is compiled the same way. Adding a new component means adding a
# block here, which keeps the flags identical across all of them.
CFLAGS_COMMON="-std=c11 -D_GNU_SOURCE -DWLR_USE_UNSTABLE -O2 -g"
CFLAGS_COMMON="$CFLAGS_COMMON -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers"
CFLAGS_COMMON="$CFLAGS_COMMON -fno-common -Wno-unused-but-set-variable"

# The wlroots package on Debian trixie is literally named "wlroots-0.18".
WLROOTS_PKG="wlroots-${WLROOTS_ABI}"
[ "$NATIVE" -eq 1 ] && [ -z "${WLR_PKG:-}" ] && WLR_PKG="wlroots"
[ "$NATIVE" -eq 1 ] && [ -n "${WLR_PKG:-}" ] && WLROOTS_PKG="$WLR_PKG"

if [ "$NATIVE" -eq 1 ]; then
  [ -n "$(pkg-config --cflags "$WLROOTS_PKG" wayland-server 2>/dev/null || true)" ] \
    || die "host wlroots not found; install libwlroots-dev"
fi

# compile_component <name> <outdir> <sources> [extra flags] [pkg-config packages]
#
# The last argument exists because the stack is two different kinds of program.
# The compositor is a Wayland *server* and links wlroots plus libwayland-server.
# The shell and every app are Wayland *clients*: they call wl_display_connect()
# and the proxy marshalling code, none of which lives in libwayland-server.
# Linking a client against wayland-server therefore fails at link time with
# undefined references to wl_proxy_* - or, worse, succeeds and crashes on the
# first call. So the package list is a parameter, not a hardcoded constant.
compile_component() {
  local name="$1"; shift
  local outdir="$1"; shift
  local sources="$1"; shift
  local extra_flags="${1:-}"; shift || true
  local pkgs="${1:-$WLROOTS_PKG wayland-server}"
  local OUT_CHROOT="$outdir"
  [ "$NATIVE" -eq 0 ] && OUT_CHROOT="$(in_rootfs_path "$ROOTFS" "$outdir")"

  if [ "$NATIVE" -eq 1 ]; then
    local cflags libs
    cflags="$(pkg-config --cflags $pkgs)" || die "pkg-config --cflags $pkgs failed"
    libs="$(pkg-config --libs $pkgs)"   || die "pkg-config --libs $pkgs failed"
    ( cd "$BUILD_IN" && \
      gcc $CFLAGS_COMMON -I"$BUILD_IN/pg" -I"$BUILD_IN/gui/ui/include" \
        -I"$BUILD_IN/gui/shell/include" -I"$BUILD_IN/apps/common/include" \
        $cflags $extra_flags $sources $libs \
        -lm -lpthread -o "$outdir/$name" ) \
      || die "failed to build $name"
  else
    in_rootfs "$ROOTFS" /bin/bash -c "
      cd '$BUILD_IN_CHROOT' && export PKG_CONFIG_PATH=/usr/lib/${DEB_TRIPLET}/pkgconfig && \
      gcc $CFLAGS_COMMON -Ipg -Igui/ui/include -Iapps/common/include \
        -Igui/compositor/include -Igui/shell/include \
        \$(pkg-config --cflags $pkgs) $extra_flags \
        $sources \$(pkg-config --libs $pkgs) \
        -lm -lpthread -o '$OUT_CHROOT/$name'
    " || die "failed to build $name"
  fi
  ok "built $name"
}

log "Compiling Astrix UI library"
if [ "$NATIVE" -eq 1 ]; then
  gcc $CFLAGS_COMMON -I"$GUI_SRC/ui/include" -c "$GUI_SRC/ui/src/astrix_canvas.c" -o "$LIB_OUT/astrix_canvas.o"
  gcc $CFLAGS_COMMON -I"$GUI_SRC/ui/include" -c "$GUI_SRC/ui/src/astrix_font.c"   -o "$LIB_OUT/astrix_font.o"
else
  in_rootfs "$ROOTFS" /bin/bash -c "
    cd '$BUILD_IN_CHROOT' && gcc $CFLAGS_COMMON -Igui/ui/include -c gui/ui/src/astrix_canvas.c -o '$LIB_OUT_CHROOT/astrix_canvas.o' &&
    gcc $CFLAGS_COMMON -Igui/ui/include -c gui/ui/src/astrix_font.c -o '$LIB_OUT_CHROOT/astrix_font.o'
  " || die "failed to build libastrix-ui"
fi
ok "built libastrix-ui"

log "Compiling astrix-compositor"
compile_component astrix-compositor "$BIN_OUT" \
  "gui/compositor/src/main.c gui/compositor/src/input.c gui/compositor/src/shell.c pg/xdg-shell-protocol.c"

log "Compiling astrix-shell"
# The shell draws the entire system UI itself, so it links the UI toolkit
# directly rather than against a shared library. One less .so to resolve at
# boot, on a system where the first frame is what the user is waiting for.
# apps.c is the desktop-entry scanner that fills the launcher; without it the
# home screen grid is empty on a real device.
compile_component astrix-shell "$BIN_OUT" \
  "gui/shell/src/main.c gui/shell/src/gesture.c gui/shell/src/shell.c gui/shell/src/render.c gui/shell/src/input.c gui/shell/src/apps.c gui/shell/src/keyboard.c gui/ui/src/astrix_canvas.c gui/ui/src/astrix_font.c pg/xdg-shell-protocol.c pg/virtual-keyboard-unstable-v1-protocol.c" \
  "" "wayland-client xkbcommon"

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
# Every app is a Wayland client: wayland-client plus the generated xdg-shell
# bindings, and the shared UI toolkit. They are listed explicitly rather than
# globbed so that adding a file to apps/ without wiring it here is a build
# error, not a silently missing binary.
log "Compiling Astrix applications"
# One entry per line, fields separated by '|'.
#
# The separator matters. An earlier version used a colon and iterated with
# `for entry in $APPS`, which word-splits on whitespace: every line after the
# first app source was torn off and turned into its own bogus loop iteration,
# so gcc was handed only main.c per app and the link failed with undefined
# references to the app's own functions. Reading line by line and splitting on
# a delimiter that cannot appear in a path keeps each app's source list intact.
APPS=$(cat <<'EOF'
terminal|apps/terminal|main.c astrix_term.c astrix_kbd.c
files|apps/files|main.c astrix_files.c
sysinfo|apps/sysinfo|main.c
settings|apps/settings|main.c astrix_settings.c
package-manager|apps/package-manager|main.c
apk-manager|apps/apk-manager|main.c
EOF
)
APPS_SOURCES="apps/common/src/astrix_app.c gui/ui/src/astrix_canvas.c gui/ui/src/astrix_font.c"

while IFS='|' read -r app_name app_dir app_files; do
  [ -n "$app_name" ] || continue
  app_sources=""
  for f in $app_files; do
    app_sources="$app_sources $app_dir/src/$f"
  done
  compile_component "astrix-$app_name" "$BIN_OUT" \
    "$app_sources $APPS_SOURCES pg/xdg-shell-protocol.c" \
    "-Iapps/common/include -I$app_dir/include -lutil" \
    "wayland-client"
done <<< "$APPS"

# ---------------------------------------------------------------------------
# Desktop entries
# ---------------------------------------------------------------------------
# The launcher reads freedesktop.org desktop entries, so Astrix's own apps
# have to be described the same way any Debian package describes itself.
# Without this the home screen is empty on a real device even though every
# binary exists and every icon-grid test passes.
#
# They go into the image from the *rootfs*, not the staging directory, because
# the rootfs is what gets copied into the disk image. The check below is the
# point of doing it here: a binary that is built but has no desktop entry is
# invisible to the user, and "it compiles" is not the same as "it is
# installed".
DESKTOP_DIR="${ROOTFS}/usr/share/applications"
in_rootfs "$ROOTFS" mkdir -p "$DESKTOP_DIR"
desktop_count=0
for entry in apps/desktop/*.desktop; do
  [ -f "$entry" ] || continue
  install -D -m 0644 "$entry" "$DESKTOP_DIR/$(basename "$entry")"
  desktop_count=$((desktop_count + 1))
done
[ "$desktop_count" -gt 0 ] || die "no desktop entries found in apps/desktop/ - the launcher would be empty"
ok "desktop entries -> $DESKTOP_DIR ($desktop_count)"

# Every desktop entry must name a binary this build actually produced, and
# the entry must be valid enough to be parsed. A typo in Exec= is invisible
# until someone taps the icon, which is exactly the kind of bug that only
# shows up on a device.
while IFS='|' read -r app_name app_dir app_files; do
  [ -n "$app_name" ] || continue
  entry="$DESKTOP_DIR/astrix-$app_name.desktop"
  [ -f "$entry" ] || die "astrix-$app_name was built but has no desktop entry at $entry"
  grep -qx "Exec=astrix-$app_name" "$entry" ||
    die "$entry does not declare 'Exec=astrix-$app_name'; a launcher entry that does not match its binary can never be started"
  [ -x "$BIN_OUT/astrix-$app_name" ] || die "desktop entry for astrix-$app_name exists but the binary does not"
done <<< "$APPS"
ok "every built app has a matching, valid desktop entry"

ok "GUI build complete"
ok "binaries -> $BIN_OUT"
ls -la "$BIN_OUT"/astrix-* "$LIB_OUT"
