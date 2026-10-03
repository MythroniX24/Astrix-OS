#!/usr/bin/env bash
# Astrix OS - end-to-end smoke test of the graphical stack.
#
# Builds nothing and asserts nothing about hardware. It starts the real
# arm64 astrix-compositor on the wlroots headless backend inside the rootfs,
# starts the real astrix-shell as a Wayland client against it, and checks that
# the shell actually maps a toplevel that the compositor accepts.
#
# Why "headless" is meaningful here: the headless backend still runs the real
# wlroots backend, renderer, scene graph, seat and protocol code. Only the DRM
# device and libinput hardware are replaced. So a pass here means the server
# and client agree on xdg-shell and the shell reaches the compositor - which is
# exactly the class of bug (wrong link library, missing map, no scene node)
# that unit tests on the host cannot catch.
#
# It does NOT prove anything about a real DRM device, a real touchscreen, or
# physical hardware. See docs/STATUS.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../scripts/lib.sh
source "${SCRIPT_DIR}/../scripts/lib.sh"
trap 'on_error' ERR

astrix_init_paths

ROOTFS="${ASTRIX_BUILD_DIR}/rootfs"
COMPOSITOR="$ROOTFS/usr/bin/astrix-compositor"
SHELL_BIN="$ROOTFS/usr/bin/astrix-shell"

[ -d "$ROOTFS" ] || die "no rootfs at $ROOTFS - run ./scripts/build-rootfs.sh first"
[ -x "$COMPOSITOR" ] || die "$COMPOSITOR missing - run ./scripts/build-gui.sh"
[ -x "$SHELL_BIN" ] || die "$SHELL_BIN missing - run ./scripts/build-gui.sh"

WIDTH="${ASTRIX_TEST_WIDTH:-720}"
HEIGHT="${ASTRIX_TEST_HEIGHT:-1600}"

# The compositor creates its socket in XDG_RUNTIME_DIR. A real session has
# logind provide that; here we make a private one so the test cannot collide
# with a live session or leave a socket behind.
RUN_DIR_CHROOT=/tmp/astrix-smoke
LOG_DIR_CHROOT=/tmp/astrix-smoke-logs

# Reports a failure with the logs attached, because "the shell did not appear"
# on its own is not a diagnosis.
dump_logs() {
	echo "--- compositor log ---"; cat "$ROOTFS$LOG_DIR_CHROOT/compositor.log" 2>/dev/null || echo "(none)"
	echo "--- shell log ---";      cat "$ROOTFS$LOG_DIR_CHROOT/shell.log"      2>/dev/null || echo "(none)"
}

fail() { echo "FAIL: $*" >&2; dump_logs >&2; exit 1; }

mount_rootfs "$ROOTFS"

# Clean any previous run's leftovers, then reset to a known-empty state.
in_rootfs "$ROOTFS" /bin/bash -c "rm -rf '$RUN_DIR_CHROOT' '$LOG_DIR_CHROOT'"
in_rootfs "$ROOTFS" /bin/bash -c "mkdir -p '$RUN_DIR_CHROOT' '$LOG_DIR_CHROOT' && chmod 700 '$RUN_DIR_CHROOT'"

cleanup() {
	# Kill by the pid files we wrote, not by pattern, so a real user session
	# is never targeted.
	for f in shell compositor; do
		if [ -f "$ROOTFS$LOG_DIR_CHROOT/$f.pid" ]; then
			kill "$(cat "$ROOTFS$LOG_DIR_CHROOT/$f.pid")" 2>/dev/null || true
		fi
	done
	sleep 0.3
	for f in shell compositor; do
		if [ -f "$ROOTFS$LOG_DIR_CHROOT/$f.pid" ]; then
			kill -9 "$(cat "$ROOTFS$LOG_DIR_CHROOT/$f.pid")" 2>/dev/null || true
		fi
	done
	umount_rootfs "$ROOTFS" || true
}
trap cleanup EXIT

# Start the compositor. It has no readiness signal other than the socket
# appearing, so wait for that rather than sleeping a fixed amount of time.
#
# The compositor binds a fixed name (ASTRIX_SOCKET, default astrix-0) instead
# of wl_display_add_socket_auto(), so the test asserts on that exact name. If
# the compositor ever goes back to auto-naming, this assertion is what will
# fail - which is the point, because the shell service hardcodes the same name.
socket_name="astrix-0"
in_rootfs "$ROOTFS" /bin/bash -c "
	cd /tmp
	XDG_RUNTIME_DIR='$RUN_DIR_CHROOT' ASTRIX_HEADLESS=1 WLR_RENDERER=pixman \
	  nohup astrix-compositor --headless --width $WIDTH --height $HEIGHT \
	  >'$LOG_DIR_CHROOT/compositor.log' 2>&1 &
	echo \$! > '$LOG_DIR_CHROOT/compositor.pid'
"

for _ in $(seq 1 100); do
	[ -S "$ROOTFS$RUN_DIR_CHROOT/$socket_name" ] && break
	sleep 0.2
done
[ -S "$ROOTFS$RUN_DIR_CHROOT/$socket_name" ] \
	|| fail "compositor did not create the Wayland socket '$socket_name' within 20s"
pass "compositor started and created Wayland socket '$socket_name'"

# The compositor must be genuinely alive, not a zombie that made a socket
# during startup and then died.
kill -0 "$(cat "$ROOTFS$LOG_DIR_CHROOT/compositor.pid")" 2>/dev/null \
	|| fail "compositor exited right after creating its socket"

in_rootfs "$ROOTFS" /bin/bash -c "
	cd /tmp
	XDG_RUNTIME_DIR='$RUN_DIR_CHROOT' WAYLAND_DISPLAY='$socket_name' \
	  nohup astrix-shell >'$LOG_DIR_CHROOT/shell.log' 2>&1 &
	echo \$! > '$LOG_DIR_CHROOT/shell.pid'
"

# The compositor logs one line per accepted xdg_toplevel and one when the
# shell's surface maps. Either proves the client/server pair negotiated
# xdg-shell; both together prove the shell is being treated as the shell.
registered=0
mapped=0
for _ in $(seq 1 100); do
	grep -q "new app registered" "$ROOTFS$LOG_DIR_CHROOT/compositor.log" 2>/dev/null && registered=1
	grep -q "Shell surface mapped" "$ROOTFS$LOG_DIR_CHROOT/compositor.log" 2>/dev/null && mapped=1
	[ "$registered" -eq 1 ] && [ "$mapped" -eq 1 ] && break
	# A dead shell will never map; fail fast instead of waiting out the loop.
	if ! kill -0 "$(cat "$ROOTFS$LOG_DIR_CHROOT/shell.pid")" 2>/dev/null; then
		fail "shell exited before mapping a toplevel"
	fi
	sleep 0.2
done

[ "$registered" -eq 1 ] || fail "compositor never registered the shell's toplevel within 20s"
pass "compositor accepted the shell's xdg_toplevel"

[ "$mapped" -eq 1 ] || fail "shell surface never mapped within 20s"
pass "shell surface mapped"

kill -0 "$(cat "$ROOTFS$LOG_DIR_CHROOT/shell.pid")" 2>/dev/null \
	|| fail "shell exited immediately after mapping"
pass "shell stayed running"

# The compositor must have identified the surface as the shell by its app_id,
# not merely accepted an anonymous window. "Astrix Shell surface mapped" is
# emitted only from the branch that matches app_id against ASTRIX_SHELL_APP_ID,
# so its presence is the identity check - and it is already required above, so
# here we only guard against a second, unrecognised toplevel sneaking in.
if [ "$(grep -c 'new app registered' "$ROOTFS$LOG_DIR_CHROOT/compositor.log" 2>/dev/null || echo 0)" -gt 1 ]; then
	fail "more than one toplevel registered; expected only the shell"
fi
pass "shell was the only client toplevel, and was identified by app_id"

# Protocol errors are the failure mode a bare "did it start" check walks
# straight past: libwayland logs them and the process keeps running.
if grep -q "protocol error" "$ROOTFS$LOG_DIR_CHROOT/compositor.log" "$ROOTFS$LOG_DIR_CHROOT/shell.log" 2>/dev/null; then
  fail "a Wayland protocol error was logged"
fi
pass "no Wayland protocol errors logged"

# The virtual keyboard global has to exist, or the on-screen keyboard draws,
# queues keys and delivers nothing - a keyboard that looks perfect and types
# nothing. That exact failure shipped once: the compositor never advertised
# zwp_virtual_keyboard_manager_v1 because wlroots only advertises it from its
# Wayland backend, and this compositor deliberately does not use one.
if ! grep -q "virtual keyboard manager ready" "$ROOTFS$LOG_DIR_CHROOT/compositor.log" 2>/dev/null; then
  fail "the compositor did not advertise a virtual keyboard manager"
fi
pass "the compositor advertises a virtual keyboard manager"
if ! grep -q "virtual keyboard ready" "$ROOTFS$LOG_DIR_CHROOT/shell.log" 2>/dev/null; then
  fail "the shell did not obtain a virtual keyboard; on-screen keys cannot be delivered"
fi
pass "the shell obtained a virtual keyboard and sent it a keymap"
#
# The compositor has to *subscribe* to client-created virtual keyboards.
# wlroots does not register one with any backend, so it never arrives through
# the backend's new_input signal; the compositor has to watch the manager's
# new_virtual_keyboard signal instead. Missing that second half is what made
# the on-screen keyboard draw, queue keys and type nothing at all.
if ! grep -q "virtual keyboard created by a client" "$ROOTFS$LOG_DIR_CHROOT/compositor.log" 2>/dev/null; then
  fail "the compositor did not attach the shell's virtual keyboard; its keys go nowhere"
fi
pass "the compositor attached the client's virtual keyboard"

# ---------------------------------------------------------------------------
# The whole point of the keyboard is that a key reaches an application. The
# shell can type into itself (it is the focused client), so with the self-test
# enabled the full path runs unattended: tap a key -> queue -> xkb keycode ->
# compositor -> the focused client's wl_keyboard. If any link in that chain is
# missing, the keys go nowhere and this fails.
# ---------------------------------------------------------------------------
in_rootfs "$ROOTFS" /bin/bash -c "
	cd /tmp
	rm -f '$LOG_DIR_CHROOT/shell-selftest.log'
	XDG_RUNTIME_DIR='$RUN_DIR_CHROOT' WAYLAND_DISPLAY='$socket_name' \
	  ASTRIX_SELF_TEST_KEYBOARD=1 \
	  nohup astrix-shell >'$LOG_DIR_CHROOT/shell-selftest.log' 2>&1 &
	echo \$! > '$LOG_DIR_CHROOT/shell-selftest.pid'
"
SELFLOG="$ROOTFS$LOG_DIR_CHROOT/shell-selftest.log"
queued=0
for _ in $(seq 1 100); do
  grep -q "on-screen key" "$SELFLOG" 2>/dev/null && queued=$(grep -c "on-screen key" "$SELFLOG")
  grep -q "key .* pressed" "$SELFLOG" 2>/dev/null && break
  if ! kill -0 "$(cat "$ROOTFS$LOG_DIR_CHROOT/shell-selftest.pid")" 2>/dev/null; then
    break
  fi
  sleep 0.2
done
if ! grep -q "self-test: queued 3 key(s)" "$SELFLOG" 2>/dev/null; then
  fail "the on-screen keyboard did not turn three taps into three keys"
fi
pass "three taps on the keyboard became three queued keys"
if [ "${queued:-0}" -lt 3 ]; then
  fail "only ${queued:-0}/3 keys were sent to the compositor"
fi
pass "all three keys were injected through the virtual keyboard"
#
# Delivery depends on the seat having a keyboard to deliver with, and it now
# always does: wlroots needs no hardware keyboard to forward a key, it needs a
# keyboard object on the seat, and a client-created virtual keyboard becomes
# one (the compositor hands it to the seat when nothing else holds it). So
# this is asserted unconditionally here - on the headless backend as well as
# on a real session - instead of being skipped when no keyboard is attached.
# A keyboard that cannot be shown to deliver a key is a keyboard nobody may
# claim works.
if ! grep -q "keyboard added\|virtual keyboard created by a client" "$ROOTFS$LOG_DIR_CHROOT/compositor.log" 2>/dev/null; then
  fail "the seat has no keyboard at all, so injected keys cannot be delivered"
fi
if ! grep -q "keyboard focus entered" "$SELFLOG" 2>/dev/null; then
  fail "the focused client never received wl_keyboard.enter; the seat has no focus for it"
fi
pass "the focused client was given wl_keyboard.enter"
if ! grep -q "key .* pressed" "$SELFLOG" 2>/dev/null; then
  fail "no key came back through wl_keyboard; the injection path is broken"
fi
pass "the compositor delivered the injected keys back to the focused client"

ok "Wayland session smoke test passed"
