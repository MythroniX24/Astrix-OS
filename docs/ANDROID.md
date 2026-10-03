# Astrix OS — Android compatibility

## Status: not implemented

**No Android APK currently runs on Astrix OS.** This document describes the
design that is intended, and the exact reasons it has not been built. The
`astrix-apk-manager` binary in the image is a UI shell; it presents the
compatibility surface, and it does not execute anything. Nothing in the UI
claims otherwise, and nothing in this repository should be read as claiming
APK support.

What *is* built and verified is listed at the end.

---

## 1. What "Android compatibility" means here

The goal is that a user installs an APK and it runs, from the same launcher as
a native Astrix app, and the user does not have to care which kind it is.

Explicitly **not** the goal:

- shipping Android System Services, or `libandroid_runtime.so`, or the
  framework, inside Astrix;
- making Astrix a thin shim over an Android userspace;
- using a chroot with `proot` and calling it a phone.

Astrix is a Linux system. Android apps are Linux ELF binaries that expect a
specific userspace contract. Providing that contract is a compatibility
problem with a known answer, and the answer is Waydroid.

---

## 2. Why Waydroid

Waydroid runs unmodified Android APKs on a Wayland host by supplying exactly
the pieces an Android app expects and a Wayland system does not have:

| Android expects | Waydroid provides | Astrix already has |
| --- | --- | --- |
| A Bionic-linked userspace | An Android userspace image, bind-mounted | — (this is the work) |
| `binder`, `ashmem`, `dmabuf` | Kernel modules + `lxc` | Kernel config, `CONFIG_ANDROID` |
| An OpenGL ES stack with Android extensions | Mesa, exposed over `virgl`/`lxc` | Mesa (`mesa-vulkan-drivers`, `libgl1-mesa-dri`) |
| An input stack speaking Android `InputDevice` | `uinput` devices created by the container | libinput + seatd, and our compositor |
| Audio via OpenSL/AAudio | ALSA/PulseAudio from the host | PipeWire + WirePlumber |
| A display server speaking Android `SurfaceFlinger` | A shim translating to Wayland | Astrix compositor |

The last row is the important one: **Waydroid replaces SurfaceFlinger**, and
surfaces are drawn into our own wlroots compositor through its xdg-shell path.
That is why the whole thing can be coherent with the rest of the system
instead of being a second, parallel display stack.

The alternative — writing an Android compatibility layer from scratch — is
years of work, is legally and practically constrained by bionic and the
framework's use of unexported kernel interfaces, and would be a worse result
than the one that already exists.

---

## 3. Intended architecture

```
   ┌──────────────────────────────────────────────────────────────┐
   │  Astrix Shell / Launcher                                    │
   │    native apps  ·  Android apps (one grid, one icon treatment)│
   └───────────────┬───────────────────────────┬──────────────────┘
                   │ xdg-shell                │ xdg-shell
   ┌───────────────▼─────────────┐  ┌──────────▼──────────────────┐
   │  native Wayland clients      │  │  Waydroid                    │
   │  (astrix-*, any Wayland app) │  │   ├─ Android userspace      │
   └─────────────┬────────────────┘  │   │   (system.img, ext4)     │
                 │                   │   ├─ binder/ashmem/dmabuf    │
                 │                   │   ├─ libinput → Android      │
   ┌─────────────▼─────────────────┐  │   │   input devices          │
   │  ASTRIX COMPOSITOR            │◀─┤   └─ Mesa GLES → Wayland   │
   │   wlroots 0.18, xdg-shell,     │  └─────────────────────────────┘
   │   scene graph, DRM/KMS        │        ▲              ▲
   └─────────────┬─────────────────┘        │              │
                 │                         │        lxc / user namespaces
   ┌─────────────▼─────────────────┐  ┌────┴──────────────┴───────┐
   │  DRM/KMS → panel              │  │  unprivileged `android` uid │
   └───────────────────────────────┘  └────────────────────────────┘
```

Key properties of this design:

- **The compositor is shared.** Android surfaces are compositor surfaces, not
  a nested display. Scrolling between an Android app and the Astrix home
  screen is a compositor animation, not a context switch.
- **Android runs as its own user**, `android`, created by `build-rootfs.sh` —
  already done. Android apps never run as `astrix`.
- **The container is unprivileged**: user namespaces, `lxc`, no root helper
  beyond what polkit authorises at install time.
- **The compatibility surface is explicit in the launcher**, so the security
  consequence of installing an APK is visible to the user rather than implied.

---

## 4. What has to be built

| Step | State |
| --- | --- |
| `android` user and group, non-root | **Done** — created in `build-rootfs.sh` |
| `ASTRIX_ANDROID_ENABLED` / image name in `config/os.conf` | **Done** — set, but nothing consumes them yet |
| Kernel: `CONFIG_ION`, `CONFIG_ANDROID_BINDER`, `CONFIG_ASHMEM`/`CONFIG_MEMFD`, `CONFIG_DMABUF` | **Not done** — not enabled in `config/kernel/config` |
| `lxc` + user namespaces + `CONFIG_USER_NS` | Package present in the host toolchain list; runtime config not done |
| Waydroid userspace image and the service to start it | **Not done** |
| Mesa with the Android-required GLES extensions exposed to the container | **Not done** |
| Input device translation (Wayland `libinput` → Android `InputReader`) | **Not done** |
| Audio (AAudio/OpenSL → PipeWire) | **Not done** |
| Binder wiring from the host | **Not done** |
| `astrix-apk-manager` wiring to Waydroid's session | **Not built** — the binary compiles and runs; it is a UI, not a client |
| Camera, sensors, location, telephony passed through | **Not done**, and these are the hardest parts |

The honest summary: the *plumbing that is not Android-specific* is done; the
*plumbing that is Android-specific* has not been started.

---

## 5. The parts that are genuinely hard

Worth being explicit about, because "just add Waydroid" undersells these:

**Camera.** Android camera apps go through `android.hardware.camera2` →
`Camera2` → the HAL. A container has to pass a real V4L2 or DRM pipeline
through as that HAL. The Astrix camera service does not exist yet, so there is
nothing to pass through. This is a multi-month piece of work on its own.

**Telephony and location.** The HAL interfaces for these expect a modem and a
GPS chip. A phone has them; QEMU does not; and no part of this has been built
for either case.

**Sensor fusion.** Accelerometer, gyroscope, proximity, light. Each needs a
`hw-sensor` HAL plugin and a kernel driver binding.

**Memory.** Android apps expect a lot of shared memory and a specific
`ashmem`/dmabuf model. Getting `MemoryDenyWriteExecute` and the rest of the
compositor's sandboxing to coexist with Android's JIT is itself a piece of
work.

---

## 6. Why it is not implemented yet

Not for lack of enthusiasm. The reasons are ordering:

1. **The native stack had to be real first.** A compatibility layer on top of
   a system whose own shell does not start is a compatibility layer for
   nothing.
2. **It cannot be verified honestly on the available hardware.** This project
   is developed on a single x86-64 machine with QEMU. QEMU has no camera, no
   modem, no sensors, and a software GL stack. Claiming "Android compatibility
   works" on the basis of a QEMU test would be exactly the kind of untested
   claim this project refuses to make.
3. **It is the highest-risk area for the security model.** Introducing a
   container, a second userspace and a broad hardware passthrough is the point
   at which a phone OS most often becomes insecure. It should be added when
   there is a screen lock and per-app sandboxing to contain it, not before.

---

## 7. What exists today

Verified, and unrelated to Android:

- the full native session: compositor → shell → apps, on a real ARM64 boot;
- real input: QMP-injected events travel the guest's evdev → libinput → seat →
  shell path and open apps, switch apps and close them;
- real typing: the system on-screen keyboard delivers keystrokes to whichever
  client holds keyboard focus, verified on a booted VM (`on-screen key 'a'` →
  `key 38 pressed`) — which is the same path a Waydroid window's text fields
  would use, but see below;
- `astrix-apk-manager` exists and is installed, as a UI that shows the
  compatibility surface and does not yet execute anything.

Two prerequisites for running an APK are now genuinely in place, and neither
is the blocker it used to be. The input story — the hardest part of hosting an
Android userspace on a Wayland compositor, because Android synthesises its own
keyboards and expects to own input — is solved for the native case: Astrix has
a seat that grants keyboard focus unconditionally, and a system keyboard that
can type into any focused client. The remaining work is containment, not
input: the unprivileged `android` user exists, the Waydroid image does not, and
nothing is mounted or started.

If you are reading this to decide whether Astrix can run an APK today: **it
cannot.**
