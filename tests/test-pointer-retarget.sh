#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Changing the pointer's input target must not strand a client mid-gesture.
#
# Why this test exists
# -------------------
# The input target follows app focus, so the target changes *in the middle of
# a gesture* every time a dock tap launches an app:
#
#   shell gets the press -> the new toplevel is focused -> the release is
#   delivered to the toplevel instead
#
# The shell is left holding a contact it can never end. Its gesture
# recogniser stays anchored to that old press for the rest of the session, and
# nothing is logged as wrong at the time. The visible symptom only shows up
# several gestures later, and it is baffling: a straight upward home swipe
# logged
#
#   astrix-shell: swipe-right from 400,598 to 512,576 on app
#
# where 400,598 is where the finger was when it launched an app, several
# gestures earlier. The switcher never opened, and every component insisted
# it had done its job.
#
# The fix is release_held_buttons(): a client that is given a press must
# always be given its release, so the release is sent to the outgoing surface
# *before* the switch (wlroots delivers a release to whichever surface has
# focus, so the order release -> leave -> enter is the only one that works).
#
# Why a source check and not a runtime test
# -----------------------------------------
# Reproducing it needs a real gesture that launches an app and then a second
# gesture, driven against a real compositor - a VM boot, and the diagnosis
# itself took three of them. The mistake is a missing call in two specific
# places, so it is checked in seconds here instead. What it cannot prove is
# that wlroots behaves as described; that is covered by
# tests/test-wayland-session.sh plus the boot log evidence in docs/STATUS.md.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
# shellcheck source=../scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"

INPUT_SRC="${ASTRIX_RETARGET_SRC:-gui/compositor/src/input.c}"
FAILED=0

if [ ! -f "$INPUT_SRC" ]; then
  die "not found: $INPUT_SRC"
fi

# Body of a single C function definition, from its opening brace to the
# matching close. Signatures here wrap onto a second line, and a forward
# declaration is indistinguishable from a definition on its first line, so
# this keys off the line that actually opens a body and looks back a few
# lines for the name - a declaration never opens a body.
function_body() {
  awk -v fn="$1" '
    {
      buf[NR % 4] = $0
    }
    !body && /\{[[:space:]]*$/ {
      hit = 0
      for (i = NR - 3; i <= NR; i++) {
        if (i > 0 && index(buf[i % 4], fn "(") > 0) hit = 1
      }
      if (!hit) next
      body = 1
      depth = 0
    }
    body {
      print
      line = $0
      depth += gsub(/\{/, "{", line) - gsub(/\}/, "}", line)
      if (depth <= 0) exit
    }
  ' "$INPUT_SRC"
}

# 1. The release helper exists at all.
if grep -q 'static void release_held_buttons(' "$INPUT_SRC"; then
  pass "release_held_buttons() exists"
else
  err "release_held_buttons() is gone; a retarget can strand a client mid-gesture"
  FAILED=$((FAILED + 1))
fi

# 2. It is a no-op unless a button is genuinely held and a surface has focus.
body="$(function_body release_held_buttons)"
if printf '%s' "$body" | grep -q 'button_count == 0'; then
  pass "it does nothing when no button is held"
else
  err "no button_count guard: this would send stray releases on every retarget"
  FAILED=$((FAILED + 1))
fi
if printf '%s' "$body" | grep -q 'focused_surface'; then
  pass "it refuses to release when nothing has focus"
else
  err "no focused_surface guard: wlroots would drop the release anyway"
  FAILED=$((FAILED + 1))
fi
if printf '%s' "$body" | grep -q 'WL_POINTER_BUTTON_STATE_RELEASED'; then
  pass "it sends a wl_pointer release"
else
  err "no release state sent; the contact would never end"
  FAILED=$((FAILED + 1))
fi
# The button list is copied before being drained: notifying mutates the very
# array being iterated, so releasing in place would skip or repeat buttons.
if printf '%s' "$body" | grep -q 'buttons\[i\] = state->buttons\[i\]'; then
  pass "the held button list is copied before it is drained"
else
  err "releases are sent straight out of seat->pointer_state.buttons, which notify mutates"
  FAILED=$((FAILED + 1))
fi

# 3. The two places that move the target release first. Order matters: the
#    release has to reach the *outgoing* surface, so it must precede both the
#    clear_focus and the enter.
body="$(function_body maybe_claim_system_gesture)"
if printf '%s' "$body" | grep -q 'release_held_buttons(server, time_msec);'; then
  pass "the system-gesture claim releases the app's contact first"
else
  err "maybe_claim_system_gesture() steals the gesture without releasing the app"
  FAILED=$((FAILED + 1))
fi
rel_line="$(printf '%s' "$body" | grep -n 'release_held_buttons' | head -1 | cut -d: -f1)"
clr_line="$(printf '%s' "$body" | grep -n 'wlr_seat_pointer_notify_clear_focus' | head -1 | cut -d: -f1)"
if [ -n "$rel_line" ] && [ -n "$clr_line" ] && [ "$rel_line" -lt "$clr_line" ]; then
  pass "and it does so before clearing focus"
else
  err "the release comes after clear_focus, so it is delivered to nobody"
  FAILED=$((FAILED + 1))
fi

body="$(function_body forward_pointer_motion)"
if printf '%s' "$body" | grep -q 'release_held_buttons(server, time_msec);'; then
  pass "forward_pointer_motion() releases before entering a new surface"
else
  err "forward_pointer_motion() can enter a new surface under a held button"
  FAILED=$((FAILED + 1))
fi

# 4. No other function may enter a surface. A new enter without the release
#    is exactly the bug, so any function that grows one has to grow the other.
enters="$(grep -c 'wlr_seat_pointer_notify_enter' "$INPUT_SRC" || true)"
guarded="$(grep -c 'release_held_buttons' "$INPUT_SRC" || true)"
if [ "$enters" -le "$guarded" ]; then
  pass "every notify_enter in this file has a matching release guard"
else
  err "$enters notify_enter call(s) but only $guarded release guard(s)"
  FAILED=$((FAILED + 1))
fi

if [ "$FAILED" -ne 0 ]; then
  die "$FAILED pointer-retarget check(s) failed"
fi
ok "the compositor never strands a client mid-gesture when input retargets"