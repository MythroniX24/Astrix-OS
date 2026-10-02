# Wayland protocol definitions

wlroots' own `wlr-*` protocol extensions are not published in
`wayland-protocols`, and Debian's `libwlroots-0.18-dev` does not ship their
XML. A compositor that implements them therefore vendors the XML itself.

These files are taken verbatim from the wlroots 0.18.2 release
(`protocol/` in the wlroots source tree) so that what we build matches the
version Debian trixie ships. They are regenerated into C bindings by
`scripts/build-gui.sh` via `wayland-scanner`.

Standard protocols (`xdg-shell`, `wlr-layer-shell-unstable-v1`, ...) come from
the distribution's `wayland-protocols` package where available.

## `virtual-keyboard-unstable-v1.xml` needs wayland-scanner >= 1.21

The on-screen keyboard can only deliver a key to another client through a
*virtual keyboard*, and the only compositor-side implementation in our stack is
wlroots'. It advertises `zwp_virtual_keyboard_manager_v1` (note the `zwp_`
prefix, not the older `zwlr_`; wlroots 0.18 renamed it).

That protocol declares `create_virtual_keyboard` as **seat first, new_id
second**. wayland-scanner before 1.21 silently moves a `new_id` argument to the
front, which produces a client whose metadata says `no` while the server
expects `on`. Nothing complains at build time: the client marshals the wrong
order, libwayland rejects the null object argument, and the shell exits a
second after starting with a protocol error nobody can connect to this file.

The build inside the rootfs uses Debian trixie's `wayland-scanner` (1.23), which
handles it correctly. The native build path uses whatever the host has, so the
host needs 1.21 or newer for `./scripts/build-gui.sh --native` to produce a
working keyboard.
