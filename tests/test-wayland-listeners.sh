#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Every wl_*_listener must implement every event the protocol defines.
#
# Why this test exists
# -------------------
# libwayland aborts a client that receives an event it has no listener for:
#
#   astrix-shell[940]: listener function for opcode 5 of wl_pointer is NULL
#   audit: ... comm="astrix-shell" sig=6
#
# The shell had implemented enter/leave/motion/button/axis and not frame.
# Because the compositor frames every pointer batch, the shell died on the
# first touch of every session, systemd restarted it, and the session looked
# completely healthy - a mapped surface, an active unit - while being deaf to
# input. The only symptom was one line in a serial log, on a phone nobody was
# looking at.
#
# That is a compile-time mistake hiding behind a runtime-only failure, and the
# C compiler cannot catch it: the listener structs are plain structs with
# named members, so a short initialiser is legal C and simply zero-fills the
# rest. This test parses the initialisers and compares them against the
# protocol header the build actually uses, so the same mistake fails here
# instead of on a device.
#
# It is a source-level check, deliberately: it needs no VM, no QEMU and no
# rootfs, so it runs in the fast test suite and catches the mistake in
# seconds rather than after a twelve-minute boot.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../scripts/lib.sh
. "$REPO_ROOT/scripts/lib.sh"

# The header the GUI is compiled against is the one in the rootfs, because
# that is the libwayland the binaries are linked to. Comparing against the
# host's copy would pass while the real build is broken (or fail while the
# real build is fine) - the mismatch between the two is exactly how this
# class of bug gets missed.
HEADER=""
for candidate in \
    "$REPO_ROOT/build/rootfs/usr/include/wayland-client-protocol.h" \
    "$REPO_ROOT/build/astrix-build/pg/xdg-shell-client-header.h"; do
  if [ -f "$candidate" ]; then
    HEADER="$candidate"
    break
  fi
done
if [ -z "$HEADER" ]; then
  warn "no generated Wayland protocol header found; run ./build.sh first"
  warn "skipping the listener check (it cannot be done without the real header)"
  exit 0
fi

pass=0
failed=0

# Event names for a listener struct, in declaration order, from the generated
# header. Declaration order is the opcode order on the wire, which is why
# "opcode 5" was enough to identify frame.
listener_events() {
  local iface="$1"
  python3 - "$HEADER" "$iface" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
iface = sys.argv[2]
m = re.search(r"struct %s_listener \{(.*?)\n\s*\};" % re.escape(iface), src, re.S)
if not m:
    sys.exit(0)
for name in re.findall(r"void \(\*(\w+)\)", m.group(1)):
    print(name)
PY
}

# Members an initialiser actually sets, in source order.
initialised_members() {
  local file="$1" struct="$2"
  python3 - "$file" "$struct" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
struct = sys.argv[2]

# Comments are stripped first, and that is not cosmetic. A comment explaining
# why `.frame = NULL` must never be written contains the literal text
# ".frame = NULL" - and a regex that reads it decides the handler is NULL.
# A source checker that parses prose as code is worse than no checker.
src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
src = re.sub(r"//[^\n]*", " ", src)

# The initialiser names a variable between the struct and the brace:
#   const struct wl_pointer_listener pointer_listener = { ... };
# The match must end at the *first* "};", wherever it sits. Anchoring it to
# "\n\s*};" only finds the closing brace of an initialiser whose last member
# is on its own line, so a one-line initialiser
#   static const struct wl_buffer_listener buffer_listener = { .release = f };
# runs the match past itself and swallows the next struct's members. That
# produced two phantom "sets the same member twice" errors on astrix_app.c
# and, worse, would have hidden a genuinely missing handler in a one-line
# initialiser. Listener initialisers hold only function pointers, so there
# is no nested brace for a non-greedy match to stop inside.
m = re.search(
    r"struct %s\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*\{(.*?)\};" % re.escape(struct),
    src,
    re.S,
)
if not m:
    sys.exit(0)
for name in re.findall(r"\.(\w+)\s*=", m.group(1)):
    print(name)
PY
}

check_listener() {
  local file="$1" struct="$2" iface="$3"
  local label; label="$(basename "$file"): $struct"

  # The initialiser names a variable between the struct and the brace:
  #   const struct wl_pointer_listener pointer_listener = { ... };
  if ! grep -qE "struct ${struct}[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=" "$file"; then
    return 0
  fi

  local want have
  want="$(listener_events "$iface" | tr '\n' ' ')"
  [ -n "$want" ] || return 0
  have="$(initialised_members "$file" "$struct" | tr '\n' ' ')"

  local missing=""
  for ev in $want; do
    case " $have " in
      *" $ev "*) ;;
      *) missing="$missing $ev" ;;
    esac
  done

    # A member set twice is worse than one never set. C evaluates designated
  # initialisers in order, so
  #     .frame = pointer_handle_frame,
  #     .frame = NULL,
  # ends with a NULL handler and the client aborts on the first touch - while
  # gcc only prints a -Woverride-init warning that scrolls past in a 400-line
  # build log. This is not hypothetical: it is exactly what was in the tree,
  # and it cost a full boot cycle to find.
  dupes="$(initialised_members "$file" "$struct" | sort | uniq -d | tr '\n' ' ')"
  if [ -n "$dupes" ]; then
    err "$label sets the same member twice:${dupes}"
    info "  a later initialiser silently overwrites the earlier one; gcc only"
    info "  warns (-Woverride-init), so this fails here instead of on a device"
    failed=$((failed + 1))
  fi

  if [ -n "$missing" ]; then
    err "$label does not handle:${missing}"
    info "  libwayland ABORTS a client that receives an unhandled event"
    info "  (listener function for opcode N of $iface is NULL), so a missing"
    info "  handler is a crash on the first input, not a silent no-op"
    failed=$((failed + 1))
  else
    ok "$label handles all ${iface} events"
    pass=$((pass + 1))
  fi
}

for src_file in \
    "$REPO_ROOT/gui/shell/src/main.c" \
    "$REPO_ROOT/apps/common/src/astrix_app.c" \
    "$REPO_ROOT/apps/terminal/src/main.c" \
    "$REPO_ROOT/apps/files/src/main.c" \
    "$REPO_ROOT/apps/settings/src/main.c" \
    "$REPO_ROOT/apps/sysinfo/src/main.c" \
    "$REPO_ROOT/apps/package-manager/src/main.c" \
    "$REPO_ROOT/apps/apk-manager/src/main.c"; do
  [ -f "$src_file" ] || continue
  # Every wl_*_listener initialiser in the file, checked against the
  # interface of the same name.
  while read -r struct; do
    [ -n "$struct" ] || continue
    struct="${struct#struct }"
    iface="wl_${struct#wl_}"
    iface="${iface%_listener}"
    check_listener "$src_file" "$struct" "$iface"
  done < <(grep -o 'struct wl_[a-z0-9_]*_listener' "$src_file" | sort -u)
done

# The compositor binds listeners too - it just has to *provide* them rather
# than receive them, so a missing handler is a NULL function pointer called
# by wlroots rather than an abort. Same check, same reasoning.
for src_file in \
    "$REPO_ROOT/gui/compositor/src/main.c" \
    "$REPO_ROOT/gui/compositor/src/shell.c"; do
  [ -f "$src_file" ] || continue
  while read -r struct; do
    [ -n "$struct" ] || continue
    struct="${struct#struct }"
    iface="wl_${struct#wl_}"
    iface="${iface%_listener}"
    check_listener "$src_file" "$struct" "$iface"
  done < <(grep -o 'struct wl_[a-z0-9_]*_listener' "$src_file" | sort -u)
done

# The specific regression, named so it is impossible to miss later.
SHELL_MAIN="$REPO_ROOT/gui/shell/src/main.c"
if [ -f "$SHELL_MAIN" ]; then
  if grep -q '\.frame = pointer_handle_frame' "$SHELL_MAIN"; then
    ok "wl_pointer.frame is handled (the SIGABRT regression)"
    pass=$((pass + 1))
  else
    err "gui/shell/src/main.c: wl_pointer has no .frame handler"
    info "  without it the shell SIGABRTs on the first pointer event"
    failed=$((failed + 1))
  fi
fi

info "checked $((pass + failed)) listener check(s)"
[ "$failed" -eq 0 ] || die "$failed listener check(s) failed"
ok "every Wayland listener implements every event it is sent"
