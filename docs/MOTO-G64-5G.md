# Running Astrix on a moto g64 5G (`retin`)

**Short version:** this is the closer of the two phones in this repository,
and it still does not boot Astrix. The GPU is fine. The display around the GPU
does not exist upstream. Below that there are three routes — two of them the
same as the Redmi 8A's, and a third that needs no phone at all and is a
pass/fail test today.

```sh
./scripts/port-moto-g64-5g.sh status
```

---

## Why it does not boot

| Blocker | Detail | Fixable by building? |
| --- | --- | --- |
| **No MT6855 display pipeline** | The DSI controller, the panel driver and the GPIO sequence for panel reset/enable are all downstream-only. | **No.** The code does not exist upstream. |
| **No `retin` device tree** | No upstream board file describes this phone's regulators, clocks, PHYs or GPIO. | Partially — the DT can be written, but only against a kernel with drivers to bind to. |
| **No Astrix boot chain** | Motorola's unlock token (IMEI request, emailed back) is far friendlier than Xiaomi's, but an unlocked Motorola bootloader only knows how to chainload an **Android** boot image. Astrix's own chain has to be built. | Yes. This is ordinary bootloader work. |

### The thing that makes this board different

The GPU is a **Mali-G57 MC2, and Panfrost is a real open-source DRM driver in
mainline Linux for it.** That is a genuine GPU path, and the Redmi 8A has no
equivalent — its Adreno 505 has no mainline driver in any form.

But a GPU is not a display. Panfrost gives something that can render; there is
still nothing upstream to present a scanout to the panel. Getting from "can
render" to "can light up this screen" is the entire remaining port.

---

## Route 1 — `android-host`: run Astrix inside Android

**What this is.** The Astrix userspace — compositor, shell, keyboard, terminal,
files, settings, sysinfo, package and APK manager — running as a `proot` chroot
on the phone's own Android kernel, output bridged through Termux:X11 →
SurfaceFlinger.

**What this is not.** A boot of Astrix OS. Android's kernel, drivers,
bootloader and GPU are still underneath and still in charge. Nothing is
flashed, the bootloader stays locked, no partition is touched.

**Proves:** the compositor, the shell and every app work on Dimensity 7025 at
1080×2400. Touch reaches the shell through the X11 pointer, so the on-screen
keyboard and the app grid both work.

**Does not prove:** anything about Astrix's kernel, drivers or disk layout.

```sh
./scripts/port-moto-g64-5g.sh android-host pack    # ~6 MB bundle
./scripts/port-moto-g64-5g.sh android-host check   # preflight the phone
./scripts/port-moto-g64-5g.sh android-host push    # copy it over, set up adb forward
```

On the phone: start Termux:X11's VNC server, then in Termux

```sh
bash /sdcard/Download/astrix/astrix-android-host.sh start
```

Termux, Termux:X11 and Termux:PROOT all come from **F-Droid** — the Play Store
version of Termux is unrelated and will not work. `check` verifies all three are
installed and tells you if they are not.

`pack` does not push the 3.2 GB rootfs. It walks the ELF `NEEDED` graph of the
`astrix-*` binaries and copies only the closure — about 150 files, 19 MB on
disk, 6 MB compressed — because `ldd` cannot run foreign-architecture binaries
on an x86 host. The display path is `WLR_BACKEND=x11`, with Mesa's llvmpipe for
GL, since a phone GPU's userspace GL driver is not reachable from inside a proot
chroot. Expect a usable but not fast session.

---

## Route 2 — `boot-test`: flash Astrix's kernel, read the console

**What this is.** A normal Android boot image containing Astrix's kernel and
this build's initramfs, written to `boot`, with the entire proof read off the
USB serial console.

**What this is not.** A working OS on the phone. **The screen stays dark for the
entire boot** — Panfrost renders to nothing it can present. This is the expected
outcome, not a fault.

**Proves:** Astrix's kernel starts on `retin`, initialises the board, and
reaches userspace.

**Does not prove:** a picture.

This is the cheaper of the two boot tests in this repository: Motorola's unlock
token is requested with the device's IMEI and emailed back, where Xiaomi's
Mi Unlock needs an account-bound waiting period. Some retail variants are
refused outright with "device not qualified".

```sh
./build.sh
./scripts/port-moto-g64-5g.sh boot-test build
./scripts/port-moto-g64-5g.sh boot-test log           # capture serial, no flashing

ASTRIX_ALLOW_UNVERIFIED_FLASH=1 \
  ./scripts/port-moto-g64-5g.sh boot-test stage        # the destructive one
./scripts/port-moto-g64-5g.sh boot-test log 300       # read the proof
```

`stage` refuses without `ASTRIX_ALLOW_UNVERIFIED_FLASH=1`, refuses a locked
bootloader, writes **only** `boot`, and still asks you to type the codename
`retin`.

Console: `ttyMSM0, 115200 8n1`. Look for, in order:

```
Linux version 6.12...
systemd[1]: Detected architecture arm64.
astrix-boot-report: ...
```

An empty capture is **not** proof of a failed boot — the console may not have
enumerated yet, or this kernel's `earlycon` may not be wired to the USB gadget.
`boot-test log` says so rather than letting you guess.

### A warning about the panel that is arithmetic, not opinion

1080×2400 at **120 Hz** needs 2.258 Gbit/s per lane against a 2.5 Gbit/s lane —
**10% headroom**, which is where DSI link training starts failing in ways that
look exactly like driver bugs. Bring the panel up at **60 Hz first** (55%
headroom) and only then raise the refresh.

```sh
./scripts/display-budget.sh moto-g64-5g
```

This is the same arithmetic that `scripts/display-budget.sh` and
`tests/test-display-budget.sh` enforce, and the tool refuses a panel that cannot
fit the declared link rather than rounding it into a yes.

### Getting Android back

Motorola uses virtual A/B, so the kernel you replaced is usually still there:

```sh
adb reboot bootloader
fastboot --set-active=a      # or b
fastboot reboot
```

If that is not enough, flash the stock `boot.img` for your exact ROM the same
way. Nothing outside `boot` was written, so a full ROM reflash should not be
necessary — and if it looks necessary, stop and say so rather than erasing
anything else. `boot-test rollback` prints this on demand.

---

## Route 3 — `no-boot`: check the layout with no phone at all

This one is different in kind: it is not a plan, it is a test that passes today.

```sh
./tests/test-device-panels.sh
```

It reads `DEVICE_PANEL_WIDTH`/`HEIGHT` out of every profile in
`config/devices/` and runs the shell at exactly those geometries — 1080×2400
here, 720×1520 for the Redmi 8A — asserting, with no phone, no bootloader, no
flash and no QEMU:

- the dock band lies entirely inside the screen
- all four dock icons are on screen and evenly spaced
- each dock icon is at least 15% of the panel width, so a thumb can hit it
- a tap at each dock icon's centre hit-tests to that icon, through the real
  `hit_test_dock_icon()` rather than a copy of its arithmetic
- **the dock is actually painted where the hit-test says it is** — compared by
  pixel luminance against the wallpaper strip above it, because a dock drawn at
  a different offset from the one the hit-test uses is beautiful and unusable
- the keyboard area and all 44 keys are inside the screen, and no key is a sliver
- the status-bar keyboard button is on screen and above the keyboard
- every screen actually renders a non-uniform frame

The minimum sizes are checked as a **fraction of the panel**, not in pixels:
720×1520 and 1080×2400 have completely different densities, so a pixel
threshold would mean nothing.

It also renders all fifteen screens at each device's real geometry, so the
result can be looked at rather than only asserted about:

```
build/screens/moto-g64-5g-1080x2400/
build/screens/redmi-8a-720x1520/
```

**Why this matters more than it sounds.** The QEMU dev environment runs at
1024×768 — a landscape shape neither phone has. A shell that lays out perfectly
there can put its dock's bottom edge off a 2400-row screen, and nothing in the
normal test loop would notice. The geometry is read from the profiles rather
than hardcoded, so a test that duplicates the numbers it is checking against
cannot silently keep passing when the hardware spec changes.

This route is verified. It passes for both devices, and it fails when it
should: a deliberately impossible geometry produces six failures and a non-zero
exit.

---

## Which route should you use

| You want to… | Use | Costs you |
| --- | --- | --- |
| See the Astrix UI on the phone today | `android-host` | Nothing. Reversible by deleting one folder. |
| Know whether Astrix's kernel runs on `retin` | `boot-test` | An unlock token, and Android until you flash it back. |
| Know the UI fits this panel | `no-boot` | Nothing. It already runs. |
| Actually boot Astrix OS on `retin` | None of these | An MT6855 display driver that does not exist upstream. |

---

## Status

**Nothing in this repository has been run on physical hardware.** No `retin`
device has ever been connected to a machine with this code on it. Every command
above is written against the real hardware specifications and the real host
tooling, and every one has been executed *here* up to the point where a phone
is required — the bundle builds with no dangling symlinks, the boot image is
assembled and read back by `abootimg` as valid, the refusal gates are
exercised, and the panel geometry test passes for this device.

Verified hardware facts (Dimensity 7025 / MT6855, Mali-G57 MC2, 1080×2400 @
120 Hz, virtual A/B) come from the vendor panel specification and the reference
port's device tree, not from measurement. The DSI blanking percentages in
`config/devices/moto-g64-5g.conf` are an explicit assumption, recorded as one in
the profile and labelled by `display-budget.sh`.

Related: [`DEVICES.md`](DEVICES.md), [`PORTING.md`](PORTING.md),
[`REDMI-8A.md`](REDMI-8A.md).