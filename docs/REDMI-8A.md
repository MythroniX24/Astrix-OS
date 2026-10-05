# Running Astrix on a Redmi 8A (`olive`)

**Short version:** Astrix OS does not boot on a Redmi 8A, and no amount of
build work changes that this year. What *is* possible today is running the
Astrix interface on the phone with nothing flashed and nothing unlocked, and
separately proving that Astrix's kernel starts on the hardware. Both are
supported by a script in this repository. Neither is what people usually mean
by "boot Astrix on my phone", and this page says so repeatedly on purpose.

```sh
./scripts/port-redmi-8a.sh status     # what is real, what is not, and why
```

---

## Why it does not boot

Three blockers, in order of how much work they are:

| Blocker | Detail | Fixable by building? |
| --- | --- | --- |
| **No Adreno 505 driver** | Mainline Linux has no DRM driver for Adreno 5xx at all. The only driver is Qualcomm's closed KGSL blob, which needs the downstream kernel. | **No.** The code does not exist upstream. |
| **No `olive` device tree** | No upstream board file describes this phone's regulators, clocks, PHYs, GPIO or panel wiring. | Partially — the DT can be written, but only against a kernel with drivers to bind to. |
| **Locked bootloader** | Unlocking needs a Xiaomi account bound to the device and a waiting period that is commonly 72 hours or more, via Mi Unlock on Windows. | **No.** Nothing in this repository shortens it. |

The first one is the important one. It is a *missing driver*, not a missing
`CONFIG_` option, and that distinction is the whole difference between "edit
the config and retry" and "write a kernel driver". `scripts/device.sh check
redmi-8a` prints both categories separately on purpose.

What is **not** a problem: the display link. The panel is 720×1520 at 60 Hz,
which needs 476.7 Mbit/s per lane against a 2.5 Gbit/s lane — 81% headroom.
If a driver is ever obtained, the panel side is the easy half. See
[`DEVICES.md`](DEVICES.md) and `scripts/display-budget.sh`.

---

## Route 1 — `android-host`: run Astrix inside Android

**What this is.** The Astrix userspace — compositor, shell, keyboard, terminal,
files, settings, sysinfo, package and APK manager — running as a `proot` chroot
on the phone's own Android kernel, with its output bridged to the screen
through Termux:X11 → SurfaceFlinger.

**What this is not.** A boot of Astrix OS. Android's kernel, its drivers, its
bootloader and its GPU are still underneath and still in charge. Nothing is
flashed, the bootloader stays locked, and no partition is touched. If Android
dies tomorrow, Astrix died with it.

### What it proves

- The compositor runs on Snapdragon 439 with this GPU's userspace.
- The shell and every app work at 720×1520.
- The modern visual language renders on the actual panel.
- Touch input reaches the shell through the X11 pointer, so the on-screen
  keyboard and the app grid both work.

### What it does not prove

Anything about Astrix's kernel, drivers, bootloader or disk layout.

### Running it

Host (Ubuntu, `adb` installed, phone with USB debugging on):

```sh
./scripts/port-redmi-8a.sh android-host pack    # ~6 MB bundle from build/rootfs
./scripts/port-redmi-8a.sh android-host check   # preflight the phone
./scripts/port-redmi-8a.sh android-host push    # copy it over, set up adb forward
```

`check` verifies the phone is `arm64-v8a` and that Termux, Termux:X11 and
Termux:PROOT are installed, and tells you to install them if they are not. All
three come from F-Droid — **not** the Play Store version of Termux, which is
unrelated and will not work.

On the phone:

1. Open Termux:X11 and start its VNC server (the notification).
2. Open Termux and run:

   ```sh
   bash /sdcard/Download/astrix/astrix-android-host.sh start
   ```

3. The Astrix session appears in the Termux:X11 window.
4. Stop it with the same script and `stop`.

The launcher writes `/usr/bin/astrix-compositor`'s log to
`$HOME/astrix-compositor.log` in the Termux home directory. That is the first
place to look if the window stays black.

### Why it is built this way

`pack` does **not** push the 3.2 GB rootfs. It walks the ELF `NEEDED` graph of
the eight `astrix-*` binaries plus the session scripts, starting from their
shared libraries, and copies only the closure — around 150 files, 22 MB on
disk, 6.4 MB compressed. `ldd` cannot be used for this: the binaries are
ARM64 and the host is x86_64. Each file's dynamic section is read directly
instead.

The display path is `WLR_BACKEND=x11`. Debian's `wlroots-0.18` is built with the
X11 backend, and Termux:X11 speaks X11 to SurfaceFlinger — so the Astrix
compositor talks X11, and SurfaceFlinger does the actual GPU work. Rendering
falls back to Mesa's llvmpipe (`LIBGL_ALWAYS_SOFTWARE=1`): the Adreno 505's
userspace GL driver is not reachable from inside a proot chroot, and asking for
it produces a black window and a GLX error rather than a slow window. Expect a
usable but not fast session.

---

## Route 2 — `boot-test`: flash Astrix's kernel and read the console

**What this is.** A normal Android boot image containing Astrix's kernel and
this build's initramfs, written to the phone's `boot` partition, followed by a
boot whose entire proof is the USB serial console.

**What this is not.** A working OS on the phone. **The screen stays dark for
the entire boot**, because the kernel has no Adreno 505 driver and therefore
nothing able to draw. This is the expected outcome, not a fault.

### What it proves

- Astrix's kernel starts on `olive`.
- The board initialises far enough to reach userspace.
- `systemd` comes up and `astrix-boot-report` runs.

That is a real result, and it is the prerequisite for everything else in a port.
It is also, deliberately, the *only* thing this route claims.

### Before you do it

1. **Unlock the bootloader.** Mi Unlock, Xiaomi account, waiting period, Windows.
   Unlocking erases the phone.
2. **Have a stock `boot.img` for your exact ROM version.** Set
   `ASTRIX_STOCK_BOOT_IMAGE=/path/to/stock-boot.img` before `build` and it will
   be used as the header. Without one, `abootimg` generates the header itself,
   which usually works and occasionally does not — and a stock image is how you
   get Android back.

### Running it

```sh
./build.sh                                          # kernel + initramfs
./scripts/port-redmi-8a.sh boot-test build
./scripts/port-redmi-8a.sh boot-test log           # capture serial, no flashing

ASTRIX_ALLOW_UNVERIFIED_FLASH=1 \
  ./scripts/port-redmi-8a.sh boot-test stage        # the destructive one
./scripts/port-redmi-8a.sh boot-test log 300       # read the proof
```

`stage` refuses without `ASTRIX_ALLOW_UNVERIFIED_FLASH=1`, refuses a locked
bootloader, writes **only** `boot`, and still asks you to type the codename
`olive`. The override exists because a port cannot begin without a failed
attempt to learn from; the name exists because nobody should hit it by accident.

The console is `ttyMSM0, 115200 8n1`. What to look for, in order:

```
Linux version 6.12...
systemd[1]: Detected architecture arm64.
astrix-boot-report: ...
```

If the capture is empty, that is not proof of a failed boot — the console may
not have enumerated yet, or this kernel's `earlycon` may not be wired to the USB
gadget. `boot-test log` says so rather than letting you guess.

### Getting Android back

`olive` is A/B, so the kernel you replaced is usually still there:

```sh
adb reboot bootloader
fastboot --set-active=a      # or b
fastboot reboot
```

If that is not enough, flash the stock `boot.img` for your ROM the same way.
Nothing outside `boot` was written, so a full ROM reflash should not be
necessary — and if it looks necessary, stop and say so rather than erasing
anything else.

---

## Which route should you use

| You want to… | Use | Costs you |
| --- | --- | --- |
| See the Astrix UI on the phone today | `android-host` | Nothing. Reversible by deleting one folder. |
| Know whether Astrix's kernel runs on `olive` | `boot-test` | Unlocking (erases the phone), and Android until you flash it back. |
| Actually boot Astrix OS on `olive` | Neither | A GPU driver that does not exist upstream. |

---

## Status

**Nothing in this repository has been run on physical hardware.** No `olive`
device has ever been connected to a machine with this code on it. Every
command above is written against the real hardware specifications and the real
host tooling, and every one of them has been run *here* up to the point where
a phone is required — the bundle is built, the boot image is assembled, the
refusal gates are exercised, and `tests/test-devices.sh` asserts them.

What is verified about the phone itself comes from the reference kernel's
device tree for `olive`, not from measurement. The DSI blanking percentages in
`config/devices/redmi-8a.conf` are an explicit assumption, recorded as one in
the profile and labelled by `display-budget.sh`.

---

## The vendor kernel now builds

```sh
./scripts/build-vendor-kernel.sh          # olive
```

This is a second kernel, separate from the Astrix mainline kernel in
`scripts/build-kernel.sh`: it is Qualcomm's CAF 4.9.112 "Roaring Lionus", the
one the phone actually ships with, and the only one that knows how to light this
panel.

It **compiles**: a 30 MB `Image`, plus `sdm439-olive.dtb` and the interposer
DTB carrying the panel timings. Verified from a pristine `git checkout -- .` of
the upstream tree, so the script's own patching is proven and not just an
incremental artefact.

It also compiles in CI: `.github/workflows/vendor-kernel-olive.yml` runs this
exact script on every push that touches it, at `BUILD_JOBS=4` instead of the
build host's `-j1` (one vCPU, 2 GB), and uploads the `Image` and both DTBs as
artefacts with 30-day retention. So you can get something flashable without
waiting half an hour on a slow machine. If that workflow ever goes red, the
first thing to check is the cross compiler: the tree needs
`aarch64-linux-gnu-gcc-9`, and the runner's default gcc-11 will not compile it.

Two things worth knowing before you go looking for the source:

- **The obvious repo cannot be built.**
  `redmi8a/android_kernel_xiaomi_olive` is missing all 1451 `.S` assembly
  files — `entry.S`, `head.S`, the vDSO. Its GitHub contents API returns 404
  for those paths. `scripts/build-vendor-kernel.sh` uses
  `J0SH1X/android_kernel_xiaomi_olive` branch `GSI` instead: the same 4.9.112
  CAF tree, complete.
- **gcc 11 will not compile it.** The script pins `aarch64-linux-gnu-gcc-9`,
  which does.
- **Re-running it has to be safe, because CI does.** `vendor-kernel-olive.yml`
  caches the cloned tree between runs, so `scripts/build-vendor-kernel.sh`
  re-applies its fixes to a tree that already has them. The fixes live in
  `scripts/vendor-kernel-fixes.sh` and are pinned by
  `tests/test-vendor-kernel-fixes.sh`, which applies them to a synthetic tree
  **twice** and requires a byte-identical result. This is not ceremony: the
  strrchr fix once guarded on a marker that did not match the text it wrote, so
  every cached run added another definition and CI failed with
  `redefinition of 'strrchr'` while the local tree, patched once, built fine.

### The device tree points the panel at the wrong timing

This one is worth reading before flashing anything. Decompiling the **built**
`sdm439-olive.dtb` (not reading the `.dtsi` it came from) shows two `hx8399c`
nodes:

| Node | Resolution |
| --- | --- |
| `qcom,mdss_dsi_hx8399c_truly_video` | 1080x2160 |
| `qcom,mdss_dsi_hx8399c_hd_video` | 720x1440 |

and on `mdss_dsi_ctrl0@1a94000`, `qcom,dsi-pref-prim-pan` is phandle `0x7f` —
which belongs to the **truly** node. So the DSI link is configured for a
1080x2160 panel on hardware whose screen is 720x1520 (Xiaomi's own spec page:
HD+ 720x1520).

That is a boot failure, not a rounding difference. Flashing this DTB unchanged
feeds the panel a timing it cannot display. The fix is an overlay that
repoints `qcom,dsi-pref-prim-pan` at the `hd` node — a device-tree change to
Astrix's own overlay, not a kernel change.

### Seeing what the display path is actually doing

Because the vendor kernel drives the panel through fbdev, `/dev/fb0` is the only
place the pixels are while the panel is being lit:

```sh
./scripts/port-redmi-8a.sh boot-test fb        # phone, over adb
./scripts/fb-screenshot.sh                    # or a device / dump directly
```

It reads the device's own sysfs for geometry, refuses to read a buffer too short
to be a framebuffer, and decodes 16bpp RGB565 / 24bpp / 32bpp BGRA with the real
stride. A good capture proves the compositor is drawing correct pixels at the
panel's geometry — and says loudly that it does **not** prove the DSI link is
carrying them, because the framebuffer is upstream of the link.

**None of this is a boot.** The kernel drives the panel through the Android
framebuffer stack (`FB_MSM_MDSS`), not DRM/KMS, so wlroots' DRM backend will not
find a KMS device in it as configured. `DEVICE_VERIFIED_BOOT` is still `no`, and
`tests/test-devices.sh` fails the profile if a building vendor kernel is ever
allowed to imply a verified boot or an expected display.

Related: [`DEVICES.md`](DEVICES.md), [`PORTING.md`](PORTING.md),
[`ANDROID.md`](ANDROID.md).