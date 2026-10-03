# Astrix OS on real hardware

This page answers one question directly: **does Astrix boot on a real phone?**

**No.** As of this commit, Astrix has never been booted on any physical
device. It boots on QEMU, verified on every run of the test suite and on
every manual boot.

That sentence is the most important thing on this page, so it is the first
thing on this page. Everything below is the honest account of what stands
between the current state and a boot on the two target phones, and of what has
already been built to get there.

```console
$ ./scripts/device.sh list

  PROFILE                  BOOT STATUS        DEVICE
  ------------------------ ------------------ ------
  moto-g64-5g              unsupported        moto g64 5G
  redmi-8a                 unsupported        Redmi 8A

    Only a profile with DEVICE_VERIFIED_BOOT=yes has been seen booting by anyone.
```

## Why the tooling refuses to flash

There is a `scripts/flash-device.sh`, and if you run it on either phone today
it will refuse:

```console
$ ./scripts/flash-device.sh redmi-8a --flash

  Device:  Redmi 8A (olive)
  Boot:    unsupported (verified: no)

  Refusing to flash: this device is not supported by Astrix.

      status=unsupported verified-boot=no
      Nothing in this repository has booted on this hardware. Flashing it now
      would very likely leave a phone that shows nothing.

    What is actually blocking it:
    - no-mainline-display-driver
    - no-mainline-dt
    - locked-bootloader
```

This is deliberate. Flashing an OS whose kernel cannot drive the panel leaves
a phone that boots to a black screen, with a working bootloader behind an OS
that no longer starts. That damage is not something Astrix can undo for you.
The gate is enforced in code, and `tests/test-devices.sh` runs the flasher
with no device attached and fails the build if it does not refuse.

`scripts/build-device.sh` is gated the same way, so a flash bundle cannot be
assembled for a phone nobody has booted from — the bundle is the dangerous
part, not the flash.

## What the display port needs, before it needs a driver

The first question in any display port is arithmetic, and it can be answered
today without either phone:

```console
$ ./scripts/display-budget.sh --all --quiet
moto-g64-5g: OK but tight  needs 2.258 Gbit/s/lane of 2.500 Gbit/s (10% headroom)
redmi-8a: OK  needs 476.7 Mbit/s/lane of 2.500 Gbit/s (81% headroom)
```

The moto g64 5G's 1080x2400 **120Hz** panel needs 2.258 Gbit/s per lane against a
2.5 Gbit/s lane. That fits, by 10%, and 10% is where DSI link training starts
failing in ways that look like driver bugs. Bring the panel up at 60Hz first
(1.13 Gbit/s per lane, 55% headroom). The Redmi 8A has ample headroom and still
cannot boot, because bandwidth was never its problem.

The full ordered path — bootloader, serial console, device tree, and the seven
DRM stages with the `dmesg` and `/sys/class/drm` evidence that proves each one —
is in [`docs/PORTING.md`](PORTING.md).

## What actually works today

| Target | Status | Evidence |
| --- | --- | --- |
| **QEMU (`virt`, arm64)** | **Boots.** | Every `./build.sh`; `tests/test-wayland-session.sh` runs the real compositor and shell under emulation; ~10 min to a mapped shell, ~190 s to `first-boot-complete`. |
| Redmi 8A (olive) | Does not boot — but the GUI **can** be run on it, and the kernel **can** be boot-tested on it | Blockers below; two routes in [`docs/REDMI-8A.md`](REDMI-8A.md). |
| moto g64 5G (retin) | Does not boot — but the GUI **can** be run on it, the kernel **can** be boot-tested on it, and the panel layout **is** verified with no phone | Blockers below; three routes in [`docs/MOTO-G64-5G.md`](MOTO-G64-5G.md). |

The QEMU path is not a toy: the same rootfs, the same compositor, the same
shell and the same apps run there, over a real DRM/KMS device, a real libinput
stack and real Wayland sockets. What is *not* exercised is anything that
needs a touchscreen, a modem or a real panel.

## Redmi 8A (olive) — Qualcomm Snapdragon 439

Verified hardware: Snapdragon 439 (MSM439), Adreno 505, 720×1520 panel,
Xiaomi A/B partition layout.

Three blockers, none of which is a configuration problem:

1. **No mainline display driver for Adreno 505.** The GPU is driven by Qualcomm's
   closed `msm` driver, which only builds against a downstream kernel and needs
   a vendor userspace blob. There is no open alternative, so there is no way to
   light this panel with upstream Linux. `CONFIG_DRM_MSM=y` would fail to build
   — which is exactly why it is listed under `DEVICE_DOWNSTREAM_ONLY_FEATURES`
   rather than being added to the kernel config to look thorough.
2. **No upstream device tree for olive.** Nothing describes this board's
   regulators, clock trees, PHYs or interrupt wiring.
3. **Locked bootloader.** Unlocking requires Xiaomi's Mi Unlock tool, a
   Xiaomi account bound to the device, and a waiting period that Xiaomi has
   extended without notice. Nothing Astrix does can shorten it.

The realistic path runs through a community port's kernel — postmarketOS has an
`olive` port and LineageOS maintains `kernel_olive`. Either way Astrix would be
shipping someone else's kernel, and that trade-off should be made explicitly
rather than quietly.

### What *is* possible on this phone today

Unsupported is not the same as unusable, and collapsing the two would waste the
one device most people actually have in their pocket. `scripts/port-redmi-8a.sh`
implements two routes, named for what they prove rather than for how they feel:

| Route | What it does | Proves | Costs you |
| --- | --- | --- | --- |
| `android-host` | Runs the Astrix **userspace** as a `proot` chroot on the phone's own Android kernel, output bridged through Termux:X11 → SurfaceFlinger | The compositor, shell, keyboard and apps work on this SoC and panel | Nothing. No unlock, no flash, no partition touched. |
| `boot-test` | Flashes Astrix's **kernel** to `boot` and reads the USB serial console | Astrix's kernel starts on `olive` and reaches userspace | Unlock (erases the phone), and Android until you flash it back. |

```sh
./scripts/port-redmi-8a.sh status
./scripts/port-redmi-8a.sh android-host pack && ./scripts/port-redmi-8a.sh android-host push
```

Neither route boots Astrix OS, and the script says so every time it runs. Full
detail, including exactly what has and has not been executed against a physical
`olive`: [`docs/REDMI-8A.md`](REDMI-8A.md).

## moto g64 5G (retin) — MediaTek Dimensity 7025

Verified hardware: Dimensity 7025 (MT6855, 6 nm), 2× Cortex-A78 + 6× A55,
Mali-G57 MC2, 1080×2400 @ 120 Hz, Motorola virtual-A/B layout.

This one is genuinely closer to portable than the Redmi 8A, and the difference
is worth understanding: **Panfrost is a real open-source DRM driver in
mainline Linux for this GPU.** So Astrix has a GPU path here that it does not
have for the Adreno 5xx.

It still does not boot, for three reasons:

1. **MediaTek's MT6855 display pipeline has no mainline driver.** The DSI
   controller, the panel driver, and the GPIO sequence for panel reset and
   enable-enable are all downstream-only. Panfrost gives a GPU; a GPU with
   nothing to present a scanout to is not a display. This is the hard blocker.
2. **No upstream device tree for retin.**
3. **No bootloader.** Motorola's official unlock (a token requested with the
   device's IMEI, emailed back) is much friendlier than Xiaomi's, and some
   retail variants are refused outright with "device not qualified". But an
   unlocked Motorola bootloader only knows how to chainload an Android boot
   image; Astrix's own boot chain has to be built.

An ordered path, each step independently verifiable, is recorded in
`DEVICE_PORT_STEPS` in the profile:

1. An upstream MediaTek MT6855 display driver
2. A `retin` device tree
3. An Astrix bootloader that an unlocked Motorola bootloader will chainload
4. Panel calibration (timings, porch values, gamma)
5. The userspace port

### What *is* possible on this phone today

This is the closer of the two boards, and "closer" is not "working". There are
three routes, named for what they prove rather than for how they feel:

| Route | What it does | Proves | Costs you |
| --- | --- | --- | --- |
| `android-host` | Runs the Astrix **userspace** as a `proot` chroot on the phone's own Android kernel, output bridged through Termux:X11 → SurfaceFlinger | The compositor, shell, keyboard and apps work on this SoC at 1080×2400 | Nothing. No unlock, no flash, no partition touched. |
| `boot-test` | Flashes Astrix's **kernel** to `boot` and reads the USB serial console | Astrix's kernel starts on `retin` and reaches userspace | An unlock token (IMEI request, emailed back), and Android until you flash it back |
| `no-boot` | `./tests/test-device-panels.sh` — renders every screen at this panel's exact geometry and asserts the dock, keyboard and status bar fit and accept taps | That the UI actually fits 1080×2400 | Nothing. **This one is a pass/fail test today.** |

```sh
./scripts/port-moto-g64-5g.sh status
./tests/test-device-panels.sh
```

None of the three boots Astrix OS, and the script says so every time it runs.
Full detail, including exactly what has and has not been executed against a
physical `retin`: [`docs/MOTO-G64-5G.md`](MOTO-G64-5G.md).

## What has been built for these ports

The honest answer is: the parts that can be built without the missing drivers,
plus the machinery that stops the missing parts being forgotten.

- **`config/devices/*.conf`** — verified hardware facts per device, the
  kernel features the SoC needs, the features that need downstream drivers,
  and an explicit `DEVICE_BOOT_STATUS` / `DEVICE_VERIFIED_BOOT`.
- **`scripts/device.sh`** — `list`, `show`, `check`, `kernel-features`,
  `display-budget`. `check` verifies each profile is internally consistent,
  that every upstream kernel feature it claims to need is actually enabled in
  `config/kernel/config`, and that the panel can physically be driven over the
  declared MIPI DSI link. It is a real check: run it before the config
  fragments were added and it fails on both phones.
- **`scripts/display-budget.sh`** — the arithmetic that has to be right before
  any driver work: panel pixel rate, aggregate DSI payload, required per-lane
  rate, and headroom against the link. It also *refuses*: a panel that cannot
  fit on the declared link exits non-zero, and `device.sh check` fails on it.
  Blanking percentages are labelled as assumptions in the output and in the
  profiles, because no porch values have ever been read off a working boot.
- **`scripts/flash-device.sh`** — refuses to flash any device that has not been
  verified booting, refuses a locked bootloader, and requires a typed
  confirmation of the codename. `ASTRIX_ALLOW_UNVERIFIED_FLASH=1` opens the
  first gate, loudly and only when explicitly set: a gate that cannot be opened
  is not a gate, and refusing every attempt is how no port ever starts.
- **`scripts/port-<device>.sh`** — the device-specific routes, one thin wrapper per
  profile over the shared machinery in `scripts/port-common.sh`: pack and push the
  Astrix userspace to run on Android's own kernel, or build and stage a
  boot-test image. Same override requirement for the staging half. See
  [`REDMI-8A.md`](REDMI-8A.md) and [`MOTO-G64-5G.md`](MOTO-G64-5G.md).
- **`tests/test-device-panels.sh`** — the only route that is a *test* rather than
  a plan. Runs the shell at every profile's real panel geometry and asserts the
  dock, keyboard and status bar fit and accept taps, with no phone, no
  bootloader and no QEMU. The geometry is read from the profiles so it cannot
  drift from the hardware, and the renders land in
  `build/screens/<device>-<w>x<h>/`. This matters because the QEMU dev
  environment is 1024×768 — a shape neither phone has.
- **`scripts/build-device.sh`** — the other half of the same promise. It
  assembles `build/device/<device>/` (kernel, dtb, rootfs) and refuses for the
  same reason, so a bundle nobody has booted from cannot be built by accident.
- **`config/kernel/config`** — the upstream drivers both SoCs need
  (Qualcomm UFS/QMP/RPMH/watchdog/clk/thermal; MediaTek UFS/PHY/DVFSRC/I2C/SMI;
  Panfrost). Enforced by the check above.
- **`tests/test-devices.sh`** — fails the build if a profile claims support
  without a verified boot, proves the flasher refuses, proves the override is
  what opens the gate (and that it announces itself when it does), and proves
  every device has a port tool that keeps its own gate.

## Getting to a real boot

If you want to do the port rather than read about it, the shortest honest path
is the moto g64 5G, and the first milestone is small and checkable:

> Boot a mainline kernel on retin to a serial console, with no display, and
> have it say `Hello` over UART.

That needs the bootloader unlocked and a device tree — not a display driver.
Getting that far proves the storage, clock, regulator and DT layers are right,
and it is the step after which the display driver becomes the only remaining
unknown. It is also, notably, achievable without Astrix's compositor being
involved at all.

For the Redmi 8A, the same first milestone is blocked on the bootloader, and
the bootloader is blocked on a Xiaomi account and a waiting period that Astrix
has no influence over. `scripts/port-redmi-8a.sh boot-test build` / `stage` /
`log` is that milestone, packaged: it boots Astrix's kernel on `olive` and
reads the proof off `ttyMSM0`, with the screen staying dark throughout because
there is no Adreno 505 driver to draw with.

The full ordered bring-up — serial console first, then device tree, then the
seven-stage DRM pipeline with the `dmesg`/`/sys/class/drm` evidence that proves
each stage — is in [`docs/PORTING.md`](PORTING.md). It also contains a bring-up
log template, because a port with an empty log is a port that quietly stopped.

## How to test without real hardware

This is the part that works today, and it is what `./build.sh` produces:

```console
./build.sh                      # build everything, run the host test suite
./run-qemu.sh                   # boot it, with a window
./run-qemu.sh --headless --mem 1024   # headless; note the memory, see below
```

Then, with the VM running:

```console
export ASTRIX_SCREEN_W=1024 ASTRIX_SCREEN_H=768 ASTRIX_POINTER_SCALE=2.0

# Open the Files app from the dock.
./scripts/send-input.sh tap 0.3906 0.7786

# Swipe up and hold from the bottom band: opens the app switcher.
./scripts/send-input.sh swipe-hold 0.5 0.97 0.5 0.55 12 0.8

# Swipe up on a recents card: closes that app.
./scripts/send-input.sh swipe 0.3135 0.60 0.3135 0.44 4
```

The QEMU panel is 1024×768, not the 720×1600 the shell defaults to — the shell
adopts the real mode at runtime, but the injected coordinates are absolute, so
`ASTRIX_SCREEN_W/H` have to match the panel or every tap lands somewhere
arbitrary.

Two things about QEMU worth knowing before you spend an afternoon on it:

- **Pass `--mem 1024`.** QEMU's default is 2048 MB, which is more than a modest
  host has, and the guest will fail to boot in a way that looks like an Astrix
  bug.
- **You cannot screenshot the UI.** QMP's `screendump` captures the text console,
  not the compositor's DRM plane. To see what the UI looks like, render it on
  the host: `./tests/run-all.sh` writes `build/screens/*.ppm`.

`ASTRIX_POINTER_SCALE=2.0` exists because QEMU's virtio-mouse is
**relative-only** — it has no absolute axes, so coordinates are accumulated and
drift. It is a workaround for the emulator, not something the OS needs on real
hardware.