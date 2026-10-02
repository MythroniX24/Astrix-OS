#!/usr/bin/env python3
"""Drive real input events into a running QEMU guest over QMP.

Used by scripts/send-input.sh, which owns the argument parsing and the
user-facing messages. Kept as its own file so there is exactly one
implementation of the QMP handshake, and so the tap/swipe/hold/key paths all
talk to the emulator the same way.

What matters here is that nothing is faked: these are the same events a USB
tablet sends, delivered through QEMU's input subsystem, so the guest sees them
on an evdev node and libinput has to do all the work.
"""

import json
import os
import socket
import sys
import time

# QEMU's virtio input devices use a 0..0x7FFF absolute range, regardless of the
# guest's screen size. (VIRTIO_INPUT_ABS_MAX.) The corners are therefore sent
# exactly rather than approximated.
VIRTIO_ABS_MAX = 0x7FFF


class QmpError(Exception):
    pass


class QmpClient(object):
    """One QMP connection, reused for every event in a run.

    A connection per waypoint would work but is both slow and racy: QEMU
    serves one QMP client at a time, so re-dialling immediately after a failed
    command can block waiting for the previous socket to be released. Keeping
    the socket open for the whole run removes both problems.
    """

    def __init__(self, sock_path, device=None):
        self.device = device
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(30)
        self.sock.connect(sock_path)
        self.f = self.sock.makefile("rw", encoding="utf-8", newline="\n")
        self.f.readline()  # QMP greeting
        self.execute({"execute": "qmp_capabilities"})

    def execute(self, cmd):
        self.f.write(json.dumps(cmd) + "\n")
        self.f.flush()
        reply = json.loads(self.f.readline())
        if "error" in reply:
            raise QmpError(
                "QMP %s failed: %s" % (cmd["execute"], json.dumps(reply["error"]))
            )
        return reply

    def send(self, events):
        """Send input events, degrading to relative-only if abs is refused."""
        global _supports_abs
        if self.device:
            # "qemu-input-device" is carried per event, not per command.
            for ev in events:
                ev["qemu-input-device"] = self.device
        try:
            return self.execute(
                {"execute": "input-send-event", "arguments": {"events": events}}
            )
        except QmpError as exc:
            if "event type abs" not in str(exc) or _supports_abs is False:
                raise
            relative_only = [ev for ev in events if ev["type"] != "abs"]
            if not relative_only:
                raise
            _supports_abs = False
            print("note: this input device has no absolute axes; using relative motion")
            return self.execute(
                {"execute": "input-send-event", "arguments": {"events": relative_only}}
            )

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def abs_event(axis, value):
    return {"type": "abs", "data": {"axis": axis, "value": value}}


def rel_event(axis, value):
    return {"type": "rel", "data": {"axis": axis, "value": value}}


# QEMU's pointing devices differ in how far they move: an absolute one (a real
# tablet) reacts to abs events and ignores rel, a relative one (virtio-mouse)
# rejects abs outright with "Input handler not found for event type abs" and
# only moves on rel. The device Astrix boots with today is the relative one.
#
# So the capability is probed once per run rather than assumed: the first
# motion event carries both forms, and if QEMU refuses the absolute one the
# rest of the run is relative-only. That keeps the tool correct on both kinds
# of device without the caller having to know which one is attached.
#
# The virtual field is the size of the guest's output in pixels, because a
# relative device moves in pixels and the normalised coordinates a caller
# passes have to be scaled by something. It is overridable because the guest
# panel is a property of the machine, not of Astrix:
#
#   ASTRIX_SCREEN_W=720 ASTRIX_SCREEN_H=1600 scripts/send-input.sh tap 0.5 0.5
#
# On QEMU the panel is whatever virtio-gpu's preferred mode is (1024x768 by
# default, which is why that is the default here).
SCREEN_W = float(os.environ.get("ASTRIX_SCREEN_W", "1024"))
SCREEN_H = float(os.environ.get("ASTRIX_SCREEN_H", "768"))

# libinput applies pointer acceleration to a relative device, so a delta of N
# does not move the cursor N pixels: it moves it more, by a factor that grows
# with speed. Measured on this QEMU guest it is a clean 2.0 - a request for
# (102, 77) arrives at (204, 154) - so the deltas are halved to compensate.
#
# This is an empirical correction for a virtual relative mouse, not a
# property of Astrix, which is why it is an environment variable and not a
# constant baked into the OS. On real hardware the absolute path is used and
# no scaling applies at all.
POINTER_SCALE = float(os.environ.get("ASTRIX_POINTER_SCALE", "2.0"))
_supports_abs = None  # unknown until QEMU answers


class Pointer(object):
    """Tracks the pointer's position so relative deltas can be computed.

    A relative device has no idea where it is, so the position has to be
    established before the first delta means anything. The compositor clamps
    the cursor to the output, which gives a reliable way to do that: drive it
    hard towards the top-left corner until it stops moving, and it is at
    (0,0) by definition. From there every delta is exact.
    """

    def __init__(self):
        self.x = 0.0
        self.y = 0.0
        self.homed = False

    def home_events(self):
        """Events that pin the cursor into the top-left corner.

        Repeated rather than one huge delta because libinput applies pointer
        acceleration and clamps large movements, so a single -10000 is not
        guaranteed to travel the whole distance.
        """
        return [
            [rel_event("x", -128), rel_event("y", -128)] for _ in range(48)
        ]

    def goto(self, nx, ny):
        """Return the events that move the pointer to normalised (nx, ny)."""
        tx, ty = nx * SCREEN_W, ny * SCREEN_H
        events = [
            # A relative event names its axis "x"/"y" too - the type ("abs"
            # vs "rel") is what distinguishes it. "dx"/"dy" are rejected by
            # QMP with 'Parameter axis does not accept value dx'.
            rel_event("x", round((tx - self.x) / POINTER_SCALE)),
            rel_event("y", round((ty - self.y) / POINTER_SCALE)),
        ]
        if _supports_abs is not False:
            events = [
                abs_event("x", round(nx * VIRTIO_ABS_MAX)),
                abs_event("y", round(ny * VIRTIO_ABS_MAX)),
            ] + events
        self.x, self.y = tx, ty
        return events


def button(down):
    return {
        "type": "btn",
        "data": {"down": down, "button": "left"},
    }


def key_event(down, code):
    return {
        "type": "key",
        "data": {"down": down, "key": {"type": "number", "data": code}},
    }


def home(cli, p):
    """Pin the cursor at (0,0) so the first delta of a gesture is exact."""
    for events in p.home_events():
        cli.send(events)
    p.x, p.y, p.homed = 0.0, 0.0, True


def do_tap(cli, x, y):
    p = Pointer()
    home(cli, p)
    cli.send(p.goto(x, y))
    # Press and release in two commands with no sleep between them. Batching
    # them into one command is the tidier thing to write and it does not work:
    # a button press and release delivered inside a single evdev read are
    # coalesced away, and the compositor logs no button event at all. Keeping
    # them in separate commands also keeps a *tap* from arriving in the guest
    # as a long press - under emulation the host and the guest compete for
    # one CPU, so any deliberate delay here is a delay the guest actually
    # experiences.
    cli.send([button(True)])
    cli.send([button(False)])
    print("tap at (%s, %s) -> %d,%d" % (x, y, round(x * SCREEN_W), round(y * SCREEN_H)))


def do_swipe(cli, x1, y1, x2, y2, steps):
    steps = max(int(steps), 2)
    p = Pointer()
    home(cli, p)
    cli.send(p.goto(x1, y1) + [button(True)])
    time.sleep(0.03)
    # One round trip per waypoint, with a real gap between them. The gesture
    # recogniser needs a stream of motion events spread over time: sending
    # every point in a single batch would arrive as one read and look to it
    # like a teleport rather than a drag, so no gesture would be recognised.
    for i in range(1, steps + 1):
        t = i / steps
        cli.send(p.goto(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t))
        time.sleep(0.02)
    time.sleep(0.05)
    cli.send([button(False)])
    print("swipe (%s,%s) -> (%s,%s) in %d steps" % (x1, y1, x2, y2, steps))


def do_swipe_hold(cli, x1, y1, x2, y2, steps, secs):
    """Swipe up, then keep the finger down.

    This is one gesture, not two. The shell recognises swipe-up-and-hold as
    the app switcher, and a swipe followed by a separate hold cannot produce
    it: do_swipe ends by releasing the button, which ends the touch contact
    and resets the recogniser's state, so the hold afterwards is an ordinary
    long press starting from nothing. The button must stay down across the
    whole motion, which is why this is its own function rather than a flag on
    do_swipe.
    """
    steps = max(int(steps), 2)
    p = Pointer()
    home(cli, p)
    cli.send(p.goto(x1, y1) + [button(True)])
    time.sleep(0.03)
    for i in range(1, steps + 1):
        t = i / steps
        cli.send(p.goto(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t))
        time.sleep(0.02)
    # Held at the end position, motionless. The recogniser only latches the
    # hold once the swipe has been seen, so the pause has to come after the
    # motion, not before.
    time.sleep(float(secs))
    cli.send([button(False)])
    print("swipe-hold (%s,%s) -> (%s,%s) in %d steps, held %ss"
          % (x1, y1, x2, y2, steps, secs))


def do_hold(cli, x, y, secs):
    p = Pointer()
    home(cli, p)
    cli.send(p.goto(x, y) + [button(True)])
    time.sleep(0.03)
    # Real wall-clock time, not a synthetic delay: the guest timestamps the
    # event when it arrives, so a long press has to actually be held.
    time.sleep(float(secs))
    cli.send([button(False)])
    print("held at (%s, %s) for %ss" % (x, y, secs))


# QMP takes the QEMU QKeyCode *number*, not a name: sending the string "ret"
# is rejected with "Invalid parameter type ... expected: integer". QKeyCode
# values for printable keys are their ASCII code and the control keys are
# their ASCII control code, so the common names map without importing a
# keymap whose numbering would be the host's, not QEMU's.
KEY_NAMES = {
    "ret": 0x0D,
    "cr": 0x0D,
    "tab": 0x09,
    "esc": 0x1B,
    "spc": 0x20,
    "space": 0x20,
    "bs": 0x08,
    "backspace": 0x08,
    "lf": 0x0A,
    "del": 0x7F,
}


def do_key(cli, key):
    if key in KEY_NAMES:
        code = KEY_NAMES[key]
    elif len(key) == 1:
        code = ord(key)
    else:
        try:
            code = int(key, 0)
        except ValueError:
            sys.exit(
                "unknown key name %r; use a single character, one of %s, or a number"
                % (key, " ".join(sorted(KEY_NAMES)))
            )
    cli.send([key_event(True, code)])
    time.sleep(0.03)
    cli.send([key_event(False, code)])
    print("key %s (QKeyCode %d / 0x%x)" % (key, code, code))


def main(argv):
    if len(argv) < 4:
        sys.exit("usage: qmp_input.py {tap|swipe|swipe-hold|hold|key} <qmp-sock> [device] ...")
    action, sock, device = argv[1], argv[2], argv[3]
    rest = argv[4:]

    cli = QmpClient(sock, device or None)
    try:
        if action == "tap":
            x, y = float(rest[0]), float(rest[1])
            do_tap(cli, x, y)
        elif action == "swipe":
            x1, y1, x2, y2 = (float(v) for v in rest[:4])
            do_swipe(cli, x1, y1, x2, y2, rest[4] if len(rest) > 4 else 12)
        elif action == "swipe-hold":
            x1, y1, x2, y2 = (float(v) for v in rest[:4])
            do_swipe_hold(cli, x1, y1, x2, y2,
                          rest[4] if len(rest) > 4 else 12,
                          rest[5] if len(rest) > 5 else 0.9)
        elif action == "hold":
            x, y = float(rest[0]), float(rest[1])
            do_hold(cli, x, y, rest[2] if len(rest) > 2 else 1.2)
        elif action == "key":
            do_key(cli, rest[0])
        else:
            sys.exit("unknown action %r" % action)
    finally:
        cli.close()


if __name__ == "__main__":
    main(sys.argv)
