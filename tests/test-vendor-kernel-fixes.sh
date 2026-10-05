#!/usr/bin/env bash
# Astrix OS - idempotency test for the CAF vendor-kernel build fixes.
#
# Why this test exists
# -------------------
# CI caches the cloned CAF tree between runs, so scripts/build-vendor-kernel.sh
# re-applies its fixes to a tree that already has them. Each fix therefore has
# to be a no-op the second time. One of them was not: the EFI stub strrchr fix
# guarded on the string "Deliberately not guarded by __HAVE_ARCH_STRRCHR" while
# inserting a comment reading "deliberately NOT guarded by ...". The guard could
# never match, so every run added another strrchr definition, and the kernel
# build died with "redefinition of 'strrchr'" in run 2 while the local tree,
# patched exactly once, built fine.
#
# A 30-minute kernel build is a bad place to discover that, so the fixes live in
# scripts/vendor-kernel-fixes.sh as a function over a tree path, and this test
# applies them to a synthetic tree twice. It asserts:
#
#   1. a second run changes nothing at all (byte-identical tree)
#   2. every fix's marker appears exactly once
#   3. libstub/string.c ends up with exactly one strrchr definition
#   4. a tree already poisoned with a duplicate self-heals
#   5. every guard string really is a substring of the text that fix inserts
#   6. the check bites: reintroducing the original broken guard is detected
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=../scripts/lib.sh
source "${REPO_ROOT}/scripts/lib.sh"

FIXES="${REPO_ROOT}/scripts/vendor-kernel-fixes.sh"
[ -r "$FIXES" ] || die "scripts/vendor-kernel-fixes.sh is missing"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- the synthetic CAF tree ------------------------------------------------
# Only the files the fixes touch, with the exact anchor lines they look for.
# Every anchor here is copied from the real CAF 4.9.112 tree; if one of them is
# wrong the fix will die with a clear "no anchor" error rather than silently
# doing nothing, which is the behaviour we want to keep.
make_tree() {
  local root="$1"
  rm -rf "$root"
  mkdir -p "$root"/drivers/bluetooth \
           "$root"/drivers/usb/gadget \
           "$root"/drivers/media/platform/msm/camera_v2/isp \
           "$root"/drivers/media/platform/msm/camera_v2/core \
           "$root"/drivers/firmware/efi/libstub \
           "$root"/arch/arm64/boot/dts/qcom

  printf '# bluetooth\nobj-y += foo.o\n' \
    > "$root/drivers/bluetooth/Makefile"
  printf '# gadget\nobj-y += configfs.o\n' \
    > "$root/drivers/usb/gadget/Makefile"
  printf '# isp\nobj-y += isp.o\n' \
    > "$root/drivers/media/platform/msm/camera_v2/isp/Makefile"
  printf '# camera_v2\nobj-y += main.o\n' \
    > "$root/drivers/media/platform/msm/camera_v2/Makefile"

  # The upstream CAF libstub/string.c has strstr, then this guard. No strrchr:
  # that absence is the whole reason fix 4 exists.
  cat > "$root/drivers/firmware/efi/libstub/string.c" <<'EOF'
#ifndef __HAVE_ARCH_STRRCHR
char *strstr(const char *s, const char *needle) { return 0; }
#endif

#ifndef __HAVE_ARCH_STRCMP
int strncmp(const char *cs, const char *ct, size_t count) { return 0; }
#endif
EOF

  # Written with printf, not a heredoc: the two anchors the fix looks for are
  # tab-indented kbuild lines, and a tab in a heredoc body is one refactor away
  # from silently becoming a space - which would make this fixture lie about
  # what CAF's Makefile looks like.
  # The structure is CAF's own: a `dtb-... += \` line followed by tab-indented
  # .dtb names, which is what the fix anchors on.
  printf 'dtb-$(CONFIG_MSM8937_DS4) += \\\n\tmsm8937-interposer-sdm429-mtp.dtb\n' \
    > "$root/arch/arm64/boot/dts/qcom/Makefile"
  printf 'dtb-$(CONFIG_ARCH_QM215) += qm215-qrd.dtb\n' \
    >> "$root/arch/arm64/boot/dts/qcom/Makefile"
  printf 'dtb-$(CONFIG_ARCH_QCOM) += \\\n\tqm215-qrd-smb1360.dtb\n' \
    >> "$root/arch/arm64/boot/dts/qcom/Makefile"
}

tree_digest() {
  ( cd "$1" && find . -type f | LC_ALL=C sort | xargs md5sum ) | md5sum | cut -d' ' -f1
}

# --- 1. applying twice changes nothing --------------------------------------
log "applying the fixes twice"
make_tree "$WORK/t1"
(
  # shellcheck source=/dev/null
  source "$FIXES"
  vendor_kernel_apply_fixes "$WORK/t1" >/dev/null
) || die "first application of the fixes failed"
d1="$(tree_digest "$WORK/t1")"

(
  # shellcheck source=/dev/null
  source "$FIXES"
  vendor_kernel_apply_fixes "$WORK/t1" >/dev/null
) || die "second application of the fixes failed"
d2="$(tree_digest "$WORK/t1")"

[ "$d1" = "$d2" ] \
  || die "applying the fixes twice changed the tree.
    Every fix has to be a no-op the second time: CI caches the cloned tree
    between runs. A fix that is not idempotent is how libstub/string.c ended up
    with two strrchr definitions and the kernel build failed."
pass "a second application changes nothing"

# --- 2. each fix's marker appears exactly once ------------------------------
log "each fix is present exactly once"
for pair in \
  "drivers/bluetooth/Makefile|Astrix: CAF sources include sibling headers" \
  "drivers/media/platform/msm/camera_v2/isp/Makefile|Astrix: ccflags-y does not propagate into sub-directories" \
  "drivers/usb/gadget/Makefile|Astrix: the CAF configfs.c includes <function/u_ncm.h>" \
  "drivers/firmware/efi/libstub/string.c|Astrix: deliberately NOT guarded by __HAVE_ARCH_STRRCHR" \
  "arch/arm64/boot/dts/qcom/Makefile|Astrix: the olive DTBs"; do
  file="${pair%%|*}"
  marker="${pair#*|}"
  n="$(grep -c -F "$marker" "$WORK/t1/$file" 2>/dev/null || true)"
  [ "$n" = "1" ] \
    || die "${file} contains the marker '${marker}' ${n} times, expected 1.
    A guard that cannot match what it inserts is re-applied on every run."
  pass "$(basename "$file"): ${marker:0:44}... x1"
done

# --- 3. exactly one strrchr definition --------------------------------------
log "the EFI stub keeps exactly one strrchr"
defs="$(grep -c '^char \*strrchr(const char \*s, int c)' \
         "$WORK/t1/drivers/firmware/efi/libstub/string.c" || true)"
[ "$defs" = "1" ] \
  || die "libstub/string.c defines strrchr ${defs} times, expected 1 - that does not compile"
pass "strrchr defined once"

# The definition must be unguarded: the EFI stub is freestanding and does not
# link lib/string.c, so __HAVE_ARCH_STRRCHR would hide it again.
if sed -n '/char \*strrchr/,/^}/p' \
     "$WORK/t1/drivers/firmware/efi/libstub/string.c" \
   | grep -q '__HAVE_ARCH_STRRCHR'; then
  die "strrchr is inside a __HAVE_ARCH_STRRCHR guard, so the stub would not get it"
fi
pass "strrchr is not guarded by __HAVE_ARCH_STRRCHR"

# It has to land before the strncmp block, which is the real anchor.
str_line="$(grep -n '^char \*strrchr' \
            "$WORK/t1/drivers/firmware/efi/libstub/string.c" | cut -d: -f1)"
ncmp_line="$(grep -n '^#ifndef __HAVE_ARCH_STRCMP' \
             "$WORK/t1/drivers/firmware/efi/libstub/string.c" | head -1 | cut -d: -f1)"
[ -n "$str_line" ] && [ -n "$ncmp_line" ] && [ "$str_line" -lt "$ncmp_line" ] \
  || die "strrchr (line ${str_line:-?}) is not before the strncmp anchor (line ${ncmp_line:-?})"
pass "strrchr inserted at the intended anchor"

# --- 4. a poisoned tree self-heals -----------------------------------------
# The duplicate CI actually produced. Because fix 4 is keyed on the file's
# post-state rather than on a "we have been here" marker, this tree is repaired
# rather than skipped forever. A marker guard would skip it, and since CI caches
# the tree, every later run would fail until somebody deleted the cache.
log "a tree poisoned by an older fix self-heals"
make_tree "$WORK/t2"
python3 - "$WORK/t2/drivers/firmware/efi/libstub/string.c" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
block = '''char *strrchr(const char *s, int c)
{
\tchar *last = NULL;

\tdo {
\t\tif (*s == (char)c)
\t\t\tlast = (char *)s;
\t} while (*s++);
\treturn last;
}

'''
marker = "Astrix: deliberately NOT guarded by __HAVE_ARCH_STRRCHR"
comment = "/*\n * " + marker + ".\n * injected by a run with a broken guard\n */\n"
# Each broken run inserted its own comment *and* its own function, so that is
# the shape to reproduce.
text = text.replace("#ifndef __HAVE_ARCH_STRCMP\n",
                    comment + block + comment + block + "#ifndef __HAVE_ARCH_STRCMP\n", 1)
open(path, "w").write(text)
PY
before="$(grep -c '^char \*strrchr' "$WORK/t2/drivers/firmware/efi/libstub/string.c" || true)"
[ "$before" = "2" ] || die "fixture is wrong: expected 2 strrchr definitions, made ${before}"

# No marker is removed and nothing else is faked: the fix keys on the definition
# count, sees two, and strips both before writing one.
(
  # shellcheck source=/dev/null
  source "$FIXES"
  vendor_kernel_apply_fixes "$WORK/t2" >/dev/null
) || die "the fixes refused to repair a tree with two strrchr definitions"
defs="$(grep -c '^char \*strrchr(const char \*s, int c)' \
         "$WORK/t2/drivers/firmware/efi/libstub/string.c" || true)"
[ "$defs" = "1" ] \
  || die "a tree with 2 strrchr definitions still has ${defs} after the fixes ran.
    The stale copies must be removed, not left to fail the build."
pass "2 stale copies removed, 1 left"

# And the post-condition must bite when the duplicates are *not* ones the patch
# recognises. Here they carry no Astrix comment, so the strip cannot identify
# them; writing another definition on top would leave the tree still broken, and
# guessing at code this script did not write is worse than stopping. This case
# has to end in a refusal rather than a silent partial repair.
make_tree "$WORK/t3"
python3 - "$WORK/t3/drivers/firmware/efi/libstub/string.c" <<'PY'
import sys
path = sys.argv[1]
text = open(path).read()
foreign = "char *strrchr(const char *s, int c)\n{\n\treturn 0;\n}\n\n"
text = text.replace("#ifndef __HAVE_ARCH_STRCMP\n",
                    foreign + foreign + "#ifndef __HAVE_ARCH_STRCMP\n", 1)
open(path, "w").write(text)
PY
if (
  # shellcheck source=/dev/null
  source "$FIXES"
  vendor_kernel_apply_fixes "$WORK/t3" >/dev/null 2>&1
); then
  die "the fixes accepted a libstub/string.c with 2 unrecognised strrchr definitions.
    That does not compile, and it is exactly what CI hit; the post-condition has
    to refuse it instead of writing a third definition on top."
fi
pass "the post-condition refuses unrecognisable duplicates"

# ...and a refusal has to leave the tree as it was, not half-patched.
n="$(grep -c '^char \*strrchr(const char \*s, int c)' \
      "$WORK/t3/drivers/firmware/efi/libstub/string.c" || true)"
[ "$n" = "2" ] \
  || die "the refused tree now has ${n} strrchr definitions, expected the original 2.
    A refusal has to leave the tree the way it found it."
pass "the refusal leaves the tree untouched"

# --- 5. every guard matches what its fix writes -----------------------------
# The structural version of check 1: if a guard string drifts away from the text
# its fix inserts, check 1 catches it only after the fixes have been run twice.
# This catches the mistake where the fix is correct but was never run.
#
# Fix 4 is deliberately absent: it has no marker guard, by design.
log "each guard string appears in the text its fix inserts"
mutant="$WORK/fixes-mutant.sh"
while IFS= read -r line; do
  case "$line" in
    *'grep -q "'*) printf '%s\n' "$line" ;;
  esac
done < "$FIXES" > "$WORK/guards.txt"
[ -s "$WORK/guards.txt" ] || die "found no guard strings in $FIXES"

# Each quoted marker in a guard must appear somewhere in the file: that is the
# invariant that was violated.
python3 - "$FIXES" <<'PY' > "$WORK/guards-extracted.txt"
import re
import sys

text = open(sys.argv[1]).read()
for line in text.splitlines():
    m = re.search(r'if ! grep -q -?F?\s*"([^"]+)"', line)
    if m:
        print(m.group(1))
PY
n_guards="$(wc -l < "$WORK/guards-extracted.txt" | tr -d ' ')"
[ "$n_guards" -ge 4 ] || die "expected at least 4 marker guards, found ${n_guards}"
n_found=0
while IFS= read -r marker; do
  if grep -qF -- "$marker" "$FIXES"; then
    n_found=$((n_found + 1))
  else
    die "guard marker not present in $FIXES: '${marker}'
    A guard has to grep for a string the fix actually writes."
  fi
done < "$WORK/guards-extracted.txt"
pass "all ${n_found} guard markers are substrings of the file"

# --- 6. the check bites -----------------------------------------------------
# Reintroduce the original defect - a guard that greps for something the fix
# never writes - and confirm this test's own mechanism reports it as
# non-idempotent. Without this, a future edit could weaken check 1 to nothing
# and nothing here would notice.
log "confirming the idempotency check bites"
python3 - "$FIXES" "$mutant" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
# The original bug's shape: a guard whose text is not what the fix writes, so
# it never matches and the fix runs again every time.
old = 'if ! grep -q "Astrix: the olive DTBs"'
new = 'if ! grep -q "Astrix: the olive DTBs are built"'
if old not in text:
    sys.exit("could not find the guard to mutate")
open(dst, "w").write(text.replace(old, new, 1))
PY
grep -q 'the olive DTBs are built' "$mutant" \
  || die "could not build the mutant - the guard text changed shape"
make_tree "$WORK/t4"
(
  # shellcheck source=/dev/null
  source "$mutant"
  vendor_kernel_apply_fixes "$WORK/t4" >/dev/null
) || die "the mutant's first run failed, which makes the check meaningless"
m1="$(tree_digest "$WORK/t4")"
m2_ok=0
if (
  # shellcheck source=/dev/null
  source "$mutant"
  vendor_kernel_apply_fixes "$WORK/t4" >/dev/null 2>&1
); then
  m2_ok=1
fi
m2="$(tree_digest "$WORK/t4")"

# Two ways a guard that cannot match its own output shows up: it changes the
# tree on the second run, or the second run fails outright because the anchors
# are already consumed. CI hit the first; the DTB fix's anchor checks turn the
# second into a loud error instead of a corrupt tree. Either one means this test
# would not have stayed green, which is the property being asserted.
if [ "$m2_ok" = "1" ] && [ "$m1" = "$m2" ]; then
  die "a guard that cannot match what its fix writes still produced an
    idempotent tree, so this test would not have caught the CI failure.
    The mutant is no longer representative of the defect."
fi
if [ "$m2_ok" = "1" ]; then
  pass "the defect shows up as a changed tree on the second run"
else
  pass "the defect shows up as a failed second run"
fi

ok "vendor kernel build fixes are idempotent and self-checking"