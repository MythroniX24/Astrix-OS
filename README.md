# Astrix OS

A touch-first mobile operating system for ARM64 Linux, built from the pieces up:

```
  bootloader  ->  Linux kernel  ->  minimal Debian userspace
              ->  systemd  ->  DRM/KMS  ->  Wayland  ->  Astrix compositor
              ->  Astrix Shell  ->  native Linux apps
              ->  Android APK compatibility (Waydroid)  ->  one launcher
```

Astrix is **not** an Android skin, ROM, container, proot chroot or terminal
emulator. It boots its own kernel, runs its own init, starts its own Wayland
display server, and ships its own mobile shell and applications as native ARM64
binaries. Android support is a *compatibility layer on top*, never the base.

---

## Quick start

```bash
./build.sh                 # full build: rootfs -> GUI -> image
./run-qemu.sh              # boot the image in QEMU (ARM64, headless-friendly)
./scripts/screenshot.sh    # capture the running VM's screen over QMP
./scripts/send-input.sh    # drive the VM with taps, swipes, holds and keys over QMP
./tests/run-all.sh         # host test suite
./clean.sh                 # remove build artefacts
```

`build.sh` takes a while the first time (it downloads and unpacks a Debian
minbase rootfs, then cross-builds every GUI binary). Options:

| Command | What it does |
| --- | --- |
| `./build.sh` | Build everything needed for a bootable image. |
| `./build.sh --full` | Also install `config/packages-dev.txt` (git, gcc, rustc, python3, nodejs, …) into the rootfs. |
| `./build.sh --no-kernel` | Skip the kernel build and use Debian's `linux-image-arm64`. |
| `./build.sh --clean` | Start from scratch. |
| `./build.sh --resume` | Re-apply Astrix configuration to an existing rootfs (fast). |
| `./run-qemu.sh --headless` | No window; serial console to `build/qemu-serial.log`. |
| `SSH_PORT=2223 ./run-qemu.sh` | Change the forwarded guest SSH port. |
| `./scripts/screenshot.sh` | While the VM is running: `screendump` over the QMP socket → `build/astrix-screenshot.png`. **Note:** on virtio-gpu this captures the text console, not the compositor's DRM plane — see the header of `scripts/screenshot.sh`. |
| `./scripts/send-input.sh tap 512 538` | While the VM is running: inject a real tap at **normalised** 0..1 coordinates over the QMP socket, so it works at any guest resolution. `swipe`, `hold`, `key`, `pos` are also available. |
| `ASTRIX_SCREEN_W=720 ASTRIX_SCREEN_H=1600 ASTRIX_POINTER_SCALE=1.0 ./scripts/send-input.sh tap 0.5 0.95` | Treat the guest as a 720x1600 portrait panel. `ASTRIX_POINTER_SCALE` compensates libinput pointer acceleration (this guest applies exactly 2.0x). |

#### Driving the running VM

`send-input.sh` is how input is verified end to end: QMP `input-send-event`
→ QEMU input device → evdev → libinput → libseat → wlroots → `wl_pointer` →
the shell's gesture recogniser. Each command echoes what the guest did (see
the serial log, `build/qemu-serial.log`).

```bash
./scripts/send-input.sh tap 0.5 0.5        # centre of the panel
./scripts/send-input.sh swipe 0.5 0.8 0.5 0.2   # up-swipe from lower to upper
./scripts/send-input.sh hold 0.5 0.5 0.6    # long press (opens the power menu)
./scripts/send-input.sh key ret
```

Known limitation, stated up front: only **relative pointer** input is
exercised. `virtio-tablet-device` is not attached because the Debian arm64
guest never binds an evdev node to it, and this QEMU has no `usb-tablet`, so
**true multi-touch is not verified**. See `docs/STATUS.md` §5.

The QEMU panel is 1024x768, not the 720x1600 the shell defaults to. The shell
adopts the real mode at runtime, but injected coordinates are absolute, so
`ASTRIX_SCREEN_W/H` must match the panel or every tap lands arbitrarily:

```bash
export ASTRIX_SCREEN_W=1024 ASTRIX_SCREEN_H=768 ASTRIX_POINTER_SCALE=2.0
./scripts/send-input.sh tap 0.3906 0.7786                  # open Files from the dock
./scripts/send-input.sh swipe-hold 0.5 0.97 0.5 0.55 12 0.8 # app switcher
./scripts/send-input.sh swipe 0.3135 0.60 0.3135 0.44 4      # close that app
```

If QEMU fails to boot with no useful error, pass `--mem 1024`: the default is
2048 MB, which is more than a modest host has.

## Physical devices

**Astrix has never been booted on a physical phone.** QEMU is the only target
that works today, and that is stated plainly rather than buried: neither the
Redmi 8A nor the moto g64 5G has a port, because both are blocked on a display
driver that does not exist in mainline Linux, and neither has an upstream
device tree.

```bash
./scripts/device.sh list              # every known device and its honest status
./scripts/device.sh show redmi-8a     # hardware, blockers, and the path to a boot
./scripts/device.sh check redmi-8a    # is the kernel config actually complete?
./scripts/display-budget.sh --all     # can the panel be driven over MIPI DSI?
./scripts/panel-check.sh              # the 7 display stages of the *running* kernel
./scripts/flash-device.sh redmi-8a --flash   # refuses, and explains why
```

`display-budget.sh` is the first step of the port and the only one that works
today without any hardware: it computes the pixel rate a panel demands and the
per-lane MIPI DSI rate that implies, then tells you whether the link can carry
it. It reports, for example, that the moto g64 5G's 1080x2400 **120Hz** panel
needs 2.258 Gbit/s per lane against a 2.5 Gbit/s lane — a 10% margin, which is
why the guide says to bring it up at 60Hz first. A Redmi 8A panel needs 477
Mbit/s per lane and has 81% headroom, which is a reminder that bandwidth was
never its problem: Adreno 505 has no mainline driver.

The flasher refuses to touch any device that has not been verified booting. That
is deliberate: flashing an OS whose kernel cannot drive the panel leaves a phone
that boots to a black screen, and that damage cannot be undone from here. The
same rule gates `scripts/build-device.sh`, so no flash bundle can even be
assembled. Full detail, including what has been built and what is missing for
each phone, is in [`docs/DEVICES.md`](docs/DEVICES.md); the ordered bring-up
procedure, and how to verify every step over serial/`dmesg` instead of by
staring at the phone, is in [`docs/PORTING.md`](docs/PORTING.md).

### Host requirements

x86-64 Linux with `qemu-system-aarch64`, `qemu-user-static` + `binfmt_misc`
(for cross-building the GUI), `binutils`/`gcc` cross tools, `sgdisk`,
`mkfs.vfat`, `mtools`, `dosfstools`, `parted`, and `debootstrap`.

Every one of these is checked by `scripts/lib.sh:require_binfmt` and by the
`hostcheck` stage of `build.sh`, which fails with an explicit message rather
than a confusing error 180 seconds later.

---

## What is in the box today

| Layer | Status |
| --- | --- |
| Build system (`build.sh`, `scripts/`) | Working, reproducible, tested |
| Debian trixie ARM64 rootfs | Built and bootable |
| systemd as PID 1 | Verified on a real QEMU boot |
| Wayland compositor (wlroots 0.18) | Built, and verified running a real client |
| Astrix Shell (touch UI) | Built, and verified mapping a surface on the compositor |
| System on-screen keyboard | Built: QWERTY, symbols, shift/caps, space/return/backspace. Keys are delivered through `zwlr_virtual_keyboard_v1`, so they reach the focused app instead of stopping at the shell |
| Native apps (terminal, files, sysinfo, settings, package manager, APK manager) | Built and installed as ARM64 ELF binaries |
| Host test suite | 37 tests, all passing |
| Android APK execution (Waydroid) | **Not implemented** — see `docs/ANDROID.md` |
| Physical-device boot | **Not implemented** — no phone has ever been booted. Profiles and a gated flasher exist for the Redmi 8A and moto g64 5G; see `docs/DEVICES.md` |

The last two rows are the honest ones. `docs/STATUS.md` is the single source of
truth for what is verified, what is merely built, and what does not exist yet.
Nothing in this repository claims hardware support that has not been tested on
hardware.

---

## Repository layout

```
config/            os.conf (single source of truth), package lists, kernel config
protocol/          vendored wlroots protocol XMLs (mirrors, not forks)
gui/compositor/    Astrix compositor: Wayland display server, input, xdg-shell
gui/shell/         Astrix Shell: touch UI, gestures, app lifecycle, rendering
gui/ui/            shared 2D canvas + bitmap font used by the shell and apps
apps/              native applications (terminal, files, sysinfo, settings, …)
system/            systemd units and the session/boot-report helpers
rootfs/etc/        files overlaid into /etc during the rootfs build
scripts/           the build pipeline
qemu/run.sh        QEMU launcher: machine, devices, display, serial
tests/             host test suite
docs/              architecture, design, security, Android, status
```

### The three scripts you will actually use

`scripts/build-rootfs.sh`
:   debootstrap a minbase Debian trixie rootfs, install `config/packages.txt`,
    apply the Astrix `/etc` overlay, create the `astrix`/`android` users and
    the groups the session needs.

`scripts/build-gui.sh`
:   cross-compile every Astrix binary for `aarch64` using the *target's* own
    toolchain inside the rootfs, and install the results into
    `/usr/bin` and `/usr/lib/astrix/gui`.

`scripts/build-image.sh`
:   pack the rootfs into a GPT disk image: an ESP written with `mtools` (the
    build host has no FAT kernel module), an ext4 root, and post-install
    verification that aborts the build if anything is missing.

---

## Design rules

These are not aspirations; they are constraints the build enforces.

1. **Never fake a feature.** If a thing is not implemented, it is absent from
   the UI and called out in `docs/STATUS.md`. A button that does nothing is
   worse than no button.
2. **The GUI never runs as root.** `astrix` (uid 1000) owns the session. Only
   narrowly-scoped helpers ever run privileged, and they are separate programs.
3. **One display stack.** Wayland + wlroots + a KMS backend. No X11, no
   display manager, no window manager.
4. **Reproducible builds.** `SOURCE_DATE_EPOCH` is pinned in `config/os.conf`;
   the kernel, package set and toolchain versions are pinned.
5. **Every milestone is testable.** A milestone that cannot be booted, or
   asserted, is not finished.
6. **QEMU and physical devices never share a path.** Everything under `qemu/`
   and `scripts/build-image.sh` produces a virtual disk image. Nothing in this
   repository writes to physical storage.

---

## Testing

```bash
./tests/run-all.sh            # everything
./tests/run-all.sh --fast     # reuse the cached Debian package index
```

Six stages, currently **31 passing / 0 failing**:

- shell syntax for every script in the repo
- `test-ui` — canvas primitives, clipping, text layout
- `test-gestures` — tap / swipe / long-press recognition, navigation state
- `test-packages` — every package in `config/packages.txt` really exists for
  `arm64` in Debian trixie (checked against the real archive index)
- `render-screens` — renders every shell screen to PPM so a human can look at
  what changed
- `test-wayland-session` — **end-to-end**: the real ARM64 compositor and the
  real ARM64 shell, run under `qemu-user`, asserting that the socket appears,
  the `xdg_toplevel` is accepted, the surface is mapped, the app id is
  `org.astrix.Shell`, and no protocol error is raised
- `test-compositor-focus` / `test-pointer-retarget` — source-shape checks for
  two bugs that each took a real boot to find: a NULL dereference on every app
  exit, and the compositor stranding a client mid-gesture when the input target
  moves. Both run against pre-fix fixtures to prove they still detect the bug
- `test-devices` — device profiles cannot claim a boot nobody observed, and
  `flash-device.sh` must refuse an unsupported device

The last one is the important one: it is the test that catches "the shell is
dead and nothing says why", which is the failure mode that matters most for an
OS whose display *is* the product.

Boot-time correctness is checked separately by `scripts/build-image.sh`, which
verifies the finished image (init → systemd, `/etc/os-release` → Astrix, the
GUI binaries present and executable) and refuses to produce an image that fails.

---

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — the layers, and why each one
  was chosen
- [`docs/DESIGN.md`](docs/DESIGN.md) — UI, input, app model, session lifecycle
- [`docs/SECURITY.md`](docs/SECURITY.md) — the trust model, hardening, and what
  is *not* yet enforced
- [`docs/ANDROID.md`](docs/ANDROID.md) — the Waydroid compatibility design and
  its current (unimplemented) state
- [`docs/DEVICES.md`](docs/DEVICES.md) — **which phones Astrix boots on (none
  yet)**, what is blocking each one, and how to test without real hardware
- [`docs/STATUS.md`](docs/STATUS.md) — **read this first**: verified vs. built
  vs. not implemented

## Licence

Not yet chosen. Add a `LICENSE` before distributing anything.
