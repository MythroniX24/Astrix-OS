#!/usr/bin/env bash
# Astrix OS - the build fixes applied to the CAF vendor kernel tree.
#
# Sourced by scripts/build-vendor-kernel.sh, never executed directly:
#
#   . scripts/vendor-kernel-fixes.sh
#   vendor_kernel_apply_fixes "$KSRC"
#
# It is a separate file for one reason: these fixes are idempotent *by
# construction and by test*, and the only way to test that without a 30-minute
# kernel build is to apply them to a synthetic tree. That is what
# tests/test-vendor-kernel-fixes.sh does.
#
# Idempotency is not a nicety here. The CI workflow caches the cloned tree
# between runs, so a fix whose "have I already done this?" guard does not match
# the text it actually writes gets applied a second time on the next run. That
# is not harmless: for the EFI stub strrchr below it produced a duplicate
# definition and CI failed with "redefinition of 'strrchr'" while the local tree,
# which had been patched only once, built fine. Every guard in this file
# therefore greps for a string that is a substring of the text it inserts, and
# test-vendor-kernel-fixes.sh asserts that property rather than trusting it.

# Every fix is a real defect in the CAF tree that upstream and Google's build
# system paper over; kbuild alone does not. Grouped by cause rather than by
# file so the reasoning stays visible.
vendor_kernel_apply_fixes() {
  local ksrc="$1"
  local d sibling_note cam_note

  log "applying CAF build fixes"

  # 1. Include paths. CAF sources include sibling headers with <> and set
  #    TRACE_INCLUDE_PATH to ".", neither of which resolves without the source
  #    directory on the include path. Added per-directory rather than globally:
  #    cam_utils (v2/v3) and ipa_v2/v3 contain same-named headers, so a global
  #    -I silently shadows one with the other. -I$(src) is scoped and cannot.
  #
  #    Discovered by scanning every .c/.S for an <> include that names a header
  #    sitting next to it, plus every header defining TRACE_INCLUDE_PATH to ".".
  if ! grep -q "Astrix: CAF sources include sibling headers" \
       "${ksrc}/drivers/bluetooth/Makefile" 2>/dev/null; then
    sibling_note='
# Astrix: CAF sources include sibling headers with <> and set TRACE_INCLUDE_PATH
# to ".", neither of which resolves without the source directory on the include
# path. Scoped to this directory so same-named headers elsewhere (cam_utils,
# ipa_v2/v3) cannot shadow each other.
ccflags-y += -I$(src)'
    for d in drivers/bluetooth drivers/cpuidle drivers/gpu/msm \
             drivers/media/platform/msm/camera_v2/common \
             drivers/net/wireless/broadcom/brcm80211/brcmsmac \
             drivers/video/fbdev/msm/msm_dba \
             drivers/platform/msm/ipa/ipa_clients \
             drivers/platform/msm/ipa/ipa_v2 \
             drivers/platform/msm/ipa/ipa_v3 \
             drivers/clk/qcom/mdss drivers/devfreq drivers/android \
             drivers/staging/android/trace drivers/video/adf \
             drivers/video/fbdev/msm drivers/soc/qcom \
             drivers/media/platform/msm/sde/rotator \
             drivers/media/platform/msm/camera/cam_utils \
             drivers/media/platform/msm/camera_v3/cam_utils \
             arch/arm64/kvm arch/mips/kvm arch/powerpc/kvm arch/s390/kvm \
             arch/x86/kvm; do
      [ -f "${ksrc}/${d}/Makefile" ] && printf '%s\n' "$sibling_note" >> "${ksrc}/${d}/Makefile"
    done
    pass "per-directory -I\$(src) for sibling <> includes"
  else
    pass "per-directory -I\$(src) for sibling <> includes (already applied)"
  fi

  # 2. camera_v2 needs every sibling directory on its include path, because
  #    ccflags-y does not propagate into sub-directories. Checked that this
  #    subtree has no duplicate header basenames, so listing all 27 of them
  #    cannot shadow anything.
  if ! grep -q "Astrix: ccflags-y does not propagate into sub-directories, and the CAF" \
       "${ksrc}/drivers/media/platform/msm/camera_v2/isp/Makefile" 2>/dev/null; then
    cam_note='
# Astrix: ccflags-y does not propagate into sub-directories, and the CAF camera
# sources include headers from sibling directories with <>. This subtree has no
# duplicate header basenames (checked), so listing every directory here cannot
# shadow a same-named header.
'
    for d in $(cd "${ksrc}" && find drivers/media/platform/msm/camera_v2 -type d | sort) \
             drivers/media/platform/msm/camera_v2; do
      printf '%sccflags-y += -I$(srctree)/%s\n' "$cam_note" "$d" >> "${ksrc}/${d}/Makefile"
    done
    pass "camera_v2 sibling include paths"
  else
    pass "camera_v2 sibling include paths (already applied)"
  fi

  # 3. drivers/usb/gadget/configfs.c includes <function/u_ncm.h>, but only
  #    drivers/usb/gadget/udc was on the include path.
  if ! grep -q "Astrix: the CAF configfs.c includes <function/u_ncm.h>" \
       "${ksrc}/drivers/usb/gadget/Makefile" 2>/dev/null; then
    printf '\n# Astrix: the CAF configfs.c includes <function/u_ncm.h>, so the gadget\n# directory itself (not just udc/) has to be on the include path.\nccflags-y += -I$(srctree)/drivers/usb/gadget\n' \
      >> "${ksrc}/drivers/usb/gadget/Makefile"
    pass "usb/gadget include path"
  else
    pass "usb/gadget include path (already applied)"
  fi

  # 4. Real link error, not an include problem. CAF added a strrchr() call to
  #    scripts/dtc/libfdt/fdt_ro.c (upstream does not have one). The EFI stub
  #    links libfdt but is freestanding and pulls in only libstub/string.c, which
  #    upstream defines strstr and strncmp in but not strrchr. Without a local
  #    definition the final link fails on __efistub_strrchr.
  #    The guard is deliberately NOT __HAVE_ARCH_STRRCHR: arm64's asm/string.h
  #    defines it, because arm64's lib/string.c provides the function for the
  #    real kernel - but the stub does not link lib/string.c. Only
  #    libstub/string.c defines it, so there is no clash.
  #
  #    This one is keyed on the file's *post-state* rather than on a marker that
  #    says "we have been here". That distinction is the whole point: an earlier
  #    version of this fix used a marker guard whose text never matched what it
  #    wrote, so it ran again on every build, and CI's tree cache kept the
  #    duplicates. A "have I already done this?" guard then skips that tree
  #    forever and every later run fails. Asking instead "does the file already
  #    have exactly one definition?" repairs a poisoned tree and is a no-op on a
  #    healthy one.
  local stub_src="${ksrc}/drivers/firmware/efi/libstub/string.c"
  local defs
  defs="$(grep -c '^char \*strrchr(const char \*s, int c)' "$stub_src" 2>/dev/null || true)"
  if [ "${defs:-0}" != "1" ]; then
    # Every patch step checks its own exit status rather than relying on the
    # caller running under `set -e`. A patch that fails silently is worse than
    # one that fails loudly: the kernel then builds without the fix and the
    # error only appears as a link failure much later.
    python3 - "$stub_src" <<'PATCH' || die "could not add strrchr to ${stub_src}
    See the python error above. The file did not have the expected shape."
import re
import sys

path = sys.argv[1]
text = open(path).read()

marker = "Astrix: deliberately NOT guarded by __HAVE_ARCH_STRRCHR"

# Drop every copy a previous run injected, so the result depends only on the
# pristine input and never on how many times this script has run.
pattern = re.compile(
    r"/\*\n \* " + re.escape(marker) + r"\.\n"
    r"(?: \*.*\n)*?"
    r" \*/\n"
    r"char \*strrchr\(const char \*s, int c\)\n"
    r"\{.*?\n\}\n\n",
    re.DOTALL,
)
text, removed = pattern.subn("", text)

addition = '''/*
 * ''' + marker + '''.
 * arch/arm64/include/asm/string.h defines __HAVE_ARCH_STRRCHR because arm64's
 * lib/string.c provides strrchr for the real kernel, but the EFI stub is
 * freestanding and links only libstub's own string.c. CAF added a strrchr()
 * call to scripts/dtc/libfdt/fdt_ro.c (upstream has none), so without a local
 * definition the final vmlinux link fails with an undefined reference to
 * __efistub_strrchr. Only libstub/string.c defines it, so no clash with
 * lib/string.c in the actual kernel.
 */
char *strrchr(const char *s, int c)
{
\tchar *last = NULL;

\tdo {
\t\tif (*s == (char)c)
\t\t\tlast = (char *)s;
\t} while (*s++);
\treturn last;
}

'''

anchor = "#ifndef __HAVE_ARCH_STRCMP\n"
if anchor not in text:
    sys.exit("libstub/string.c has no '#ifndef __HAVE_ARCH_STRCMP' anchor to insert before")
text = text.replace(anchor, addition + anchor, 1)

if text.count("char *strrchr(const char *s, int c)") != 1:
    sys.exit("expected exactly one strrchr definition after patching")
open(path, "w").write(text)
print("stale injected copies removed: %d" % removed)
PATCH
    pass "EFI stub strrchr (${defs:-0} stale definitions replaced)"
  else
    pass "EFI stub strrchr (already applied)"
  fi

  # Assert the post-condition rather than trusting the patch above: two
  # definitions here do not compile, and without this the failure lands 30
  # minutes later in a link log instead of here.
  defs="$(grep -c '^char \*strrchr(const char \*s, int c)' "$stub_src" 2>/dev/null || true)"
  [ "$defs" = "1" ] \
    || die "libstub/string.c has ${defs:-0} strrchr definitions, expected exactly 1.
    Two copies do not compile. Something is adding definitions this script
    cannot recognise, so it refuses to guess. Re-run with --clean."
  pass "libstub/string.c defines strrchr exactly once"

  # 5. The olive DTBs are referenced only as an overlay base, so kbuild never
  #    builds them. They are the ones carrying the panel timings.
  local dts_make="${ksrc}/arch/arm64/boot/dts/qcom/Makefile"
  if ! grep -q "Astrix: the olive DTBs" "$dts_make" 2>/dev/null; then
    python3 - "$dts_make" <<'PATCH' || die "could not add the olive DTBs to ${dts_make}
    See the python error above. The Makefile did not have the expected anchors."
import sys
path = sys.argv[1]
text = open(path).read()
note = ('\n# Astrix: the olive DTBs were referenced only as an overlay base, so\n'
        '# kbuild never built them - and they are the ones carrying the DSI panel\n'
        '# timings.\n')
for anchor, added in (
    ('\tmsm8937-interposer-sdm429-mtp.dtb\n',
     '\tmsm8937-interposer-sdm429-mtp.dtb \\\n\tmsm8937-interposer-sdm439-olive.dtb\n'),
    ('\tqm215-qrd-smb1360.dtb\n',
     '\tqm215-qrd-smb1360.dtb \\\n\tsdm439-olive.dtb\n'),
):
    if anchor not in text:
        sys.exit("qcom/Makefile has no %r anchor" % anchor)
    text = text.replace(anchor, added, 1)
open(path, 'w').write(text + note)
PATCH
    pass "olive DTBs added to the build"
  else
    pass "olive DTBs added to the build (already applied)"
  fi

  ok "patches applied"
}