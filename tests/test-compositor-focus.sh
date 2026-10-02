#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# The compositor's focus teardown must not derive the server from a toplevel
# that is allowed to be NULL.
#
# Why this test exists
# -------------------
# set_focused_toplevel() is called with NULL to clear focus. That happens on
# every path where an app gives up the foreground: it closed, it asked to be
# minimised, the shell lost the front of the screen. Those are exactly the
# paths that run while wl_client_destroy() is tearing the client down.
#
# The function used to open with
#
#     struct astrix_server *server = toplevel->server;
#
# and then branch on `if (toplevel)` a few lines later - so NULL was a
# documented, intended input that the first line dereferenced anyway. It was
# not a corrupted-free or use-after-free: a plain NULL dereference, taken
# every single time an app exited, taking the whole compositor with it:
#
#   astrix-compositor[745]: fatal signal, backtrace follows:
#   astrix-compositor(+0x6254)      set_focused_toplevel, gui/compositor/src/shell.c:147
#   wl_signal_emit_mutable+0x90
#   wlr_surface_unmap+0x40          our unmap handler
#   wl_client_destroy+0x90
#   sig=11
#
# addr2line on the guest binary resolved +0x6254 to that exact line, which is
# what made this diagnosable at all: the crash was inside our own compositor,
# not in wlroots, and the frame said which function.
#
# Why a source check and not a runtime test
# -----------------------------------------
# Reproducing it needs a client that maps a toplevel and then exits, driven
# against a real compositor - a VM boot. The mistake is a one-line shape
# mistake in a signature, so it is checked in seconds here instead of after a
# twelve-minute boot. tests/test-wayland-session.sh remains the test that
# proves the session as a whole still comes up.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../scripts/lib.sh
. "$REPO_ROOT/scripts/lib.sh"

# ASTRIX_FOCUS_SRC points the check at a different file. It exists so the
# guard itself can be tested: tests/fixtures/focus-pre-fix.c is the pre-fix
# source, and running this test against it must fail. A check that has only
# ever seen passing code is not evidence of anything.
#
#   ASTRIX_FOCUS_SRC=tests/fixtures/focus-pre-fix.c \
#     bash tests/test-compositor-focus.sh   # must exit non-zero
SHELL_SRC="${ASTRIX_FOCUS_SRC:-$REPO_ROOT/gui/compositor/src/shell.c}"

if [ ! -f "$SHELL_SRC" ]; then
  warn "no compositor shell.c at $SHELL_SRC; skipping the focus-teardown check"
  exit 0
fi

failed=0

# 1. The signature must take the server. If it does not, the server is being
#    derived from the toplevel, and the first use of it crashes on NULL.
if grep -q 'static void set_focused_toplevel(struct astrix_server \*server,' "$SHELL_SRC"; then
  pass "set_focused_toplevel() takes the server as a parameter"
else
  err "set_focused_toplevel() does not take the server as a parameter"
  info "  deriving it from toplevel->server dereferences NULL on every"
  info "  focus-clearing path, which runs during wl_client_destroy (bug 33)"
  failed=$((failed + 1))
fi

# 2. Nothing inside the function may read a field through the toplevel before
#    the NULL check. Scoped to the function body so unrelated code is not
#    flagged; the body ends at the first line that is exactly "}".
body="$(awk '
  /static void set_focused_toplevel\(/ { inside = 1 }
  inside { print }
  inside && /^\}$/ { exit }
' "$SHELL_SRC")"

if printf '%s' "$body" | grep -q 'toplevel->'; then
  # Every use must sit inside the `if (toplevel)` guard.
  guarded="$(printf '%s\n' "$body" | awk '
    /if \(toplevel\)/ { guarded = 1 }
    guarded && /toplevel->/ { print }
  ')"
  total="$(printf '%s\n' "$body" | grep -c 'toplevel->' || true)"
  if [ "$(printf '%s\n' "$guarded" | grep -c 'toplevel->' || true)" = "$total" ]; then
    pass "every toplevel-> use in set_focused_toplevel() is inside if (toplevel)"
  else
    err "set_focused_toplevel() reads toplevel-> outside the NULL guard"
    failed=$((failed + 1))
  fi
else
  pass "set_focused_toplevel() never dereferences toplevel before the guard"
fi

# 3. Every call site must pass a server. A one-argument call is the old,
#    broken signature wearing a new name.
if grep -nE 'set_focused_toplevel\((NULL|t)\);' "$SHELL_SRC" >/dev/null 2>&1; then
  err "set_focused_toplevel() is called without a server argument:"
  grep -nE 'set_focused_toplevel\((NULL|t)\);' "$SHELL_SRC" | sed 's/^/    /'
  failed=$((failed + 1))
else
  n="$(grep -c 'set_focused_toplevel(' "$SHELL_SRC" || true)"
  pass "all ${n} set_focused_toplevel() references pass a server"
fi

# 4. The NULL call must still exist. A refactor that removed the "go home"
#    path would satisfy checks 1-3 while quietly breaking focus clearing.
if grep -q 'set_focused_toplevel(t->server, NULL)' "$SHELL_SRC"; then
  pass "focus can still be cleared (set_focused_toplevel(..., NULL))"
else
  err "no call clears focus; apps could never give up the foreground"
  failed=$((failed + 1))
fi

# 5. Prove the guard still detects the bug, by running it against the pre-fix
#    source. Guarded on ASTRIX_FOCUS_SRC being unset so this does not recurse.
if [ -z "${ASTRIX_FOCUS_SRC:-}" ] && [ -f "$REPO_ROOT/tests/fixtures/focus-pre-fix.c" ]; then
  if ASTRIX_FOCUS_SRC="$REPO_ROOT/tests/fixtures/focus-pre-fix.c" \
       bash "$0" >/dev/null 2>&1; then
    err "the guard passes against the pre-fix fixture, so it detects nothing"
    info "  either the checks are too loose or the fixture was 'fixed' by mistake"
    failed=$((failed + 1))
  else
    pass "the guard fails against the pre-fix fixture (it detects the bug)"
  fi
fi

if [ "$failed" -ne 0 ]; then
  die "$failed focus-teardown check(s) failed"
fi
ok "compositor focus teardown survives a client disappearing"