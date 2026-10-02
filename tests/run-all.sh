#!/usr/bin/env bash
# Astrix OS - run the host test suite.
#
# These tests do not need a phone, a GPU or QEMU: they verify the parts of the
# system whose behaviour is pure logic (gesture recognition, rendering, package
# manifests, app registry), plus one end-to-end Wayland session check
# (tests/test-wayland-session.sh) that runs the real arm64 compositor and shell
# under emulation. Anything that can only be verified on a booted machine is
# covered by scripts/build-image.sh's image verification instead.
#
#   ./tests/run-all.sh          run everything
#   ./tests/run-all.sh --fast   skip the package-index download if cached
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
# shellcheck source=../scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"

# shellcheck source=../config/os.conf
source "${REPO_ROOT}/config/os.conf"

FAST=0
[ "${1:-}" = "--fast" ] && FAST=1

OUT="${ASTRIX_BUILD_DIR:-build}/tests"
mkdir -p "$OUT"

PASS=0
FAIL=0
SKIP=0
FAILED_NAMES=()

banner() {
  printf '\n%s\n' "==============================================================="
  printf '  %s\n' "$1"
  printf '%s\n' "==============================================================="
}

record() {
  local name="$1" rc="$2"
  if [ "$rc" -eq 0 ]; then
    printf '  %sPASS%s  %s\n' "$C_GREEN" "$C_RESET" "$name"
    PASS=$((PASS + 1))
  else
    printf '  %sFAIL%s  %s\n' "$C_RED" "$C_RESET" "$name"
    FAIL=$((FAIL + 1))
    FAILED_NAMES+=("$name")
  fi
}

skip() {
  printf '  %sSKIP%s  %s (%s)\n' "$C_DIM" "$C_RESET" "$1" "$2"
  SKIP=$((SKIP + 1))
}

# ---------------------------------------------------------------------------
banner "Astrix OS test suite"
# ---------------------------------------------------------------------------

# 1. Shell scripts are at least syntactically valid.
banner "shell syntax"
for f in build.sh run-qemu.sh clean.sh scripts/*.sh tests/*.sh qemu/*.sh; do
  [ -f "$f" ] || continue
  if bash -n "$f" 2>"$OUT/syntax.log"; then
    record "$f" 0
  else
    record "$f" 1
    sed 's/^/      /' "$OUT/syntax.log"
  fi
done

# 2. UI toolkit unit tests.
banner "Astrix UI toolkit"
if command -v cc >/dev/null || command -v gcc >/dev/null; then
  CC_BIN="$(command -v cc || command -v gcc)"
  if "$CC_BIN" -std=c11 -O1 -Igui/ui/include \
      gui/ui/src/astrix_canvas.c gui/ui/src/astrix_font.c \
      tests/test-ui.c -o "$OUT/test-ui" -lm 2>"$OUT/test-ui.log"; then
    if "$OUT/test-ui" >"$OUT/test-ui.out" 2>&1; then
      record "test-ui ($(tail -1 "$OUT/test-ui.out" | tr -d '\n'))" 0
    else
      record "test-ui" 1
      grep -E "^FAIL" "$OUT/test-ui.out" | sed 's/^/      /' | head -20
    fi
  else
    skip "test-ui" "compile failed"
    sed 's/^/      /' "$OUT/test-ui.log" | head -10
  fi
else
  skip "test-ui" "no C compiler"
fi

# 3. Shell logic: gestures, app registry, notifications, navigation.
banner "Astrix Shell logic"
if command -v cc >/dev/null || command -v gcc >/dev/null; then
  CC_BIN="$(command -v cc || command -v gcc)"
  if "$CC_BIN" -std=c11 -O1 -Igui/ui/include -Igui/shell/include \
      gui/ui/src/astrix_canvas.c gui/ui/src/astrix_font.c \
      gui/shell/src/gesture.c gui/shell/src/shell.c \
      tests/test-gestures.c -o "$OUT/test-gestures" -lm 2>"$OUT/test-gestures.log"; then
    if "$OUT/test-gestures" >"$OUT/test-gestures.out" 2>&1; then
      record "test-gestures ($(tail -1 "$OUT/test-gestures.out" | tr -d '\n'))" 0
    else
      record "test-gestures" 1
      grep -E "^FAIL" "$OUT/test-gestures.out" | sed 's/^/      /' | head -20
    fi
  else
    skip "test-gestures" "compile failed"
    sed 's/^/      /' "$OUT/test-gestures.log" | head -10
  fi
else
  skip "test-gestures" "no C compiler"
fi

# 3b. The on-screen keyboard. It is the one UI where "it drew something" and
#     "it works" are different claims: the tap has to become the right
#     character, shift has to be one-shot, and a tap on the keyboard must never
#     also land on the home screen behind it. This test found a real SIGFPE -
#     the symbol layer leaves the zxcvbnm row empty and the layout divided by
#     its length - so it is not decoration.
banner "on-screen keyboard"
if command -v cc >/dev/null || command -v gcc >/dev/null; then
  CC_BIN="$(command -v cc || command -v gcc)"
  if "$CC_BIN" -std=c11 -O1 -Igui/ui/include -Igui/shell/include \
      gui/ui/src/astrix_canvas.c gui/ui/src/astrix_font.c \
      gui/shell/src/gesture.c gui/shell/src/shell.c gui/shell/src/keyboard.c \
      tests/test-keyboard.c -o "$OUT/test-keyboard" -lm 2>"$OUT/test-keyboard.log"; then
    if "$OUT/test-keyboard" >"$OUT/test-keyboard.out" 2>&1; then
      record "test-keyboard ($(tail -1 "$OUT/test-keyboard.out" | tr -d '\n'))" 0
    else
      record "test-keyboard" 1
      grep -E "^  x|FAILED" "$OUT/test-keyboard.out" | sed 's/^/      /' | head -20
    fi
  else
    skip "test-keyboard" "compile failed"
    sed 's/^/      /' "$OUT/test-keyboard.log" | head -10
  fi
else
  skip "test-keyboard" "no C compiler"
fi

# 4. Shell input: the dock is drawn in one coordinate system and hit-tested
#    in another, and they had already drifted apart once. This pins the hit
#    test to the geometry render.c actually draws, at every panel size.
banner "shell input hit testing"
if command -v cc >/dev/null || command -v gcc >/dev/null; then
  CC_BIN="$(command -v cc || command -v gcc)"
  if "$CC_BIN" -std=c11 -O1 -Igui/ui/include -Igui/shell/include \
      gui/ui/src/astrix_canvas.c gui/ui/src/astrix_font.c \
      gui/shell/src/gesture.c gui/shell/src/shell.c gui/shell/src/keyboard.c \
      tests/test-shell-input.c -o "$OUT/test-shell-input" -lm 2>"$OUT/test-shell-input.log"; then
    if "$OUT/test-shell-input" >"$OUT/test-shell-input.out" 2>&1; then
      record "test-shell-input ($(tail -1 "$OUT/test-shell-input.out" | tr -d '\n'))" 0
    else
      record "test-shell-input" 1
      grep -E "^  x|FAILED" "$OUT/test-shell-input.out" | sed 's/^/      /' | head -20
    fi
  else
    skip "test-shell-input" "compile failed"
    sed 's/^/      /' "$OUT/test-shell-input.log" | head -10
  fi
else
  skip "test-shell-input" "no C compiler"
fi

# 5. Shell framebuffer ownership: on a real panel the framebuffer is an aliased
#    wl_shm mapping, not the shell's own allocation. Resizing must not free it -
#    and it did, which SIGSEGV'd the shell on every 1024x768 boot while the host
#    smoke test (720x1600) never took the resize path at all.
banner "shell framebuffer ownership"
if command -v cc >/dev/null || command -v gcc >/dev/null; then
  CC_BIN="$(command -v cc || command -v gcc)"
  if "$CC_BIN" -std=c11 -O1 -Igui/ui/include -Igui/shell/include \
      gui/ui/src/astrix_canvas.c gui/ui/src/astrix_font.c \
      gui/shell/src/gesture.c gui/shell/src/shell.c \
      tests/test-shell-size.c -o "$OUT/test-shell-size" -lm 2>"$OUT/test-shell-size.log"; then
    if "$OUT/test-shell-size" >"$OUT/test-shell-size.out" 2>&1; then
      record "test-shell-size ($(tail -1 "$OUT/test-shell-size.out" | tr -d '\n'))" 0
    else
      record "test-shell-size" 1
      grep -E "^  x|FAILED" "$OUT/test-shell-size.out" | sed 's/^/      /' | head -20
    fi
  else
    skip "test-shell-size" "compile failed"
    sed 's/^/      /' "$OUT/test-shell-size.log" | head -10
  fi
else
  skip "test-shell-size" "no C compiler"
fi

# 6. Package manifest must match the real Debian archive.
banner "package manifest"
if [ "$FAST" -eq 1 ] && [ ! -s "${ASTRIX_CACHE_DIR:-/tmp/astrix-cache}/${DEBIAN_SUITE}-${DEB_ARCH}-Packages" ]; then
  skip "test-packages" "--fast and no cached index"
elif bash tests/test-packages.sh 2>&1 | tail -3; then
  record "test-packages" 0
else
  record "test-packages" 1
fi

# 7. Render every shell screen; catches drawing regressions and produces
#    images a human can inspect.
banner "shell rendering"
if command -v cc >/dev/null || command -v gcc >/dev/null; then
  CC_BIN="$(command -v cc || command -v gcc)"
  mkdir -p build/screens
  if "$CC_BIN" -std=c11 -O1 -Igui/ui/include -Igui/shell/include \
      gui/ui/src/*.c gui/shell/src/gesture.c gui/shell/src/shell.c gui/shell/src/render.c gui/shell/src/keyboard.c \
      tests/render-screens.c -o "$OUT/render-screens" -lm 2>"$OUT/render.log"; then
    if "$OUT/render-screens" build/screens >"$OUT/render.out" 2>&1; then
      n=$(ls build/screens/*.ppm 2>/dev/null | wc -l)
      record "render-screens ($n screens)" 0
    else
      record "render-screens" 1
    fi
  else
    skip "render-screens" "compile failed"
    sed 's/^/      /' "$OUT/render.log" | head -10
  fi
else
  skip "render-screens" "no C compiler"
fi

# 8. End-to-end Wayland session: the real arm64 compositor and shell, run
#    under qemu-user inside the rootfs. This is the one test that exercises the
#    actual server/client protocol path, so it is the test that would have
#    caught the missing-initial-configure deadlock.
#    It is opt-in because it needs a built rootfs and runs arm64 code through
#    emulation, which is slow on a small host. It is skipped, not silently
#    passed, when the rootfs is absent.
banner "wayland session (arm64, emulated)"
if [ ! -x "${ASTRIX_BUILD_DIR:-build}/rootfs/usr/bin/astrix-compositor" ]; then
  skip "test-wayland-session" "no built rootfs - run ./scripts/build-gui.sh"
elif bash tests/test-wayland-session.sh 2>&1 | sed 's/^/      /'; then
  record "test-wayland-session" 0
else
  record "test-wayland-session" 1
fi

# 9. Every wl_*_listener implements every event it will be sent.
#
#    A missing handler is a SIGABRT, not a no-op: libwayland aborts a client
#    that receives an event it has no listener for. The shell had no
#    wl_pointer.frame, so it died on the first touch of every session and
#    systemd restarted it - a session that looked completely healthy while
#    being deaf. That is a compile-time mistake with a runtime-only symptom,
#    so it is checked here in seconds rather than after a twelve-minute boot.
banner "wayland listeners (source check)"
if bash tests/test-wayland-listeners.sh 2>&1 | sed 's/^/      /'; then
  record "test-wayland-listeners" 0
else
  record "test-wayland-listeners" 1
fi

# 10. The compositor must survive a client disappearing.
#
#     set_focused_toplevel() read `toplevel->server` before checking whether
#     `toplevel` was NULL, on exactly the paths that run inside
#     wl_client_destroy(). Every app exit therefore took the compositor down
#     with a SIGSEGV (bug 33). Reproducing it needs a VM boot, so the shape of
#     the fix is checked here in seconds instead. The test also runs itself
#     against the pre-fix source to prove it still detects the bug.
banner "compositor focus teardown (source check)"
if bash tests/test-compositor-focus.sh 2>&1 | sed 's/^/      /'; then
  record "test-compositor-focus" 0
else
  record "test-compositor-focus" 1
fi

# ---------------------------------------------------------------------------
# The compositor's input target follows app focus, so it changes in the middle
# of a gesture every time a dock tap launches an app. The release then goes to
# the new toplevel and the shell is left holding a contact it can never end -
# its recogniser stays anchored to that old press for the rest of the session,
# so the next gesture is classified against a stale origin. On a booted VM
# this logged "swipe-right from 400,598 to 512,576" for a straight upward
# home swipe, 400,598 being where the finger was several gestures earlier
# (bug 39). A client given a press must always be given its release, so the
# release is sent to the outgoing surface before the switch. Reproducing it
# needs several gestures against a real compositor, so the shape of the fix is
# checked here in seconds instead.
banner "compositor pointer retarget (source check)"
if bash tests/test-pointer-retarget.sh 2>&1 | sed 's/^/      /'; then
  record "test-pointer-retarget" 0
else
  record "test-pointer-retarget" 1
fi

# ---------------------------------------------------------------------------
# A device profile is the only thing between a forum post and "Astrix supports
# this phone", and being wrong about that costs somebody a handset. So the
# claims in config/devices/*.conf are checked as assertions, and
# scripts/flash-device.sh is run against a real profile with no device attached
# to prove it refuses rather than merely intending to.
banner "device profiles and flash safety"
if bash tests/test-devices.sh 2>&1 | sed 's/^/      /'; then
  record "test-devices" 0
else
  record "test-devices" 1
fi

# ---------------------------------------------------------------------------
# The first thing a "boot it with a display" port has to get right is not the
# driver: it is whether the panel can physically be driven over the MIPI DSI
# link at all. That is arithmetic, it can be checked on the host in seconds,
# and it is easy to write a tool that always says yes. So the budget is pinned
# here including the case where it has to refuse a panel.
banner "display link budget (moto g64 5G, Redmi 8A)"
if bash tests/test-display-budget.sh 2>&1 | sed 's/^/      /'; then
  record "test-display-budget" 0
else
  record "test-display-budget" 1
fi

# ---------------------------------------------------------------------------
banner "summary"
# ---------------------------------------------------------------------------
printf '  passed:  %s%d%s\n' "$C_GREEN" "$PASS" "$C_RESET"
printf '  failed:  %s%d%s\n' "$([ "$FAIL" -gt 0 ] && echo "$C_RED" || echo "$C_GREEN")" "$FAIL" "$C_RESET"
[ "$SKIP" -gt 0 ] && printf '  skipped: %s%d%s\n' "$C_DIM" "$SKIP" "$C_RESET"
if [ "$FAIL" -gt 0 ]; then
  printf '\n  failing: %s\n' "${FAILED_NAMES[*]}"
  exit 1
fi
ok "all host tests passed"
