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
# The fixes live in scripts/vendor-kernel-fixes.sh rather than inline so they
# can be applied to a synthetic tree and tested for idempotency without a
# 30-minute kernel build. That matters because CI caches the cloned tree between
# runs: a fix whose idempotency guard does not match the text it actually writes
# is applied again on every run, which is exactly how the EFI stub strrchr fix
# came to be defined twice and failed the kernel build.
# shellcheck source=vendor-kernel-fixes.sh
source "${SCRIPT_DIR}/vendor-kernel-fixes.sh"

if [ "$PATCHED" != "1" ]; then
  vendor_kernel_apply_fixes "$KSRC"
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