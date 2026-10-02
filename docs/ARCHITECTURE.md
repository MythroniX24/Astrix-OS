# Astrix OS — Architecture

This document describes what Astrix OS is made of and, where it matters, why
each layer was chosen over the alternatives that were available.

The short version: Astrix is a **stack of small, auditable pieces**, each of
which is a real Linux project, wired together by a small amount of Astrix code
that only ever does the thing the layer below it cannot do itself.

---

## 1. The stack

```
┌───────────────────────────────────────────────────────────────┐
│  Android apps (APK)                                           │  ← compatibility only
│  ┌─────────────────────────────────────────────────────────┐  │
│  │  Astrix native apps (Wayland clients, ARM64 C)          │  │
│  │  Shell · Terminal · Files · Settings · Sysinfo · Store   │  │
│  └─────────────────────────────────────────────────────────┘  │
├───────────────────────────────────────────────────────────────┤
│  Astrix Shell — touch UI, gestures, app lifecycle, launcher  │  ← written here
│  Astrix Compositor — Wayland display server (wlroots 0.18)   │  ← written here
├───────────────────────────────────────────────────────────────┤
│  Wayland · libinput · libxkbcommon · seatd · rtkit            │
├───────────────────────────────────────────────────────────────┤
│  systemd · dbus · NetworkManager · PipeWire/WirePlumber      │
├───────────────────────────────────────────────────────────────┤
│  Linux kernel (6.12 LTS) · DRM/KMS · Mesa · exFAT/ext4        │
├───────────────────────────────────────────────────────────────┤
│  Debian trixie minbase, arm64 — glibc, apt, coreutils, …     │
├───────────────────────────────────────────────────────────────┤
│  U-Boot / systemd-boot → bootloader partition (ESP)          │
├───────────────────────────────────────────────────────────────┤
│  ARM64 SoC, eMMC/UFS                                        │
└───────────────────────────────────────────────────────────────┘
```

Everything below the two bold rows is off-the-shelf Debian or upstream Linux.
Everything in the bold rows is Astrix code, and it is small on purpose: roughly
ten thousand lines of C in total, so that a person can read all of it.

---

## 2. Why each layer

### 2.1 Bootloader

**Chosen:** U-Boot on a physical device, `systemd-boot` for the QEMU image.

The ESP (`/boot/efi`) is written with `mtools` rather than mounted, because the
build host has no FAT kernel module. The image gets a `loader.conf` pointing at
`/vmlinuz` + `/initrd.img` on the ESP and the root by UUID, so a kernel update
is an A/B slot switch rather than a rewrite of a running root.

**Not chosen:** Android's `boot.img` + LK/AVB. It is a locked, vendor-signed
chain, and it is the single biggest reason an "Android-like" system can never
be a real independent OS.

### 2.2 Kernel

Debian's `linux-image-arm64` (6.12 LTS) for the QEMU milestone so the OS is
bootable before the in-tree build exists. `config/kernel/config` is the config
for the in-tree kernel that replaces it later.

Enabled because a phone needs them: `CONFIG_DRM`, the panel and touch drivers,
`CONFIG_INPUT_EVDEV`, cgroups v2, namespaces, `CONFIG_SECURITY_APPARMOR`,
`CONFIG_SECURITY_LOCKDOWN_LSM`, ext4, exFAT, and the suspend paths.

The kernel is a *dependency*, not a contribution. Astrix does not fork Linux;
it configures it.

### 2.3 Userspace: Debian, and why not anything else

The choice of base distribution is the most consequential decision in the
project, and it is deliberate:

| Base | Verdict |
| --- | --- |
| **Debian trixie minbase** | **Chosen.** glibc, apt, a signed archive, 30 years of ABI stability, and a complete `arm64` port. |
| Ubuntu | Rejected: snaps, apparmor-by-default opinionation aimed at servers, and a release cadence that churns underneath you. |
| Alpine | Rejected: musl breaks Android's bionic-linked binaries and much of GNOME-adjacent software, and the musl/musl-cross toolchain is a worse host for a GUI toolchain. |
| Arch / postmarketOS | Rejected for the shipping image: rolling release means an OS that changes under you, and postmarketOS is a *distribution on top of* Alpine — using it would make Astrix a derivative of a derivative while claiming to be from scratch. |
| Android (AOSP) | Rejected outright. It is the thing Android compatibility is layered on top of, not the thing being replaced. |
| proot / chroot of a distro | Rejected: proot is a `ptrace` shim, not a kernel, not a bootloader, and gives no real isolation. It is a demo, not an OS. |

`config/packages.txt` is deliberately short (about 70 packages). No desktop
environment, no display manager, no window manager. Everything in that list has
to be justifiable as "a phone needs this".

### 2.4 Init: systemd

systemd is not a stylistic choice; it is load-bearing.

- **Ordered, restartable units** mean the session is a dependency graph, not a
  pile of `&`-backgrounded shell scripts. When the shell dies it restarts; when
  the compositor dies, the shell is torn down and restarted cleanly.
- **`Type=oneshot` + `RemainAfterExit`** gives an "autologin" that is a real
  transaction: it either finishes or fails visibly.
- **`StateDirectory=`, `RuntimeDirectory=`, `DynamicUser`** give per-service
  state with correct ownership, instead of every component inventing its own
  path under `/tmp`.
- **`systemd-boot-report.service`** prints unit states, journals and device
  listings to the console on every boot. This is not decoration: it is what
  turned two completely silent session failures into one-line diagnoses, and
  it is the reason this project can debug a display stack that has just failed
  to display anything.

### 2.5 Display: DRM/KMS → Wayland → wlroots → Astrix compositor

```
     ┌──────────────┐
     │  ASTRIX      │   xdg-shell client (shell, apps, any Wayland app)
     │  COMPOSITOR  │   1200 lines, C, wlroots 0.18
     └──────┬───────┘
            │ wlroots backend
     ┌──────▼───────┐
     │  Mesa / pixman│   GPU or software
     └──────┬───────┘
     ┌──────▼───────┐
     │  DRM/KMS     │   card0, renderD128
     └──────┬───────┘
     ┌──────▼───────┐
     │  panel       │   720×1600 portrait
     └──────────────┘
```

The compositor does the minimum a phone display server must do: take over the
seat's DRM master, accept `xdg_shell` toplevels, run a scene graph, and turn
libinput events into gestures. It does **not** implement window management in
the desktop sense (no tiling, no repositioning, no per-window decorations) —
on a phone, the shell is the only client that matters and the OS draws the
frame.

Two details that were real bugs and are now load-bearing, because they are what
"it runs" actually depends on:

1. **A `wlr_surface` is not `mapped` until it is in a scene tree.** Attaching
   `new_toplevel->surface` to `wlr_scene_tree_create(&scene->tree)` in
   `handle_new_toplevel` is not optional; without it the client waits forever
   for a frame callback that never comes, and the shell is simply invisible
   with no error anywhere.
2. **The first `wlr_xdg_surface_schedule_configure` must be sent from the commit
   handler.** A client that receives no initial configure will not draw. The
   compositor tracks a per-toplevel `configured` flag for this.

The socket name is **fixed** (`astrix-0`, via `ASTRIX_SOCKET`) rather than
left to `wl_display_add_socket_auto`. Both units then name the same socket
explicitly, so a mismatch is a readable configuration error instead of a
session that starts and connects to nothing.

### 2.6 Input: libinput + seatd

`seatd` owns the "seat0" logind seat. It is the only session component that may
sit on `logind`'s seat list without a display manager, and it is what makes
`astrix-compositor.service` able to `Requires=seatd.service` and get a real
input device. The compositor attaches to the seat's keyboard, pointer and
touch devices and feeds them into the shell as pointer events; gesture
recognition is Astrix code, in `gui/shell/src/gesture.c`, and is unit-tested on
the host.

### 2.7 Networking, audio, power

Stock Debian services, started by systemd, not by Astrix:

- **NetworkManager + systemd-resolved + iwd** — the phone has to find a network
  without a login.
- **PipeWire + WirePlumber** — one audio stack that works for both the
  compositor's sinks and the app clients.
- **UPower + TLP** — battery reporting and platform profiles.

Astrix's job is only to present them to the user: `astrix-settings` and
`astrix-sysinfo` talk to them over D-Bus.

### 2.8 Applications

Every Astrix app is a **Wayland client** linking `libwayland-client` and
`xdg-shell`, drawn with the same shared 2D canvas (`gui/ui`). There is no
toolkit, no GTK, no Qt, no Electron. A phone OS with a 30 MB app runtime would
be an absurdity.

The shared `gui/ui` canvas is the reason the whole UI layer is ~10 kLOC: a
software rasteriser, a bitmap font, clipping, and rounded rectangles. It is
deliberately not accelerated — the compositor does the scanout, and a phone
UI made of solid rounded rectangles does not need a shader.

---

## 3. Process and privilege model

```
  PID 1  systemd
    ├── astrix-session.service      (root, oneshot)  autologin + seat setup
    │     └── astrix-session.target
    │           ├── astrix-compositor.service   (User=astrix)  the session
    │           │     • CAP_SYS_NICE for RTKit
    │           │     • /dev/dri/* rw, nothing else
    │           └── astrix-shell.service       (User=astrix)  the UI
    │                 └── app clients, launched by the shell
    ├── astrix-boot-report.service  (root, oneshot)  console diagnostics
    ├── seatd.service               (root)
    ├── NetworkManager / systemd-resolved
    └── PipeWire / WirePlumber
```

Rules enforced in the units themselves:

- **Nothing graphical runs as root.** `astrix` is uid 1000 and owns the session.
- **The compositor gets exactly one capability** (`CAP_SYS_NICE`, for RTKit)
  plus device access to the GPU and input nodes. `CapabilityBoundingSet` drops
  everything else.
- **The shell gets no capabilities at all** — `CapabilityBoundingSet=` is empty.
- **Every service declares what it can write.** `ProtectSystem=strict` plus an
  explicit `ReadWritePaths=` means a compromised UI cannot modify the system.

> A path that bit us, and is worth recording: in `ReadWritePaths=`, `%h`
> expands against the **manager's** home (`/root`), not the service user's.
> Using `%h` produced
> `Failed to set up mount namespacing: /root/.local/share/astrix` and a unit
> that died before `ExecStart` was ever reached. The path is now a literal, and
> `build-rootfs.sh` guarantees the directory exists.

### 3.1 The runtime directory

Both the compositor and the shell run with
`XDG_RUNTIME_DIR=/run/user/1000`, and the compositor creates its Wayland socket
there. On an ordinary system `systemd-logind` creates that directory when
someone logs in. **Astrix has no login prompt** — it goes straight to the home
screen — so on a headless or first boot nothing creates it, the compositor
cannot create its socket, and the session dies silently.

`system/astrix-session` therefore creates it explicitly, preferring
`user-runtime-dir@<uid>.service` and falling back to `install -d`, then
*asserting* it exists. Self-contained, and loud when it is not.

---

## 4. Build architecture

```
  host (x86_64)                         target (aarch64)
  ┌──────────────────────┐             ┌────────────────────────────┐
  │ scripts/build-rootfs │─ deboot ───▶│ build/rootfs (ext4 dir)    │
  │                      │   strap     │  /usr/bin/astrix-*         │
  │ scripts/build-gui    │─ qemu-user ▶│  /usr/lib/astrix/…         │
  │   (binfmt_misc)      │   + aarch64 │                            │
  │                      │   gcc       │                            │
  │ scripts/build-image  │─ mtools ───▶│ build/astrix.img (GPT)     │
  └──────────────────────┘   + mkfs    └────────────────────────────┘
```

Two build decisions worth calling out:

**GUI binaries are compiled *inside* the target rootfs**, using the target's own
`gcc` (installed by `config/packages-dev.txt`) driven through `binfmt_misc` +
`qemu-user-static`. The host has glibc 2.35 and the target has 2.41;
cross-compiling on the host produces binaries that fail at runtime. Compiling
in the rootfs removes the host/target ABI gap by construction.

The build is slow because the compiler runs under emulation. It is also the
only approach here that cannot produce a subtly-wrong-linking binary, which is
worth far more than the build time.

**The ESP is written with `mtools`, not mounted.** The build host's kernel has
no FAT module and there is no `modprobe`, so `mount -t vfat` simply fails.
`mtools` needs its own `MTOOLSRC` and, with `-i <drive>`, targets must be
written `z:/path` rather than `::/path`. This cost about an hour to discover.

---

## 5. Layout of the source

| Path | Contents |
| --- | --- |
| `config/os.conf` | Single source of truth: versions, arch, users, display geometry, `SOURCE_DATE_EPOCH`. Sourced by every script; contains no logic. |
| `config/packages.txt` | The 70-package shipping set. Every entry is checked against the real Debian archive by `tests/test-packages.sh`. |
| `config/packages-dev.txt` | Toolchain and debug tooling, installed only by `--full`. |
| `protocol/` | Four vendored wlroots protocol XMLs — mirrors, not forks. |
| `gui/compositor/` | The Wayland display server. |
| `gui/shell/` | Touch UI, gestures, rendering, app lifecycle. |
| `gui/ui/` | Shared 2D canvas and bitmap font. |
| `apps/` | Native applications. |
| `system/` | systemd units plus the session launcher and boot reporter. |
| `rootfs/etc/` | Files overlaid into `/etc` during the rootfs build. |
| `scripts/` | The pipeline. `lib.sh` holds the shared helpers. |
| `qemu/run.sh` | QEMU machine, device model, display, serial, port probing. |
| `tests/` | The host test suite. |
| `docs/` | This documentation. |
