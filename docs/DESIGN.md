# Astrix OS — Design

This document covers the parts of Astrix that are design decisions rather than
platform facts: how the phone feels, how the shell is put together, how apps
are launched and killed, and what happens on a power button press.

---

## 1. Design principles

1. **One thing visible at a time.** A phone screen is small and interruptions
   are constant. The shell never shows a window list, a taskbar, or more than
   one app. What is on screen is what you are doing.
2. **The screen edge is the navigation.** There is no navigation bar to learn.
   Back is a gesture; home is a gesture; the launcher is a gesture. Every
   gesture is also available as a visible control somewhere, for anyone who
   cannot perform it.
3. **Direct manipulation only.** No nested menus to nowhere. If a control is on
   screen it does what it says.
4. **Latency is a feature.** The compositor must present a frame within one
   refresh of input. This is why the compositor is small and why the shell
   draws with a plain software canvas: there is no layout engine, no style
   recalculation pass, and no IPC to a compositor in the middle of a swipe.
5. **Honest about failure.** A missing capability shows as missing, in the UI,
   in the place where the user would have used it. It never shows as a
   spinner, or as a success that did not happen.

---

## 2. Input model

Input arrives at the compositor as libinput events and reaches the shell as
pointer motion, button press and button release. The shell classifies them.

### 2.1 Gesture vocabulary

| Gesture | Meaning |
| --- | --- |
| Tap | Activate the control under the finger. |
| Long press (500 ms) | Contextual action (app menu, text selection). |
| Swipe up from the bottom edge | Home / close the current app. |
| Swipe in from the left edge | Back, one level. |
| Swipe left/right | Switch between the home-screen pages. |
| Swipe down from the top edge | Notification shade. |
| Pinch | Zoom, where the content supports it. |

Recognition lives in `gui/shell/src/gesture.c` as pure logic over an event
stream: `(x, y, time, type) → gesture`. It has no dependency on Wayland, which
is why `tests/test-gestures.c` can exercise it natively on the build host
instead of needing an emulator. This is a deliberate design constraint: input
recognition is exactly the sort of code that is painful to debug through a
serial console, so it is kept testable in isolation.

### 2.2 Why edges and not on-screen buttons

A software navigation bar costs 48 dp of a 720×1600 panel — about 3% of the
screen — and it is the first thing to become a habit you have to unlearn. Edge
gestures cost no screen space. The trade-off is discoverability, which is why
the first-launch flow teaches the gestures and why every gesture also exists as
a visible control in the shade and in Settings.

---

## 3. The Shell

`gui/shell/` is a single Wayland client that owns the surface the user looks
at, for the entire session. It is not a window manager; the compositor has no
window-management policy either. The shell is the UI.

```
   shell.c    screen stack, navigation, app lifecycle, event routing
   gesture.c  event stream → gesture (pure logic, unit-tested)
   input.c    libinput-style events → shell events
   render.c   shell state → gui/ui draw calls
   main.c     Wayland/xdg-shell client setup
```

### 3.1 Screen stack

The shell maintains a stack of screens. Each screen renders into the shared
canvas and reports the region it wants touch events for.

```
   [ shade ]          pulled down over whatever is below
   [ app     ]        an app, full screen
   [ home     ]       launcher pages, status bar
```

Pushing is `shell_push()`, popping is `shell_pop()`. The back gesture pops;
if the stack is one deep it goes home rather than exiting, because a phone app
that can lose your place is a phone app you stop trusting.

### 3.2 Frame loop

The shell renders on demand, not on a timer. Input, a state change or a timer
(expiry, animation frame) marks the shell dirty; the frame callback then
redraws the whole screen into the shm buffer and commits. On a static screen
this costs nothing, which is most of the time on a phone.

### 3.3 The on-screen keyboard

The keyboard is the shell's, not the compositor's: it is part of the system UI,
drawn by the shell into its own buffer, and it is dismissed by a `hide` key
rather than by tapping outside, because there is no outside on a phone.

Design rules, each of which exists because the alternative broke something:

- **It consumes the press, not the tap.** A key takes the event on *press*, so a
  drag that starts on a key never also becomes a swipe on the screen behind it.
  A keyboard that leaked taps to the home screen underneath it was the first
  thing `tests/test-keyboard` caught.
- **A tap is a character, not a keycode.** `gui/shell/src/keyboard.c` produces a
  codepoint; `main.c` turns it into an xkb keycode by walking the keymap's level
  0 and level 1. The layout can therefore be tested without a compositor, and
  the two halves (what was meant, what is typed) cannot silently diverge.
- **One-shot shift, not a sticky key.** Shift applies to the next character and
  releases. Caps lock is separate and explicit.
- **The keyboard scales with the panel.** Its area is `height/2`, not a constant
  that happens to fit one QEMU resolution; four panel sizes are asserted in the
  unit test, including the two target phones.
- **Empty rows are legal.** The symbol layer leaves the `zxcvbnm` row empty, and
  the layout code originally divided by that row's length — a SIGFPE, found by
  the unit test, on a layer nobody had ever run.

---

## 4. Application model

### 4.1 What an app is

An app is an executable in `/usr/bin` that:

- connects to `$WAYLAND_DISPLAY`,
- creates an `xdg_toplevel` with `app_id` set to its reverse-DNS name
  (`org.astrix.Terminal`),
- draws, and exits when told to.

That is the whole contract. It means any Wayland program on the system is a
valid Astrix app — `foot`, `weston-terminal`, anything — without modification.
Astrix does not have an app SDK it is trying to trap you with; `gui/ui` is a
convenience, not a requirement.

### 4.2 Lifecycle

```
   launcher tap
        │
        ▼
   shell asks the compositor to launch the app
        │                  (argv: /usr/bin/astrix-terminal)
        │                  (env:  WAYLAND_DISPLAY, XDG_RUNTIME_DIR, ASTRIX_APP_ID)
        ▼
   app creates its toplevel with matching app_id
        │
        ▼
   compositor maps it → shell receives the map event
        │
        ▼
   shell pushes the app screen, the status bar slides in
        │
        ▼
   home gesture → shell closes the surface → app exits
```

`app_id` is the join key. The shell does not track a PID for its own sake: it
recognises a surface by the `app_id` the app declares, which is what lets an
app crash and restart without the shell losing track of it.

The compositor and shell both run under `systemd`, and an app is started in
`systemd-run --user` scope so that a hung app is killable by unit name rather
than only by `kill -9`. The shell refuses to start a second instance of an app
that is already running and instead raises it — a phone should not have four
copies of the terminal.

### 4.3 The shipped apps

| Binary | What it does |
| --- | --- |
| `astrix-terminal` | A real terminal: PTY, line discipline, an on-screen keyboard, scrollback, and a selectable font. Runs ordinary shell commands — it is `forkpty` + a VT emulator, not a command whitelist. |
| `astrix-files` | File browser over the user's own storage, with size/type detail and delete. |
| `astrix-settings` | System settings that actually change the system, over D-Bus. |
| `astrix-sysinfo` | CPU, memory, storage, uptime, temperature, battery. Reads real `/proc` and `/sys`; the reason it exists is that on a phone you need to know what is eating the battery. |
| `astrix-package-manager` | The apt front-end for the user's own software. |
| `astrix-apk-manager` | The Android-app manager. **Currently a UI shell only** — see `docs/ANDROID.md`. |

---

## 5. The launcher

The home screen is a horizontally paged grid of app icons with a status bar
above and a gesture hint area below. Pages are paged, not infinite; there is a
fixed number because a phone launcher that scrolls forever is a launcher
nobody can finish looking at.

The launcher is also the **compatibility aggregator**: native Astrix apps,
third-party Linux software, and (once implemented) Android apps appear in the
same grid, distinguished by an icon treatment, because the whole point of
Android compatibility is that the user does not have to think about which kind
of app they are opening.

---

## 6. Session lifecycle

```
   power on
      │
      ▼
   bootloader → kernel → initramfs → systemd (PID 1)
      │
      ├─► local-fs, NetworkManager, PipeWire, seatd, …
      │
      ▼
   astrix-session.service            (root, oneshot)
      │  • enable-linger for `astrix`
      │  • seat-launch seat0
      │  • create /run/user/1000     ← without this the compositor has
      │  •                             nowhere to put its Wayland socket
      ▼
   astrix-session.target
      │
      ├─► astrix-compositor.service  (User=astrix, Restart=on-failure)
      │       takes the DRM master, binds $XDG_RUNTIME_DIR/astrix-0
      │
      └─► astrix-shell.service       (User=astrix, Restart=always)
              waits for the socket, connects, maps its toplevel
      │
      ▼
   astrix-boot-report.service        (root, oneshot, after a 15 s settle)
          prints unit states + journals to the console
```

**Autologin is a development decision, and a documented one.** A phone has one
user and no login prompt, so `astrix-session.service` logs `astrix` in without
a password. A shipping device must require a screen lock; see
`docs/SECURITY.md`.

The `Restart=` policy is chosen per unit by what failure means. The compositor
uses `on-failure`: a clean exit means the GPU was handed back and there is
nothing to retry. The shell uses `always`: if the shell is gone, the user is
looking at a black screen, so it must come back whatever happened.

**The boot report is not optional.** It is the only diagnostic channel that
works when the failure *is* the display. Two of the bugs fixed during this
project (`status=216/GROUP`, `status=226/NAMESPACE`) were completely silent
without it and obvious with it.

---

## 7. Power and update design

### 7.1 Power

The power button is a shell-level concern: a long press shows the power menu
(power off, restart, reboot to bootloader), a short press locks the screen.
Both go through `systemd`'s logind (`PowerKeyIgnorePressed=`, lid handling), so
the policy lives in one place rather than being reimplemented in C.

Screen timeout, brightness and the do-not-disturb mode are Settings
preferences, persisted under the user's `~/.local/share/astrix`.

### 7.2 Updates

The design, which is **not implemented**:

- A/B root partitions with an inactive slot, so an update that bricks the
  device still boots.
- The update payload is a signed, versioned squashfs or a delta against the
  previous image.
- The *bootloader* switches slots, not the running system, and the update
  applies to the inactive slot so a power cut mid-write is survivable.
- Boot-success is signalled from userspace; if the new slot does not report in
  within a timeout, the bootloader reverts.

`system/updates/` is the placeholder for this. `docs/STATUS.md` records it as
unimplemented, and nothing in the UI currently claims otherwise.
