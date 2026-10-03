# Porting Astrix to a real phone

This is the document for "bro boot it with display on my moto g64 5G / Redmi 8A".

It is written as an ordered path, because that is the only honest way to write
it. **No phone has ever booted Astrix.** Not because the userspace is wrong,
but because two things have to exist before a phone can show anything, and
neither does yet:

1. a driver that can drive that SoC's display hardware, and
2. a bootloader and device tree that can reach it.

The honest state of each is in `docs/DEVICES.md` and is enforced by
`scripts/device.sh check`, `scripts/build-device.sh` and
`scripts/flash-device.sh` — all three refuse, and the test suite proves they
refuse. Nothing in this repository will let you flash a phone and be told it
worked.

So this document is the path. Everything in steps 0–2 can be done today, today,
on the hardware you have.

---

## Step 0 — the arithmetic (10 minutes, no hardware)

Before writing a driver, know whether the panel can be driven at all.

```bash
./scripts/display-budget.sh --all
```

| | moto g64 5G | Redmi 8A |
|---|---|---|
| panel | 1080x2400 @ 120Hz | 720x1520 @ 60Hz |
| pixel clock | 376.4 MHz | 79.5 MHz |
| DSI payload @ 24bpp | 9.03 Gbit/s | 1.91 Gbit/s |
| required per lane (4 lanes) | **2.258 Gbit/s** | 476.7 Mbit/s |
| per-lane capability | 2.5 Gbit/s | 2.5 Gbit/s |
| headroom | **10%** | 81% |

Two conclusions come straight out of it:

* **The moto g64 5G at 120Hz has 10% headroom.** That is enough to attempt and
  not enough to trust. DSI link training is sensitive to ribbon cable length,
  temperature and part-to-part D-PHY variation, and when it fails it fails as a
  flicker or a panel that lights up on the bench and not on the handset — which
  looks exactly like a driver bug. **Bring it up at 60Hz first**
  (1.13 Gbit/s per lane, 55% headroom) and only chase 120Hz once the whole chain
  is proven. You can re-run the tool with any numbers by editing the profile.
* **The Redmi 8A has bandwidth to spare and still cannot boot**, because Adreno
  505 has no mainline driver at all. A comfortable budget does not create a
  driver. If you are choosing where to spend a month, the moto g64 5G is the
  answer: Panfrost is a real, open, upstream driver for its Mali-G57, so the GPU
  half of the problem is already solved somewhere.

Blanking percentages in the profiles are **assumptions** (10% per axis) and are
labelled as such in every file. Real porch values come out of a working device
tree, in step 2.

---

## Step 1 — unlock the bootloader

Nothing can be flashed before this, and neither step can be shortened from
inside this repository.

**Motorola moto g64 5G (retin)** — official route. Request a token from
Motorola's developer portal using the device IMEI; the token is emailed; then
`fastboot flashing unlock`. Some retail variants are refused outright with
"device not qualified", so this can end the port before it starts. Check this
first — it is the cheapest thing to find out and the most common reason a port
never begins.

**Xiaomi Redmi 8A (olive)** — Mi Unlock tool, a Xiaomi account bound to the
device, and a waiting period (commonly 72h, extended without notice), on
Windows. This is a multi-day lead time. Start it before anything else.

```bash
fastboot devices
fastboot getvar unlocked      # must answer yes before any flashing script runs
```

`scripts/flash-device.sh` refuses to run at all against a locked bootloader and
prints exactly this requirement.

---

## Step 2 — a serial console, before anything else

This is the step people skip, and it is the step that decides whether the port
takes a week or a month.

**Never bring up a display without a serial console.** If you cannot see the
screen, you cannot tell "the kernel panicked" from "the panel is not wired
correctly" from "the panel is wired correctly and the GPU crashed". One
`printk` on a UART answers all three. Debugging blind is the single biggest
cause of display ports dying.

What to do:

1. Build a mainline kernel for the board (Armbian publishes one for MT6855;
   `DEVICE_PORT_REFERENCES` in the profile points at the community trees).
2. Find the board's UART. On MT6855 the debug UART is usually UART0 at 115200
   8N1 — **the pinout must be confirmed against the board's schematics or a
   working community kernel's `.dts`; it is not guessed here.** Connect a USB-TTL
   adapter (3.3V logic level, cross TX/RX, common ground).
3. Put `earlycon` and a serial `console=` on the kernel command line so you see
   output before the filesystem exists. If you see one line of kernel output,
   the console works and everything after this is debugging, not archaeology.
4. **Define done for the whole display port: a kernel that prints a known
   string over serial.** Not a picture. A string.

Record the console output as you go. A bring-up log with real lines in it is
worth more than any status document, and `docs/STATUS.md` only gets honest
because of logs like it.

---

## Step 3 — the device tree

There is no upstream device tree for `retin` or `olive`. A device tree for this
board has to describe, at minimum:

* the **regulators** that power the panel, the DSI PHY and the rails around them
* the **clocks** feeding the DSI PLL and the pixel clock
* the **GPIO** sequence for panel reset, and the `reg-enable` GPIO that tells
  the panel it may start
* the **DSI host** node, wired to a **panel node** through a `ports` /
  `endpoint` graph
* the panel's own timings: `panel-width`, `panel-height`, porches, sync, and
  the DSI format and lane count

A reasonable approach is to start from the reference port's device tree
(LineageOS `device/retin` plus `kernel_mediatek`), delete everything to do
with Android, and treat what remains as the board description. That is a real,
doable path — it is just weeks of work, and it is why step 0 exists.

**When you have real porches, put them in `config/devices/moto-g64-5g.conf`**
(`DEVICE_PANEL_HBLANK_PERCENT` and friends, or better, extend the profile with
explicit porch values) and re-run `./scripts/display-budget.sh`. The budget will
move, and if it moves above the link's capability, the panel needs a lower
refresh mode before any driver work is worth doing.

---

## Step 4 — the display driver chain, in the order it actually comes up

This is the DRM pipeline as it initialises. Each stage is separately observable,
and the order matters: never debug stage 5 when stage 2 never ran.

| # | Stage | Success looks like |
|---|---|---|
| 1 | DSI PHY + clocks probe | `dsi_phy` and the PLL clocks register; no `-EPROBE_DEFER` left behind |
| 2 | DSI host probes | the host driver binds and reports the lane count it found |
| 3 | Regulator/GPIO sequence | the panel's enable line is asserted, reset is pulsed, no regulator probe failure |
| 4 | Panel attaches | the panel's `of` node is matched and its mode is added |
| 5 | Connector registered | a DRM connector appears in `/sys/class/drm` |
| 6 | CRTC + planes | a CRTC can be set to the panel's mode |
| 7 | fbdev emulation | `/dev/fb0` exists and a test pattern can be blitted |

Verifying each of those **without looking at the phone** is what
`scripts/panel-check.sh` automates. It runs unchanged here (where it reports
the virtio-gpu chain) and on the handset over adb, and prints one line per
stage:

```console
# on the handset
adb shell astrix-panel-check
# or, from this repository
./scripts/panel-check.sh
```

Exit status is non-zero while any stage is missing, so it can be the last line
of a bring-up script. The underlying evidence it reads:

```bash
# 1-4: the kernel tells you what it managed to bring up, and where it stopped
dmesg | grep -Ei 'drm|dsi|panel|connector|regulator|phy|eprobe|error|fail'

# 5-6: the DRM objects that exist. This is the real "is there a display"
# question, and it is answerable from a shell.
ls /sys/class/drm/
cat /sys/class/drm/card0-DSI-1/status      # connected / disconnected / unknown
cat /sys/kernel/debug/dri/0/state          # crtcs, connectors, planes, encoders

# 7: fbdev
ls -l /dev/fb0
```

`modetest -c -p` (from `libdrm-tests`/`kms-tools`) prints every connector and
mode the driver believes in. If a mode is listed as available and the panel
still shows nothing, the problem is electrical (reset/enable/power) or the DSI
timings, not the mode-setting code. That distinction is worth ten pages of
dmesg.

A useful sanity check that needs no panel at all: **`drm_kms_helper`'s
`debugfs` state above is populated as soon as the connector exists.** A
connector that never appears means stage 4 failed; that is a completely
different bug from a mode that fails to light up.

Once stage 7 works, Astrix's own stack is the easy part: `astrix-compositor`
starts on any DRM/KMS device, and the shell draws into it. `WLR_BACKENDS=drm`
is all that is needed — the same binary that runs in QEMU runs on a phone.

---

## Step 5 — the Astrix boot chain

A phone does not need a bootloader written from scratch; it needs an unlocked
one to chainload an Android-style `boot.img`. Once a `boot.img` containing
Astrix's kernel and initramfs is accepted:

```bash
./scripts/build-device.sh <device>        # assembles build/device/<device>/
./scripts/flash-device.sh <device> --dry-run
./scripts/flash-device.sh <device> --flash
```

Both of those commands refuse today, and they will keep refusing until
`DEVICE_BOOT_STATUS=supported` and `DEVICE_VERIFIED_BOOT=yes` are both true in
the profile. **Those two variables are only set from evidence**: a recorded boot,
with the serial log, in `docs/STATUS.md`. Do not set them to get past a gate.
`tests/test-devices.sh` fails the build if a profile claims support without a
verified boot, and that test is one of the reasons this project is worth
trusting.

---

## How to test Astrix today, without either phone

The QEMU ARM64 machine runs the real kernel, the real compositor and the real
shell, and it is the only display Astrix has ever driven.

```bash
./build.sh                       # ~6 minutes; deletes and rebuilds the images
./run-qemu.sh --headless --mem 1024
```

The host is 2 GB, so the 2048 MB default will not start — `--mem 1024` is not
optional here. TCG emulation takes roughly 190 seconds to `first-boot-complete`
and about ten minutes to a mapped shell; that is the machine being slow, not
Astrix hanging.

To see the UI without a monitor:

```bash
# synthetic taps, in normalised coordinates
ASTRIX_SCREEN_W=1024 ASTRIX_SCREEN_H=768 ASTRIX_POINTER_SCALE=2.0 \
  ./scripts/send-input.sh tap 0.39 0.78

# the shell's own rendered frames (the compositor runs pixman under QEMU, so
# these are software frames)
ls build/screens/
```

Two limits worth stating rather than discovering:

* QEMU's virtio mouse is **relative only** — there is no absolute pointing
  device and no multi-touch, so `ASTRIX_POINTER_SCALE` exists to make relative
  motion cover the screen. Real multi-touch is **not** exercised by any test in
  this repository and is not claimed to be.
* `fastboot`-style screenshots of the console are not the compositor's output.
  The UI is inspected through `build/screens/`.

Run the host test suite — 39 tests, no hardware, no QEMU — with:

```bash
./tests/run-all.sh --fast
```

---

## Bring-up log template

Copy this per device and fill it in with real output. Empty logs are how a port
quietly stops.

```
device:      moto g64 5G (retin)
board:       XT2431-1
bootloader:  unlocked <date>  (token <date>)
uart:        <port>, 115200 8N1, first output seen: <date>
--- step 2: serial console ---
<first line of real kernel output>
--- step 3: device tree ---
regulators described: ...
panel timings read from DT: h<>, v<>, hsync <>, vsync <>
re-run display-budget.sh with these porches: result
--- step 4: display chain ---
1 DSI PHY:   <ok/fail, dmesg line>
2 DSI host:  <ok/fail, dmesg line>
3 GPIO/reg:  <ok/fail, dmesg line>
4 panel:     <ok/fail, dmesg line>
5 connector: <ls /sys/class/drm output>
6 CRTC:      <drm debugfs state>
7 fbdev:     <ls /dev/fb0>
--- step 5: Astrix ---
astrix-compositor started, output <W>x<H>
astrix-shell mapped
screen:      lit / not lit
```