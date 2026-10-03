# Astrix OS — Status

**This is the honest document.** It records what has been *verified*, what is
merely *built*, and what does *not exist*. If a capability is not in the
"verified" or "built" tables, it is not in the OS, regardless of what any
other file implies.

Last updated: 0.1.0 development milestone, M4 reached (boot + real input + real typing on a booted VM).

---

## 1. Verified

"Verified" means: exercised on a real ARM64 boot in QEMU, with the evidence
being the boot output and/or the host test suite. Nothing in this section is
inferred from source code.

| # | Capability | How it was verified |
| --- | --- | --- |
| V1 | The build pipeline produces a bootable ARM64 disk image end to end | `./build.sh` → `build/astrix.img`, then a QEMU boot reaching `Welcome to Astrix OS 0.1.0!` |
| V2 | `systemd` is PID 1, not a shell | Boot log: `Reached target first-boot-complete.target`, init verified as `/lib/systemd/systemd` by `build-image.sh` |
| V3 | The system identifies as Astrix | `/etc/os-release` → `ID=astrix`; asserted by the image verifier |
| V4 | virtio-gpu DRM/KMS is detected and usable | Boot log shows the DRM device; the compositor runs on it |
| V5 | The Astrix compositor runs, binds its Wayland socket, and serves a client | `tests/test-wayland-session.sh`: socket created, `xdg_toplevel` accepted, surface mapped, no protocol error |
| V6 | The Astrix Shell connects to the compositor as a real client | Same test: `app_id` reported as `org.astrix.Shell` |
| V7 | All 8 GUI binaries are genuine ARM64 ELF with the right linkage | `file` + `ldd` in the build: compositor → `libwlroots-0.18.so` + `libwayland-server.so.0`; shell/apps → `libwayland-client.so.0` |
| V8 | Every package in `config/packages.txt` exists for arm64 in Debian trixie | `tests/test-packages.sh`, against the real archive index |
| V9 | The Debian userland is complete and usable | The graphical session is built and run *from* it; `bash`, `apt` and `coreutils` are present |
| V10 | The host test suite passes | `./tests/run-all.sh --fast` → **37 passed, 0 failed** (syntax, UI, gestures, shell input, shell framebuffer ownership, packages, rendering, Wayland session, listener check, compositor focus teardown, pointer retarget, device profiles, display link budget, on-screen keyboard) — `test-gestures` 159 checks, `test-shell-input` covers the dock *and* the recents-card geometry at three panel sizes |
| V11 | The image self-verification gate works | `build-image.sh` refuses to emit an image that fails its checks |
| V12 | A real `seat0` logind session is created for `astrix` and handed to the compositor | Boot log: `loginctl attach` succeeds, drop-in `/run/systemd/system/astrix-compositor.service.d/10-logind-session.conf` written, compositor reports `wlr_session ready (seat seat0, vt 0)` |
| V13 | The compositor drives the virtio-gpu **DRM/KMS** backend on a booted VM | Boot log: `session reported 1 KMS device(s); first is fd 10 (226:0)` → `Initializing DRM backend for /dev/dri/card0 (virtio_gpu)` → `Found 1 DRM CRTCs` → `Found 2 DRM planes` → `DRM backend ready` → `libinput backend attached`. This is a real KMS device, not the headless backend. |
| V14 | Boot reaches the graphical session fast enough to be usable | First boot to `first-boot-complete` fell from ~450 s to ~190 s after moving the session off `multi-user.target` and masking the network wait-online units |
| V15 | **A booted VM reaches the Astrix shell's home screen, with a populated launcher and a stable process** | Boot log: `DRM backend ready` → `libinput backend attached` → `astrix-compositor ready` → `new app registered` → `astrix-shell: panel reports 1024x768 at scale 1; laying out for 1024x768` → `re-laying out at 1024x768` → `astrix-shell: launcher has 7 app(s) from the desktop` → `astrix-shell: running (1024x768)`. The shell runs as a **single PID** for the whole session — no crash-restart loop — and no `WARNING: layout … does not match the panel size`. |
| V16 | The session launcher completes | `astrix-session.service` no longer ends in `Result=timeout`; it reaches `starting astrix-session.target` and the units it wants come up |
| V17 | **Real touch/pointer input is handled by the OS end to end** | Input injected over QMP travels the whole real path (virtio input → evdev → libseat → libinput → wlr → `wl_pointer` → the shell's gesture recogniser → navigation). On a booted VM the serial log shows `astrix-shell: tap at 186,598: home -> app` **and** `astrix-shell: opening app 'Android Apps'` — a tap that lands on a **dock** icon and opens it, at a stable PID, with no crash. A swipe gives `swipe-up-hold at 512,294: app -> app-switcher`. `scripts/send-input.sh` drives it with normalised coordinates. Only the relative-pointer path is verified; see §4. |
| V18 | **The shell lays out for the panel it is actually on, and the dock is tappable there** | The build's phone panel is 720x1600 but the QEMU/virtio-gpu panel is 1024x768. The shell binds `wl_output`, adopts the real mode when the compositor publishes it (`output_handle_done` → `resize_to_output` → `shell_resize`), and re-does its layout and framebuffer. At 1024x768 the dock band is rows 484–711 with icon centres at x = 186, 400, 614, 828; a tap at **(186, 598)** — inside that band, verified against the renderer's geometry — opens the app (V17). `build/screens/12-home-1024x768-qemu.ppm` is that layout rendered for inspection. Before the fix the dock was drawn but laid out below the visible area, and the resize path crashed — see bugs 25 and 27. |
| V19 | **Tapping an app actually starts it** | A dock tap on `Files` on a booted VM produces, in order: `astrix-compositor: pointer entering surface` → `pointer button 272 pressed/released` → `astrix-shell: opening app 'Files'` → `astrix-shell: launched 'Files' as pid 946 (astrix-files)` → `astrix-compositor: new app registered` → `astrix-compositor: focus app: Files`, with no `app closed` afterwards. A real process was forked, connected to `astrix-0` as a Wayland client, and was given focus. Until this, `astrix_shell_open_app()` only set a field and printed a line. |
| V20 | **A user can close a running app, and closing it actually stops the process** | `astrix_shell_close_app()` existed but **no input path could reach it**, so a launched app stayed alive for the whole session and a tap could not even bring it forward. Swiping up on a recents card now closes that app, which mirrors `launch_pending` with a `kill_pending`: the shell decides, the Wayland client signals. The client sends `SIGTERM` to the app's **process group** (so helpers go too), escalates to `SIGKILL` after a 1.5 s grace period, and reaps children each tick so `running` follows real liveness rather than the last tap. `running` is set only when a pid is recorded, so a failed `fork()` can no longer present as a running app. **Verified on a booted VM**, end to end and with a real process:

```
astrix-shell:     tap from 400,598 to 400,598: home -> app
astrix-shell:     launched 'Files' as pid 890 (astrix-files)
astrix-compositor: new app registered (app_id=unset)
astrix-compositor: focus app: Files
astrix-shell:     swipe-up-hold from 512,744 to 512,542: app -> app-switcher
astrix-shell:     switcher swipe-up from 322,460 hit card 1 (1 app(s) running)
astrix-shell:     sent SIGTERM to 'Files' (pid 890)
astrix-compositor: app closed: (untitled)
```

`app closed` is the compositor's xdg_toplevel role being destroyed, which only
happens when the client disconnects — so the process really terminated rather
than merely being marked closed. The compositor stayed at PID 773 and the shell
at PID 865 throughout, which is the bug 33 regression check: a client
disconnecting must not take the session with it. Getting here took bugs 37, 39,
40, 42, 43 and 44. See bugs 29–33 for the original work and 37–44 for what
actually stood in the way. |

| V21 | **The home gesture actually opens the switcher on a real VM** | Verified on a booted VM: `swipe-up-hold from 512,744 to 512,542: app -> app-switcher`. The compositor's system-gesture hand-off was firing (`system gesture claimed after 31px; input returns to the shell`) and the switcher still did not open, so V20's close path was unreachable through the only gesture that reaches it. The cause was **not** the recogniser and **not** pointer drift: the shell logged `swipe-right from 400,598 to 512,576`, where `400,598` is the Files dock icon from a *previous* gesture. Launching an app from a dock tap moves the pointer's input target mid-gesture, so the release went to the new toplevel and the shell was left holding a contact it could never end — anchoring every later gesture to that old press. Fixed by releasing held buttons to the outgoing surface before retargeting (bug 39); three further defects surfaced on the way (bugs 35–38, 40, 42, 44). The diagnostic that actually located it was one extra field in the log: printing the recogniser's *start* point, not just the end point. |
| V22 | **Two target phones have an honest, enforced support status** | `config/devices/{redmi-8a,moto-g64-5g}.conf` specify verified hardware facts, the kernel features each SoC needs, and — separately — the features that **no upstream driver exists for**. `scripts/device.sh check` verifies the kernel config really covers the upstream set (it failed on both phones before the config fragments were added). `scripts/flash-device.sh` refuses to flash anything not verified booting, and `tests/test-devices.sh` runs it with no device attached and fails the build if it does not refuse. Hardware facts were checked against vendor and reference sources rather than written from memory. **Neither phone boots**, and the repo is built so that cannot quietly become a claim later. See `docs/DEVICES.md`. |

| V23 | **The display port has an arithmetic gate before it has a driver** | `scripts/display-budget.sh` computes what each phone's panel demands of its MIPI DSI link from the vendor panel spec: the moto g64 5G's 1080x2400 **120Hz** needs 2.258 Gbit/s per lane against a 2.5 Gbit/s lane (**10% headroom**), the Redmi 8A's 720x1520@60 needs 477 Mbit/s (**81%**). The tool *refuses* an infeasible panel (exit 1) rather than rounding it into a yes, `device.sh check` fails on a profile whose panel cannot be driven, and `tests/test-display-budget.sh` pins both directions. The finding that changes the port plan: bring the moto panel up at **60Hz first** (55% headroom), because 10% is where DSI link training starts failing in ways that look like driver bugs. Also: `scripts/build-device.sh` now exists and is gated by the same rule as the flasher, so no flash bundle can be assembled for a device nobody has booted, and `tests/test-devices.sh` proves it refuses and leaves nothing on disk. The ordered bring-up, including how to verify each of the seven DRM stages over serial/`dmesg` instead of by looking at the phone, is in `docs/PORTING.md`. **Still no phone boots.** This is groundwork, not progress-claimed-as-a-boot. Re-verified after the change: `./build.sh` → `BUILD COMPLETE in 513s`, 35/35 host tests, and a fresh VM boot reaches `astrix-compositor ready` → `panel reports 1024x768` → `launched 'Files' as pid 942`. |

| V24 | **The OS can type: a system on-screen keyboard that delivers real keys** | The shell owns a keyboard (status-bar button, or the `hide` key) with QWERTY, a symbol layer, one-shot shift, caps lock, space/return/backspace. A tap becomes a queued codepoint; `main.c` turns it into an xkb keycode by walking the keymap's level 0 and level 1 and pushes it through **`zwp_virtual_keyboard_v1`** — the only Wayland-legal way for a shell to type into another client, because the seat belongs to the compositor. `tests/test-keyboard` (37 checks) pins the tap→character mapping, the one-shot shift, the symbol layer and the rule that a tap on the keyboard never also reaches the home screen behind it; it immediately caught a real SIGFPE (the symbol layer leaves the `zxcvbnm` row empty and the layout divided by its length). Three keyboard screens are rendered to `build/screens/` for inspection. Delivery is asserted end to end — three taps, three queued keys, three `wl_keyboard.key` events back in the focused client — on every host test run (see V26). Key injection is verified on a booted VM with a keyboard attached (see V25); it is not yet verified against a physical touchscreen. |

| V25 | **The on-screen keyboard injects real keys into a booted VM** | On a real ARM64 boot (1024x768, virtio-keyboard attached), a QMP tap on the status-bar button opens the keyboard (`keyboard opened`) and a tap on a key produces `on-screen key 'a'` / `'b'` — the tap→codepoint half, driven through the guest's own evdev stack, not by calling the shell's function. |

| V26 | **Injected keys now reach a client** (bug 48) | The delivery half used to stop at the compositor, silently. wlroots 0.18 does not register a client-created virtual keyboard with any backend, so it never arrives through `backend->events.new_input`; the compositor now watches `manager->events.new_virtual_keyboard`, attaches the keyboard and gives it to the seat when nothing else holds it. Verified twice: (1) on a booted VM, `on-screen key 'a'` → `key 38 pressed` / `key 38 released` and `'b'` → 56, i.e. xkb keycodes reaching the focused client's `wl_keyboard`; (2) on every host test run, where three taps become three `key` events. The delivery assertion in `tests/test-wayland-session.sh` was previously **skipped whenever no hardware keyboard was attached** — the one environment where it mattered most was the one place it was not required; it is now asserted unconditionally, including `wl_keyboard.enter`. |

### The four bugs the keyboard work exposed

Writing the delivery half found four real defects, none of them in the layout
logic the unit tests cover — which is the argument for having written it:

| # | Defect | Symptom | Cause |
|---|---|---|---|
| 45 | The compositor never advertised a virtual keyboard manager | The keyboard drew, queued keys and typed nothing | wlroots only creates that global from its **Wayland backend**, and this compositor deliberately binds its own `wl_display` and builds its own seat. One line — `wlr_virtual_keyboard_manager_v1_create(server->wl_display)` — and `tests/test-wayland-session.sh` now asserts the global exists |
| 46 | The vendored protocol XML put `new_id` before `seat` | `error marshalling arguments for create_virtual_keyboard: null value passed for arg 1`, then the shell died | The real protocol is **seat first, new_id second**; wayland-scanner older than 1.21 silently moves `new_id` to the front, and nothing says so at build time. The in-rootfs build uses 1.23 and is correct; `protocol/README.md` records the requirement |
| 47 | The keymap fd was closed before the compositor read it | Shell exited ~2s after start, with no error at all | The compositor reads the fd when the request is *dispatched*. It is now held until after the roundtrip. The shell also grew a `SIGSEGV` backtrace handler, because the first symptom was one line reading `qemu: uncaught target signal 11` and that turned out to be a missing diagnostic, not the bug |
| 48 | The compositor never subscribed to client-created virtual keyboards | Every key the on-screen keyboard sent went nowhere — silently | wlroots 0.18 gives a `zwp_virtual_keyboard_v1` a `wlr_keyboard` but **never registers it with any backend**, so it never reaches a compositor through `backend->events.new_input`. The only notification path is `manager->events.new_virtual_keyboard`, which nobody was listening to, so keys were emitted on a signal with no listener attached. The compositor now watches that signal, attaches the keyboard, and hands it to the seat when no hardware keyboard holds it — which is also the case that matters on a real phone |

Bug 46 is the one worth remembering: a protocol definition compiled by a
different version of the same tool produced a client that built, linked, ran,
and died, with an error message that points nowhere near the cause.

### The keyboard now delivers to a client (bug 48, fixed)

The delivery half used to stop at the compositor: keys were injected, nothing
crashed, and no client ever received one. The cause was not the shell and not
the seat's focus — it was that wlroots never told anybody the virtual keyboard
had arrived. See bug 48 above.

The whole chain is now asserted on every host test run, headless backend
included, in `tests/test-wayland-session.sh`:

```
astrix-shell: self-test: queued 3 key(s) from the on-screen keyboard
astrix-shell: keyboard focus entered
astrix-shell: on-screen key 'a'
astrix-shell: key 38 pressed      <- xkb keycode 38 = 'a'
astrix-shell: key 38 released
...
astrix-shell: key 54 pressed      <- 'c'
```

and in the compositor's log, on the same run:

```
virtual keyboard manager ready
virtual keyboard created by a client; attaching it
seat capabilities: keyboard
keyboard focus -> the Astrix Shell
```

The check used to be *skipped* whenever no hardware keyboard was attached,
which is precisely the case that hid the bug for so long: the environment with
no keyboard is the environment where a keyboard matters most, and it was the
one place delivery was not required. It is now asserted unconditionally, and
the same assertion covers `wl_keyboard.enter`, because "the client got a
keymap" and "the client got a keystroke" are different claims and only the
second one is the feature.

Still **not** verified: typing on a physical touchscreen (no absolute-touch
device exists on this QEMU host, see §6).

---

## 2. Built (compiles, installs, but not yet demonstrated on a full boot)


| Capability | Note |
| --- | --- |
| `astrix-compositor` | Verified end to end on a booted VM (V13, V15): real DRM/KMS, libinput, binds its Wayland socket, serves a client. |
| `astrix-shell` | Verified to connect and map its surface on a booted VM (V15). Rendering is software (pixman) under QEMU; no GPU-accelerated path has been exercised. |
| `astrix-terminal` | Builds and installs as ARM64 ELF, and is launched by the same verified path as `astrix-files` (V19). **Typing into it with the on-screen keyboard is not yet exercised on a booted VM.** Since V26 the keyboard is no longer the limitation — it delivers to whichever client holds keyboard focus — but the terminal's own input path (its `wl_keyboard` handler, cursor, line editing) has not been driven by it, so that stays unverified. |
| `astrix-files` | **Launched and mapped on a booted VM** (V19): a dock tap forks the process, it connects to `astrix-0` and the compositor reports `new app registered` → `focus app: Files`. Also **terminated on request** (V20). |
| `astrix-settings` | Builds and installs. Not yet exercised on a booted VM. |
| `astrix-sysinfo` | Builds and installs. Not yet exercised on a booted VM. |
| `astrix-package-manager` | Builds and installs. **Cannot install anything** — the privileged helper does not exist. |
| `astrix-apk-manager` | Builds and installs. **Is a UI only — it executes no APKs.** |
| Gesture recognition | Unit-tested on the host (V10) **and** exercised through real injected input on a booted VM (V17): a tap opens a dock app, a swipe-up-hold opens the app switcher, and a swipe up on a recents card closes that app (V20). Only the relative-pointer path is verified — see §4. |
| Screen rendering | Every shell screen renders to PPM on the host and is visually inspectable in `build/screens/`. |
| NetworkManager, PipeWire, seatd, udev, apt, polkit | Installed. Started by systemd; the boot reaches `multi-user.target` with them active. |

---

## 3. Not implemented

Listed so that nothing is mistaken for working.

| Capability | State |
| --- | --- |
| **Android APK execution (Waydroid)** | Not started beyond design. See `docs/ANDROID.md`. |
| **Physical-device flashing** | Not implemented. No device tooling, no `fastboot`, no partition writing for real hardware. QEMU images only. |
| **Camera** | No camera service, no HAL, no pass-through. |
| **Telephony / SMS / calls** | Nothing. |
| **Location / GPS** | Nothing. |
| **Sensors** (accelerometer, gyroscope, proximity, light) | Nothing. |
| **Bluetooth** | `bluez` is installed. No pairing UI, no profile integration, no verification. |
| **Wi-Fi / cellular radio bring-up on real hardware** | No hardware. NetworkManager is present and the stack is standard, but **no radio has been driven**. |
| **Full-disk or per-user encryption** | Nothing. |
| **Secure lock screen** | Nothing. Autologin is a documented development default. |
| **Per-app sandboxing** | Every app runs as `astrix` with that user's full access. |
| **A/B verified updates / OTA** | Designed in `docs/DESIGN.md` §7.2. Not built. |
| **Secure boot / verified image signing on device** | Nothing. |
| **Privileged package helper (`astrix-pkg-helper`)** | Not written. Referenced by design only. |
| **Typing in a real app with the system on-screen keyboard** | The keyboard is a system input method now (V26) and its delivery to the focused client is verified, but no *app* has been driven by it yet: `astrix-terminal` has never been typed into on a booted VM. |
| **Text input in an IME sense** — composing, dead keys, non-Latin layouts, candidate windows | Nothing. The keyboard emits codepoints; there is no input-method protocol (`zwp_input_method_v1`) and no client binding one. |
| **Notifications from third-party services** | Nothing. |
| **Multi-user / guest / per-profile switching** | Single-user by design. |
| **Accessibility services** (screen reader, talkback) | Nothing. |
| **Localisation beyond `C.UTF-8`** | Nothing. |

---

## 4. Explicit non-claims

These are things that could be *mistaken* for support, and are not:

- **"Astrix is a real OS for your phone."** It is a real OS that boots in
  QEMU. It has never been flashed to, or booted on, physical ARM64 hardware.
- **"Android apps work."** They do not run at all.
- **"The camera works."** There is no camera code.
- **"It's secure."** It has a reasonable privilege model for the graphical
  session and no encryption, no lock screen, and no app sandboxing. Read
  `docs/SECURITY.md` §11.
- **"Multi-touch works."** Only the relative-pointer path is verified. On this
  QEMU host `virtio-tablet` never gets an evdev node and there is no
  `usb-tablet`, so absolute touch and true multi-touch have not been driven at
  all; pinch and two-finger gestures are unit-tested only. `scripts/send-input.sh`
  says so when it falls back to relative motion.
- **"The app switcher can close apps."** It can, over the relative-pointer
  path, at the panel sizes the host test covers. Absolute touch is still
  unverified (above), and no app that ignores `SIGTERM` has yet been observed
  going through the `SIGKILL` escalation.
- **"It's production ready."** It is a 0.1.0 development milestone.

---

## 5. Milestone history

Each milestone had to be *testable* before it counted.

| Milestone | Reached when |
| --- | --- |
| M0 — repository, build system, config | `./build.sh` runs end to end on a clean tree |
| M1 — bootable Debian ARM64 rootfs | QEMU reaches the login prompt with `systemd` as PID 1 |
| M2 — minimal GUI stack builds | All 8 ARM64 binaries exist with correct linkage |
| M3 — Wayland session works | The shell maps a surface on the real compositor (host test) |
| M4 — boots to a graphical session | **Reached and verified** (V15–V23). DRM/KMS + libinput + libseat come up, the compositor binds `astrix-0`, the shell adopts the real 1024x768 panel and shows a populated 7-app launcher, and **real injected input drives the whole app lifecycle**: a dock tap starts a real process that maps a window, a swipe-up-hold opens the app switcher, and a swipe up on a recents card sends that process `SIGTERM` and the compositor sees the client disconnect — `sent SIGTERM to 'Files' (pid 890)` → `app closed`, with the compositor and shell each still on a single PID throughout. The full `./build.sh` pipeline was re-run from a clean GUI stage as `BUILD COMPLETE in 513s` with 35/35 host tests. Still unverified: true multi-touch / absolute touch input (no evdev tablet node exists on this QEMU host — see §6), a picture of the UI on the virtio-gpu primary plane (`screendump` captures the console scanout, not the DRM plane — bug 16), and **any physical device at all** (`docs/DEVICES.md`). |
| M5 — Android compatibility | Not started |
| M6 — physical device | Not started |

### Bugs found and fixed by actually booting

Recorded because each was invisible without a real boot, and because the
failure mode — a black screen with no message — is the defining hazard of this
kind of project.

1. Compositor never sent the initial `wlr_xdg_surface` configure → the shell
   deadlocked and never mapped. Fixed in `toplevel_commit_handler`.
2. The compositor started twice (system unit and `systemd-run --user` with the
   same name) → the two scopes fought. Now system scope only.
3. `WAYLAND_DISPLAY` disagreed with the socket the compositor actually bound.
   Now a fixed `astrix-0` on both sides.
4. `seat` group missing → `status=216/GROUP`, compositor dead, no log line.
5. `ReadWritePaths=%h/...` expanded to `/root/...` → `status=226/NAMESPACE`,
   shell dead before `ExecStart`.
6. `/run/user/1000` never created (no login on a headless boot) → the compositor
   had nowhere to put its socket. This was the blocker at the time of writing.
7. `StartLimitIntervalSec`/`DefaultDependencies`/`After` placed in `[Service]`
   and `[Install]` where systemd silently drops them.
8. `journal`-only output meant a display failure had no visible symptom.
   Changed to `journal+console` and added `astrix-boot-report.service`.
9. **Kernel and initramfs were resolved independently** — two separate
   `find … | head -1` calls paired `vmlinuz-6.12.107` with
   `initrd.img-6.12.111`, so `virtio_blk` never loaded and the kernel panicked
   with `ALERT! /dev/vda2 does not exist`. They are now resolved as a **pair**:
   a `vmlinuz-X` is only accepted when `initrd.img-X` exists.
10. **The session was ordered behind `multi-user.target`**, which is itself
    ordered after `network.target`. NetworkManager's wait-online cost over
    *13 minutes* of black screen. The session now starts from
    `basic.target`, and the wait-online units are masked in the image.
11. `wlr_backend_autocreate()` only reaches DRM **through a libseat session**.
    With `XDG_SESSION_ID` unset it silently fell through to the headless
    Wayland backend, which is why the compositor "ran" but never touched the
    GPU. The compositor now creates the session itself
    (`wlr_session_create` + `wlr_session_find_gpus` + `wlr_drm_backend_create`).
    Note: `wlr_backend_is_drm()` returns **false** for a multi-backend, so the
    "do we have a real display" decision uses an explicit `real_display` flag
    set when the DRM backend is created.
12. `loginctl` blocks on the system bus; while the compositor was crash-looping
    the session launcher hung and died on a timeout **with zero output**. Every
    logind call now goes through `loginctl_bounded()` (5 s default,
    `ASTRIX_LOGINCTL_TIMEOUT`).
13. **`ProtectSystem=strict` remounts `/` read-only — including `/run`.** The
    compositor had already opened its DRM backend successfully and then died
    creating its own Wayland socket:
    `unable to open lockfile /run/user/1000/astrix-0.lock check permissions` →
    `failed to create wayland socket 'astrix-0'`. Fixed with
    `ReadWritePaths=/run/user/1000`, and `build-rootfs.sh` now *fails the build*
    if a unit keeps `ProtectSystem=strict` without it.
14. **`ProtectHome=yes` hides `/run/user` behind an empty read-only mount.**
    This was the *actual* cause of bug 13 and it survived the first fix,
    because `ReadWritePaths` cannot rescue a directory that is not there. The
    message is identical, the DRM backend keeps coming up first, and every
    other signal says the GPU is fine. Fixed with `ProtectHome=read-only`;
    `build-rootfs.sh` now fails the build on `ProtectHome=yes` in the
    compositor unit. Generalisable lesson: systemd hardening options interact,
    and the symptom points at the wrong layer.
15. **Touch input through libinput is still unexercised.** The libinput backend
    attaches and enumerates, but no pointer/touch event has been driven through
    it on a booted VM.
16. **`screendump` over QMP does not photograph the UI.** On virtio-gpu it
    captures the device's console scanout (1024x768 text console), not the DRM
    primary plane the compositor scans out to. `scripts/screenshot.sh` exists
    and is honest about this; the UI is verified through
    `tests/render-screens.c` → `build/screens/*.ppm` instead.
17. **`wlr_backend_start()` was never called.** Output and input devices are
    only enumerated *during* backend start (`new_output` / `new_input` fire
    then). Without the call the session came up, the socket was bound and the
    client connected — to **zero outputs and zero input devices**, with
    nothing in the logs to say so. It is now called once every listener is
    registered, and a failure is fatal rather than silent.
18. **Requesting the built-in 720x1600 mode is rejected by virtio-gpu**, and
    the output stayed disabled: a black screen with no further messages. The
    compositor now logs the rejection
    (`Virtual-1 rejected the requested 720x1600; falling back to the
    preferred mode`) and retries with the mode the connector prefers.
19. **The output was never added to the output layout.** In wlroots 0.18
    nothing else does it, and the layout is what the cursor is constrained
    to — with no output in it every pointer motion is clamped to a corner and
    every tap lands in the same wrong place. Fixed with
    `wlr_output_layout_add_auto()`. (Layout position lives on
    `struct wlr_output_layout_output`; `wlr_output->lx/ly` no longer exist.)
20. **`wlr_seat_pointer_notify_enter()` was never called.** Without a focused
    surface, `notify_motion` and `notify_button` are documented no-ops, so
    input silently went nowhere even though the logs showed the seat
    advertising `pointer keyboard`.
21. **Pointer and touch coordinates were forwarded in layout space** instead of
    surface-local space, so every event was offset by the surface's position
    in the layout. Added `layout_to_surface()`.
22. **`wl_pointer.frame` had no listener, and libwayland ABORTS a client that
    receives an event it has no handler for** — so the shell died with SIGABRT
    on the first pointer event of every session and systemd restarted it in a
    loop. (`set_cursor` is a *request*, not an event; opcode 5 is `frame`.)
    A second, subtler variant of the same bug: a later duplicate `.frame =
    NULL` in the same initialiser silently overwrote the real handler, and
    the crash survived one fix.
23. **The launcher's app list was always empty.** Nothing scanned
    `/usr/share/applications` and no desktop entries existed, so the home
    screen showed an empty grid. Added `gui/shell/src/apps.c` and 7 desktop
    entries; `build-gui.sh` now installs them and *fails the build* if a built
    app has no desktop entry whose `Exec=` matches its binary.
24. **Dock icons were drawn at the bottom but hit-tested with page-grid
    coordinates** (near the top), so real taps missed. Added
    `hit_test_dock_icon()`, and `ASTRIX_DOCK_COLS` now lives in one header
    shared by the renderer, the input code and the app scanner.
25. **The shell assumed the panel size instead of asking for it.** It laid
    out for the build's 720x1600 regardless of the display it was actually
    given, so on the QEMU panel the dock and navigation bar were laid out
    *below* the visible area — drawn, but impossible to touch. The shell now
    binds `wl_output` and adopts the real mode (logical size = mode ÷
    output scale), measured with `WAYLAND_DEBUG`.
    Binding alone is not enough, and this is the subtle part: the
    compositor sends geometry/mode/scale/done as a separate batch that
    arrived **more than two seconds** after the bind — long after
    `wl_display_roundtrip()` returned and the shell had begun drawing. A
    one-shot read during startup therefore *always* sees nothing (reproduced
    on the host in 100 s, twice: once with no wait at all, once with a 2 s
    wait that still timed out 56 ms short). The size is now adopted whenever
    it arrives — `output_handle_done` → `resize_to_output` → `shell_resize`,
    which also re-establishes the alias between the shell's framebuffer and
    its shm buffer, something the pre-existing resize path never did.
26. **Launcher logging used `printf`**, i.e. block-buffered stdout under
    systemd — so the evidence existed but never appeared. Launcher, gesture
    and app-open messages all go to stderr now.
27. **The shell freed a framebuffer it did not own — but only on a panel
    whose size differed from the build's default.** The Wayland host aliases
    `astrix_shell->pixels` onto its `wl_shm` mapping, so the shell draws
    straight into the buffer the compositor uploads. `shell_resize()` then
    `munmap`ped that same mapping and `astrix_shell_set_size()` saw a size
    change and called `free()` on it — `free()` on an `mmap`'d, already
    unmapped pointer. Every boot ended in `sig=11` (SIGSEGV) seconds after
    `re-laying out at 1024x768`, and systemd restarted the shell in a loop.
    The host smoke test never caught it because it ran at the default
    720x1600, where `shell_resize()` early-returns on the matching size; on
    the real QEMU panel (1024x768) the resize path is actually taken. It was
    also timing-dependent: if `wl_output`'s state arrived before the shm
    buffers existed, the other branch ran and the shell survived, which is
    why an earlier boot appeared healthy. Fixed by tracking ownership —
    `struct astrix_shell.pixels_owned` — and freeing only when the shell
    owns the buffer.    `tests/test-shell-size.c` now aliases an `mmap` region
    the way the host does and fails if a resize or destroy touches it.
28. **The compositor dereferenced a NULL device list when the GPU's node did
    not exist yet.** `wlr_session_find_gpus()` has *three* outcomes — a
    positive count, `0` for "this session has no KMS device", and `-1` for
    failure — but the code only tested `n < 0`, so the `0` case fell into the
    success branch and read `gpus[0]` off a NULL list. SIGSEGV, with only
    libseat's `Waiting for a KMS device` line above it as a clue. Whether it
    happened at all was pure timing: the session launcher started the
    compositor at 399s on one boot and virtio_gpu finished initialising at
    428s, so the compositor crashed; on a slightly slower boot the same code
    ran at 490s, after the device was ready, and worked. The retry made it
    worse — the device was then present but not yet fully initialised, so it
    died in the allocator (`Cannot use DRM dumb buffers with non-primary DRM
    FD`) and the whole session timed out. Two fixes: the compositor now
    treats `n <= 0` as "no device" and falls back with a diagnostic, and the
    session launcher waits (bounded, `ASTRIX_GPU_WAIT`, default 20s) for
    `/dev/dri/card0` before starting the graphical session, reporting a
    timeout rather than refusing to start. A phone always has a GPU; a
    headless developer VM may not.
29. **A running app could not be closed, and nothing tracked it.** Three
    separate defects hid behind one symptom (the app switcher never emptied):
    (a) `astrix_shell_close_app()` was implemented but *nothing in the input
    path called it* — there was no gesture, key or button that reached it;
    (b) the shell recorded no pid, so even if it had been called there was
    no process to signal; and (c) `SIGCHLD` was set to `SIG_IGN`, which has
    the kernel auto-reap children — fine for zombies, but it made the shell
    permanently blind to an app quitting, so `running` could only ever be
    set, never cleared. Fixed: a `pid` field on the app record, a
    `kill_pending` request mirroring `launch_pending`, a recents-card swipe-up
    to close, `SIGTERM` to the process group with `SIGKILL` escalation, and
    per-tick `waitpid()` so exits update the model. `running` is now set by
    `astrix_shell_set_app_pid()` and nowhere else — a tap requests, a pid
    proves.
30. **Rediscovering an app orphaned its process.** `astrix_shell_add_app()`
    treats a duplicate id as a reinstall and updates the entry in place, but
    it overwrote the whole record except `running` — so the `pid` was lost. A
    running app that was rescanned (a changed `.desktop` file) became
    permanently running with nothing left to signal: unkillable, and stuck in
    the app switcher for the rest of the session. Both `running` and `pid`
    are now preserved across a rediscovery.
31. **The app switcher's second and third cards were unreachable.** The
    renderer used `card_w = width/2 + width/12` while offsetting each card by
    half its width, so a group of three spanned ~2.2 panel widths: card 1
    started inside card 0 and card 2 started past the right edge. The hit
    test only accepted the frontmost card, so no tap could ever reach the
    other two, and neither could a swipe-to-close. Card width is now
    `width/3 + 40` with a half-width step, so three cards span `2*card_w` and
    all fit. The hit test also walks cards **back to front**, because the
    cards overlap and the one drawn last is the one the user sees. This was
    found by the new host test, not by looking at the screen.
32. **A swipe-to-close could never close anything.** Even with cards reachable,
    the close was hit-tested at the position where the finger *ended*, and a
    swipe up always ends above the card it started on. The card is now
    resolved from the gesture's start position (`sh->gestures.start_x/y`),
    which is the same reason the gesture recogniser latches swipe-up-hold.
33. **The compositor crashed whenever a client disconnected — and closing an
    app *is* a client disconnect.** This is the most serious bug in the
    project, and it is why "the shell can terminate an app" could not be
    claimed before it was fixed. `astrix_toplevel` had two independent
    destroy handlers — `toplevel_surface_destroy_handler` and
    `toplevel_role_destroy_handler` — and **both called `free(t)`**. A client
    going away fires *both* (wlroots destroys the role object and the
    surface), so the second one operated on freed memory. Worse, neither
    unregistered the struct's *other* listeners, and those listeners live
    inside the struct, so the signal lists were left holding pointers into
    freed heap. The crash landed on the very next emit: the backtrace from a
    real boot is
    `wl_client_destroy -> wlr_surface_unmap -> wl_signal_emit_mutable ->
    astrix-compositor+0x623c`, i.e. a SIGSEGV inside the compositor's own
    handler. It took down the whole graphical session the moment an app was
    dismissed, and the shell logged `app pid 898 killed by signal 6` only as
    a bystander. The double `free()` was fixed with a single idempotent
    `toplevel_finish()`: a `torn_down` flag makes the second call a no-op,
    every listener is unregistered there and nowhere else, the scene node and
    focus state are cleared in one place, and `astrix_shell_finish()`
    (shutdown, which destroys clients *afterwards*) goes through the same
    path. The unmap and commit handlers also bail on `torn_down`, because
    wlroots unmaps a surface before destroying it.

    **That fixed only half of it, and this entry was wrong until a boot proved
    it.** The next run crashed at `astrix-compositor+0x6254` — a *different*
    address, so `addr2line -e` on the guest binary was the thing that made it
    diagnosable at all. It resolved to `set_focused_toplevel`,
    `gui/compositor/src/shell.c:147`, whose first line was

        struct astrix_server *server = toplevel->server;

    while three call sites passed `NULL` — every path that clears focus when
    an app gives up the foreground, which is exactly what happens during
    `wl_client_destroy`. The function branched on `if (toplevel)` a few lines
    later, so NULL was an intended input that the first line dereferenced
    anyway. Not a use-after-free at all: a plain NULL dereference, and the
    crash was never going to move until that was read rather than guessed
    at. Fixed by passing the server in as a parameter, which makes the NULL
    case expressible instead of impossible.

34. **Every app aborted on its first touch — and the test that exists to
    prevent exactly this was not looking at the apps.** libwayland kills a
    client that is sent an event it has no listener for, so a short listener
    initialiser is a SIGABRT, not a no-op. The shell's `wl_pointer` listener
    had been fixed for this (see the header of
    `tests/test-wayland-listeners.sh`), but `apps/common/src/astrix_app.c` —
    the host library **every** app is built on — implemented
    enter/leave/motion/button/axis and stopped. The rest zero-filled, so
    `.frame` was NULL, and

        astrix-shell[945]: listener function for opcode 5 of wl_pointer is NULL

    killed `astrix-files` the moment it was touched. Two findings here, and
    the second is the more interesting one:

    - The listener genuinely was incomplete. `wl_pointer` needs `frame`,
      `axis_source`, `axis_stop`, `axis_discrete`, `axis_value120` and
      `axis_relative_direction` as well; `wl_touch` also needs `shape` and
      `orientation`. All added. `shape`/`orientation` describe a stylus and
      are never read here — but on a panel that reports a pen, libwayland
      would have aborted the app over a value it was never going to look at.

    - `tests/test-wayland-listeners.sh` listed the seven app `main.c` files
      and the shell — and **not** `apps/common/src/astrix_app.c`. The
      library is where the listeners live; the per-app files are mostly
      `main()` and drawing code. So the test passed, loudly, on a tree in
      which every single app crashed on first touch. A test that does not
      cover the code that broke is worse than no test, because it is
      evidence. That file is now scanned.

    The checker also had a bug of its own: it anchored the end of an
    initialiser to `\n\s*};`, which does not match a one-line initialiser
    ending `};` on the same line as its last member. The match then ran past
    the end of the initialiser and swallowed the next struct's members,
    reporting two phantom "sets the same member twice" errors on
    `astrix_app.c` — and, worse, would have hidden a genuinely missing
    handler in any one-line initialiser. Now it ends at the first `};`.

35. **The home gesture was handed to the shell with no anchor position.**
    `wl_pointer.button` carries **no coordinates**: a client reads the
    position of a press from the last motion event it was given. The
    compositor's claim path released focus and sent a press without
    sending a position first, so the press could be interpreted at
    whatever position the shell last saw. `maybe_claim_system_gesture()`
    now sends the press origin to the shell immediately before the
    synthesised press. (This turned out to be necessary but nowhere near
    sufficient — see bug 39, which was the actual cause.)

36. **The recogniser required strictly `ady > adx`, which a relative
    pointer cannot satisfy.** QEMU's virtio-mouse is relative-only
    (`query-mice` reports `absolute: false`; QMP rejects `abs` outright),
    and a relative device accumulates acceleration and clamping error, so a
    straight vertical swipe arrives with real x-drift.
    `classify_release()` therefore took the horizontal branch whenever
    `ady <= adx`, even with a 300 px vertical component. Replaced with
    `vertical_dominant()`, which requires the vertical component to exceed
    the horizontal one by `vertical_dominance_pct` (default 120%) — a
    margin that absorbs drift without turning a genuine diagonal into a
    vertical swipe. The leftover horizontal cases now fall back to the sign
    of `dx` rather than re-testing dominance, so no swipe is classified as
    nothing. Covered by `test_vertical_swipe_survives_pointer_drift()`.

37. **A latched gesture was reported twice, and the second report replaced
    the swipe.** Latching exists so a long press shows its menu *while the
    finger is still down*. The motion handler that sets the latch also
    **returns** it, and the release handler then returned the latch a
    second time. The second report is the one that does the damage: on a
    swipe-up it overwrote the correctly classified `SWIPE_UP` with the
    stale hold. On a booted VM a real upward home swipe logged
    `long-press at 512,704` and `long-press at 512,575` — the same
    gesture, twice, and the app switcher never opened. The release now
    clears the latch and reports nothing.

38. **A slow swipe lost to a long press, because the hold fires on a
    timer rather than on movement.** The recogniser fires long press when
    the finger has been down for `long_press_ms` *and* is still within
    `long_press_slop_px` of the start. Under emulation that second
    condition is not protection: one injected waypoint takes tens of
    milliseconds of guest time, so a swipe that should cross the slop in
    50 ms can still be sitting inside it when the hold timer expires. The
    hold then latches and wins. A long press is now **revoked** if the
    finger subsequently travels past the slop, which is what a human does
    the moment a context menu appears under their thumb. Covered by
    `test_long_press_is_revoked_by_later_travel()`, written from the VM log.

    The honest limit of this: on real hardware at 60–120 Hz the slop test
    is enough on its own, and this is belt-and-braces. It was needed here
    because QEMU's input round trip is slower than the gesture.

39. **The compositor stranded the shell mid-gesture, so every later gesture
    was measured from a stale anchor — this is the actual cause of the home
    swipe failing, and my first three fixes were all treating symptoms.**
    The diagnostic that settled it was one log line: the shell now prints
    the recogniser's start point as well as the end point, and it read

        astrix-shell: swipe-right from 400,598 to 512,576 on app

    `512,576` is where the finger lifted. `400,598` is the **Files dock
    icon** — where the finger was when it launched an app several gestures
    earlier. The recogniser was not misclassifying a swipe; it had never
    received a new press at all.

    Why not: the pointer's input target follows app focus, so it moves in
    the middle of every gesture that launches something. Tapping a dock
    icon gives the shell the press; the new toplevel is then focused; the
    release is delivered to the **toplevel**. The shell is left holding a
    contact it can never end, and its recogniser stays anchored to that
    press for the rest of the session. Every subsequent gesture is
    classified from `400,598`, which is why a straight upward home swipe
    with `dy = −173` and `dx = 0` came out as a swipe-*right*: it was
    measured from an origin 320 px below and 112 px to the left.

    This is a protocol invariant, not a policy choice: **a client that is
    given a press must always be given its release.** `release_held_buttons()`
    now sends the release to the outgoing surface before the switch.
    Order matters — wlroots delivers a release to whichever surface has
    focus, so it has to be release, then leave, then enter, which is why
    the call sits *before* `notify_clear_focus()` in
    `maybe_claim_system_gesture()` and before `notify_enter()` in
    `forward_pointer_motion()`. The button list is copied out of
    `seat->pointer_state.buttons` before being drained, because the notify
    mutates the very array being iterated.

    Guarded by `tests/test-pointer-retarget.sh`, which — like the focus
    teardown test — is run against the pre-fix source to prove it still
    detects the bug.

    40. **wlroots drops a motion that repeats the position an `enter` just
    established — so the client never learns where the press happened.**
    With bug 39 fixed the shell was still anchoring at `400,598`, and the
    motion trace (added for exactly this) showed why:

        astrix-compositor: pointer entering surface at 512,744
        astrix-compositor: synthesising the shell's press at 512,744
        astrix-shell:      pointer button 272 pressed at 400,598
        astrix-shell:      pointer motion to 512,713 (from 400,598)

    The compositor sent the position; the shell never received it. The
    claim path entered the shell at the press origin and then sent a motion
    to that same origin, and **wlroots suppresses a motion that does not
    change the position** — so the motion was discarded and the only thing
    the client had was the enter, which `pointer_handle_enter` ignored
    because it was empty.

    Two fixes, because either alone leaves a hole:

    - The compositor now enters where the cursor *is* and then moves to the
      press origin, so the position it needs to convey is by definition a
      change. Entering at the origin and moving to it can never work, and
      the ordering is not obvious enough to leave to memory.
    - The shell now takes the position from `wl_pointer.enter`, which
      carries it by protocol. That is the real defect: a client that tracks
      position only from motion is relying on something the compositor is
      entitled not to send, and it fails silently.

    This is the third bug in this sequence with the same shape — a position
    that had to travel through a step nobody was logging.41. **The gesture log could not distinguish a wrong classification from a
    wrong anchor.** `astrix-shell: swipe-right at 512,570` says nothing
    about where the press was, and that was the one thing needed to find
    bugs 39 and 40. The recogniser's start point is now printed with the
    end point (`swipe-right from 400,598 to 512,570`), every button event
    is logged with the position it was interpreted at, and motions are
    traced when they follow a button or jump more than 64 px. The last one
    is what proved bug 40: it showed the origin motion arriving *after* the
    press, in the right order but at the wrong time.

42. **"Swipe up and hold" did not require holding, so on a loaded machine
    every swipe was one.** The hold was timed from the start of the touch,
    which means any swipe slow enough to outlive `long_press_ms` satisfies
    it — and under emulation a swipe takes seconds. A swipe meant to
    dismiss a recents card came out as `swipe-up-hold`, which the switcher
    has no handler for: the screen stayed open and nothing closed, with no
    error anywhere. The clock now runs from `moved_ms`, the last moment the
    finger actually moved, so "hold" means what it says. Covered by
    `test_slow_swipe_is_not_a_hold()`.

43. **A closed app was never actually signalled — the launcher lied.** This
    is the one that was really blocking V20, and it had nothing to do with
    gestures. `astrix_shell_close_app()` recorded `kill_pending = index`
    and then, on the next line, cleared `apps[index].pid` — correctly, the
    app is no longer running from the UI's point of view. But the process
    that sends the signal lives in another translation unit and read the
    pid back out of `apps[kill_pending]`, by which point it was always
    **zero**. So the request was dropped without a word:

        astrix-shell: switcher swipe-up from 322,460 hit card 1 (1 app(s) running)
        astrix-shell: swipe-up from 322,460 to 322,373: app-switcher -> home

    ...and no signal, no exit, no `app closed`. The app kept running with
    its surface mapped while the launcher showed it closed. The model now
    carries `kill_pending_pid`, captured at the moment of the close, and
    `kill_pending_app()` refuses to drop a request silently — it says so if
    it ever has no pid. The host test asserts the captured pid directly and
    was verified to fail against the pre-fix source.

    Worth noting how this survived: the gesture that reached the card was
    wrong for three separate reasons (bugs 37, 40, 42) before it ever
    arrived, so the close path had never actually executed on a VM. A unit
    test could have caught it at any point in that sequence.

44. **Gestures were timed against the local clock, so a slow machine turned a
    tap into a long press.** Every event was stamped with
    `clock_gettime(CLOCK_MONOTONIC)` at the moment the shell *processed* it,
    so `long_press_ms` was measuring how busy the shell was, not how long the
    finger was down. On a booted VM this produced

        astrix-shell: pointer button 272 pressed at 400,598
        astrix-shell: pointer button 272 released at 400,598
        astrix-shell: long-press from 400,598 to 400,598: home -> power-menu

    — a tap that opened the power menu instead of the app, with the press and
    release 13 ms apart in the log and the shell reaching them 600 ms apart.
    Wayland already stamps `wl_pointer` and `wl_touch` events with the
    compositor's own `CLOCK_MONOTONIC` reading, which is the same clock, so
    the shell now uses that. Only `wl_pointer.enter`, which genuinely carries
    no timestamp, falls back to the local clock.

    This is the third bug in a row whose shape was "a value was right, but it
    was read at the wrong moment" — bugs 39, 40 and 44 all turned on timing
    or position being sampled somewhere other than where it is defined.

**On the three fixes that came before it.** Bugs 35, 37 and 38 are all
    real defects and all are fixed, but none of them was *this* bug, and I
    wrote bug 35's entry as though it were. What actually happened is that I
    had no way to see the anchor, so I theorised from the end position,
    "fixed" three plausible things, and each time the symptom came back and
    I theorised again. The one change that mattered came from printing the
    start point in the log — one line of diagnostics, after three
    speculative fixes. The reason it took three boots to find is that the
    evidence needed was already inside the system and simply was not being
    printed.

## 6. Known limitations of the build itself

- **Slow.** The GUI is cross-compiled by running the target's own `gcc` under
  `qemu-user` emulation, because the host glibc (2.35) and the target glibc
  (2.41) differ. It is correct and it is slow.
- **QEMU boot under TCG is very slow** — roughly 190 s of guest time to reach
  `first-boot-complete`, and longer again to the graphical session, on an
  emulated single core.
- **The build host has no FAT kernel module**, so the ESP is written with
  `mtools` and never mounted. `scripts/build-image.sh` depends on this.
- **No CI.** The test suite runs by hand on the build host.
- **`SOURCE_DATE_EPOCH` is pinned** for reproducibility, but the build has not
  been verified to produce byte-identical images across two clean runs.
