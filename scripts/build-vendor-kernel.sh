#!/usr/bin/env bash
# Astrix OS - build the Redmi 8A (olive) vendor kernel.
#
# This is a SEPARATE kernel from the Astrix mainline kernel that
# build-kernel.sh produces. This one is Qualcomm's CAF 4.9.112 "Roaring Lionus",
# which is what actually knows how to light the Redmi 8A's DSI panel. Its
# defconfig enables the framebuffer stack (FB_MSM_MDSS), not DRM/KMS, so this
# kernel drives the display the way Android does. Getting Astrix onto a real
# Redmi 8A means running Astrix's compositor on top of that display path, which
# is why this kernel is a prerequisite rather than an alternative.
#
# Nothing here has been booted on hardware. See docs/STATUS.md.
#
# Usage:
#   ./scripts/build-vendor-kernel.sh [--device olive] [--patched] [--clean]
#
#   --device   only "olive" is supported today (moto g64 has no vendor kernel)
#   --patched  skip the source patch step (already applied, or re-running make)
#   --clean    remove the clone and start over
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "${SCRIPT_DIR}/lib.sh"
trap on_error ERR

astrix_init_paths

DEVICE=olive
PATCHED=0
CLEAN=0

while [ $# -gt 0 ]; do
  case "$1" in
    --device)  DEVICE="$2"; shift 2 ;;
    --patched) PATCHED=1; shift ;;
    --clean)   CLEAN=1; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
done

[ "$DEVICE" = "olive" ] || die "device '${DEVICE}' has no vendor kernel repo (moto g64 / MT6855 has none published)"

# --- constants -------------------------------------------------------------
# NOTE: this is NOT redmi8a/android_kernel_xiaomi_olive, which looks like the
# obvious source but is incomplete: it is missing all 1451 .S assembly files
# (entry.S, head.S, md5.S, the whole vDSO), so it cannot be compiled at all.
# Verified by diffing its file list against upstream v4.9.112 and by the fact
# that a build of it dies on "No rule to make target arch/arm64/kernel/vdso/
# vdso.lds". J0SH1X's GSI branch is the same 4.9.112 CAF tree and is complete.
VENDOR_REPO="${VENDOR_REPO:-https://github.com/J0SH1X/android_kernel_xiaomi_olive}"
VENDOR_BRANCH="${VENDOR_BRANCH:-GSI}"
VENDOR_VERSION="4.9.112"
# GCC 11 (the distro default here) rejects this 4.9 tree; gcc-9 compiles it.
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
CC="${CROSS_COMPILE}gcc-9"
HOSTCC="${HOSTCC:-gcc-9}"
HOSTCXX="${HOSTCXX:-g++-9}"
DEFCONFIG="olive-perf_defconfig"
# 1 vCPU / 2 GB here. -j1 is what fits; more jobs OOMs.
JOBS="${BUILD_JOBS:-1}"

KSRC="${ASTRIX_BUILD_DIR}/vendor/${DEVICE}-kernel"
OUT="${ASTRIX_BUILD_DIR}/vendor/${DEVICE}-out"
DTBOUT="${ASTRIX_BUILD_DIR}/vendor/${DEVICE}-dtb"

if [ "$CLEAN" = "1" ]; then
  log "removing ${KSRC} and ${OUT}"
  rm -rf "$KSRC" "$OUT" "$DTBOUT"
fi

# --- toolchain preflight ---------------------------------------------------
log "checking the cross toolchain"
for t in "$CC" "${CROSS_COMPILE}ld" "${CROSS_COMPILE}objcopy" "$HOSTCC" dtc cpp; do
  command -v "$t" >/dev/null 2>&1 || die "missing tool: ${t}
    install: apt-get install -y gcc-9-aarch64-linux-gnu gcc-9 device-tree-compiler"
done
ok "toolchain present ($("$CC" -dumpversion))"

# --- clone -----------------------------------------------------------------
if [ ! -d "${KSRC}/.git" ]; then
  log "cloning the ${DEVICE} vendor kernel (${VENDOR_VERSION}, ~1 GB)"
  mkdir -p "$(dirname "$KSRC")"
  git clone --depth 1 --branch "$VENDOR_BRANCH" "$VENDOR_REPO" "$KSRC"
fi
ok "source at ${KSRC}"

KVER_ACTUAL="$(sed -n 's/^VERSION = //p; s/^PATCHLEVEL = //p; s/^SUBLEVEL = //p' "${KSRC}/Makefile" | tr '\n' '.' | sed 's/\.$//')"
[ "$KVER_ACTUAL" = "$VENDOR_VERSION" ] \
  || die "expected kernel ${VENDOR_VERSION}, tree is ${KVER_ACTUAL} - wrong branch?"
ok "kernel ${KVER_ACTUAL} $(sed -n 's/^NAME = //p' "${KSRC}/Makefile")"

# --- patches ---------------------------------------------------------------
# These are build fixes, not behaviour changes. Each one is a real defect in the
# CAF tree that upstream/b Google's build system papers over; kbuild alone does
# not. Grouped by cause rather than by file so the reasoning stays visible.
if [ "$PATCHED" != "1" ]; then
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
       "${KSRC}/drivers/bluetooth/Makefile" 2>/dev/null; then
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
      [ -f "${KSRC}/${d}/Makefile" ] && printf '%s\n' "$sibling_note" >> "${KSRC}/${d}/Makefile"
    done
    pass "per-directory -I\$(src) for sibling <> includes"
  fi

  # 2. camera_v2 needs every sibling directory on its include path, because
  #    ccflags-y does not propagate into sub-directories. Checked that this
  #    subtree has no duplicate header basenames, so listing all 27 of them
  #    cannot shadow anything.
  if ! grep -q "Astrix: ccflags-y does not propagate into sub-directories, and the CAF" \
       "${KSRC}/drivers/media/platform/msm/camera_v2/isp/Makefile" 2>/dev/null; then
    cam_note='
# Astrix: ccflags-y does not propagate into sub-directories, and the CAF camera
# sources include headers from sibling directories with <>. This subtree has no
# duplicate header basenames (checked), so listing every directory here cannot
# shadow a same-named header.
'
    for d in $(cd "${KSRC}" && find drivers/media/platform/msm/camera_v2 -type d | sort) \
             drivers/media/platform/msm/camera_v2; do
      printf '%sccflags-y += -I$(srctree)/%s\n' "$cam_note" "$d" >> "${KSRC}/${d}/Makefile"
    done
    pass "camera_v2 sibling include paths"
  fi

  # 3. drivers/usb/gadget/configfs.c includes <function/u_ncm.h>, but only
  #    drivers/usb/gadget/udc was on the include path.
  if ! grep -q "Astrix: the CAF configfs.c includes <function/u_ncm.h>" \
       "${KSRC}/drivers/usb/gadget/Makefile" 2>/dev/null; then
    printf '\n# Astrix: the CAF configfs.c includes <function/u_ncm.h>, so the gadget\n# directory itself (not just udc/) has to be on the include path.\nccflags-y += -I$(srctree)/drivers/usb/gadget\n' \
      >> "${KSRC}/drivers/usb/gadget/Makefile"
    pass "usb/gadget include path"
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
  if ! grep -q "Deliberately not guarded by __HAVE_ARCH_STRRCHR" \
       "${KSRC}/drivers/firmware/efi/libstub/string.c" 2>/dev/null; then
    python3 - "${KSRC}/drivers/firmware/efi/libstub/string.c" <<'PATCH'
import sys
path = sys.argv[1]
text = open(path).read()
addition = '''/*
 * Astrix: deliberately NOT guarded by __HAVE_ARCH_STRRCHR.
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

#ifndef __HAVE_ARCH_STRNCMP
'''
text = text.replace('#ifndef __HAVE_ARCH_STRNCMP\n', addition, 1)
open(path, 'w').write(text)
PATCH
    pass "EFI stub strrchr"
  fi

  # 5. The olive DTBs are referenced only as an overlay base, so kbuild never
  #    builds them. They are the ones carrying the panel timings.
  if ! grep -q "Astrix: the olive DTBs" \
       "${KSRC}/arch/arm64/boot/dts/qcom/Makefile" 2>/dev/null; then
    python3 - "${KSRC}/arch/arm64/boot/dts/qcom/Makefile" <<'PATCH'
import sys
path = sys.argv[1]
text = open(path).read()
note = ('\n# Astrix: the olive DTBs were referenced only as an overlay base, so\n'
        '# kbuild never built them - and they are the ones carrying the DSI panel\n'
        '# timings.\n')
text = text.replace('\tmsm8937-interposer-sdm429-mtp.dtb\n',
                    '\tmsm8937-interposer-sdm429-mtp.dtb \\\n\tmsm8937-interposer-sdm439-olive.dtb\n', 1)
text = text.replace('\tqm215-qrd-smb1360.dtb\n',
                    '\tqm215-qrd-smb1360.dtb \\\n\tsdm439-olive.dtb\n', 1)
open(path, 'w').write(text + note)
PATCH
    pass "olive DTBs added to the build"
  fi

  ok "patches applied"
fi

# --- build -----------------------------------------------------------------
# Kbuild 4.9: no LLVM=1, and the defconfig is applied in-tree.
log "applying ${DEFCONFIG}"
make -C "$KSRC" -j"$JOBS" CROSS_COMPILE="$CROSS_COMPILE" ARCH=arm64 \
     CC="$CC" HOSTCC="$HOSTCC" HOSTCXX="$HOSTCXX" "$DEFCONFIG"

# The display stack is the whole point of using this kernel. If any of these
# silently flipped off, the result would build fine and show nothing.
log "verifying the display stack is enabled"
for opt in CONFIG_QCOM_KGSL CONFIG_FB_MSM_MDSS CONFIG_MSM_MDSS_PLL \
           CONFIG_FB_MSM_MDSS_DSI_CTRL_STATUS CONFIG_QCOM_KGSL_IOMMU; do
  grep -q "^${opt}=y" "${KSRC}/.config" \
    || die "${opt} is not set - this kernel would not drive the panel"
  pass "${opt}=y"
done
if grep -q '^CONFIG_DRM_MSM=y' "${KSRC}/.config"; then
  warn "CONFIG_DRM_MSM=y - unexpected, the display path may differ from Android's"
fi

log "building Image (-j${JOBS}; a full build is ~30 min on 1 vCPU)"
make -C "$KSRC" -j"$JOBS" CROSS_COMPILE="$CROSS_COMPILE" ARCH=arm64 \
     CC="$CC" HOSTCC="$HOSTCC" HOSTCXX="$HOSTCXX" Image

IMAGE="${KSRC}/arch/arm64/boot/Image"
[ -s "$IMAGE" ] || die "make reported success but ${IMAGE} is missing or empty"
ok "Image $(human_size "$IMAGE")"

# --- device tree -----------------------------------------------------------
# Built directly with cpp + dtc rather than through kbuild: the kbuild dtb rule
# resolves %s against the wrong directory for a single out-of-tree dtb target,
# and these need no kernel build anyway.
log "building the olive device trees"
mkdir -p "$DTBOUT"
cd "${KSRC}/arch/arm64/boot/dts/qcom"
DTC_FLAGS="-Wno-unit_address_vs_reg -Wno-simple_bus_reg -Wno-unit_address_format -Wno-pci_bridge -Wno-pci_device_reg"
for d in sdm439-olive msm8937-interposer-sdm439-olive; do
  cpp -nostdinc -I "${KSRC}/include" -I . -undef -D__DTS__ \
      -x assembler-with-cpp -o "${DTBOUT}/${d}.dts.tmp" "${d}.dts"
  dtc -O dtb -o "${DTBOUT}/${d}.dtb" -b 0 -i . $DTC_FLAGS "${DTBOUT}/${d}.dts.tmp"
  ok "${d}.dtb $(human_size "${DTBOUT}/${d}.dtb")"
done

# The panel this board actually selects. If this node is missing or disabled,
# the DT is wrong for olive even though it compiled.
if ! grep -q 'dsi_hx8399c_truly_vid' "${DTBOUT}/sdm439-olive.dtb" 2>/dev/null \
   && ! strings "${DTBOUT}/sdm439-olive.dtb" | grep -q 'hx8399c'; then
  die "the built DTB does not mention the hx8399c panel - wrong tree?"
fi
pass "hx8399c DSI panel present in the DT"

echo
log "vendor kernel artefacts"
info "Image  ${IMAGE}"
info "DTB    ${DTBOUT}/sdm439-olive.dtb"
info "       ${DTBOUT}/msm8937-interposer-sdm439-olive.dtb"
echo
warn "NOT BOOTED ON HARDWARE. This kernel drives the panel through the Android"
warn "framebuffer stack (FB_MSM_MDSS), not DRM/KMS, so wlroots' DRM backend will"
warn "not find a KMS device in it yet. See docs/STATUS.md."