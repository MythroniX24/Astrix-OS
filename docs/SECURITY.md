# Astrix OS — Security

This document describes the security model, what is actually enforced today,
and — just as importantly — **what is not yet enforced**. Anything listed under
"Known gaps" is a real gap, not a future roadmap item that can be glossed over.

---

## 1. Threat model

A phone is a remote-input device that holds the user's identity, their
credentials, their photographs and their money. The realistic attackers, in
order of how much they matter to a design:

| # | Attacker | Capability |
| --- | --- | --- |
| 1 | A malicious app | Ordinary unprivileged code running as the session user, with the user's permissions |
| 2 | A hostile network | Sees all unencrypted traffic, can MITM anything that does not verify certificates |
| 3 | A lost or stolen device | Physical possession, unlocked or locked |
| 4 | A malicious update | Controls what the device runs on next boot |
| 5 | A remote exploit in a system service | Code execution as root |

Astrix's design is aimed at (1) and (3) hardest, because those are the ones a
user cannot avoid by being careful.

---

## 2. The core rule: the UI is not root

Nothing that draws to the screen runs as root. The entire graphical session —
compositor, shell, every app — runs as `astrix`, uid 1000.

This is enforced in the units themselves:

```ini
[Service]
User=astrix
Group=astrix
```

It means a bug in the compositor, the shell, the gesture code or a text
renderer is a bug as an unprivileged user. It is the single highest-value
decision in the project, and everything else in this document is a refinement
of it.

### 2.1 Privilege inventory

What actually runs as root:

| Unit | Why it needs root |
| --- | --- |
| `systemd` (PID 1) | Init. |
| `astrix-session.service` | Creates the runtime directory, launches the session, uses `loginctl`. |
| `astrix-boot-report.service` | Reads unit state and journals for diagnostics. |
| `seatd` | Owns the logind seat and opens the input devices. |
| `NetworkManager` / `systemd-resolved` | Network configuration. |
| `PipeWire` / `WirePlumber` | Audio device ownership. |
| `polkitd`, `udev`, `systemd-*` daemons | Standard Debian system services. |

That is the entire list. There is no other privileged helper, and in particular
there is no `astrix-pkg-helper`-style root daemon in the tree — the design for
one exists, and the design says it must be reached only through polkit, but it
**has not been written**. See §5.

---

## 3. Capability bounding

The compositor is the one component with a genuine privileged need: it must
own the DRM master for the seat. That is scoped as tightly as possible.

`system/services/astrix-compositor.service`:

```ini
User=astrix
DeviceAllow=/dev/dri/renderD128 rw
DeviceAllow=/dev/dri/card0 rw
DeviceAllow=/dev/input/event* rw
SupplementaryGroups=input render video seat audio
AmbientCapabilities=CAP_SYS_NICE
CapabilityBoundingSet=CAP_SYS_NICE CAP_DAC_READ_SEARCH
```

`CAP_SYS_NICE` is for RTKit, so the compositor gets real-time scheduling
*without* running as root. Everything else in the bounding set is dropped at
exec time and cannot be reacquired — `NoNewPrivileges=yes` is also set, so no
setuid binary can reintroduce a capability.

`system/services/astrix-shell.service` is stricter still:

```ini
CapabilityBoundingSet=
AmbientCapabilities=
NoNewPrivileges=yes
```

The shell needs no Linux capabilities at all. It draws.

### 3.1 Supplementary groups are a real failure mode

A missing `seat` group did not produce a warning. systemd aborted the unit
with `status=216/GROUP` and — because the compositor's output was going only
to the journal — nothing appeared anywhere a human was looking. `seat` is now
created explicitly in `build-rootfs.sh`, and the boot report would surface it
again if it were ever removed.

---

## 4. Filesystem sandboxing

Both graphical units use `ProtectSystem=strict`, which mounts the entire
filesystem read-only except for the paths each unit declares:

```ini
# compositor
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
RestrictNamespaces=yes
RestrictSUIDSGID=yes
MemoryDenyWriteExecute=yes
SystemCallArchitectures=native
SystemCallFilter=@system-service

# shell
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=/home/astrix/.local/share/astrix
StateDirectory=astrix
PrivateTmp=yes
PrivateDevices=yes
```

So the compositor cannot write to the system at all, and the shell can write
exactly one directory: its own state.

### 4.1 `%h` is not the service user's home

A subtle and expensive one. In `ReadWritePaths=`, systemd's `%h` expands
against the **manager's** home — `/root` — not the home of `User=astrix`. The
obvious-looking `ReadWritePaths=%h/.local/share/astrix` silently produced:

```
Failed to set up mount namespacing: /root/.local/share/astrix
```

and the unit died at `status=226/NAMESPACE`, before `ExecStart` was reached.
The path is now written out literally as `/home/astrix/.local/share/astrix`,
and `build-rootfs.sh` creates it, so the two cannot drift.

---

## 5. Package installation

`astrix-package-manager` is an unprivileged front-end. Installing a system
package requires root, so the design is:

- the UI is a normal Wayland client running as `astrix`;
- a privileged helper performs the actual `apt` operation;
- the user authorises the helper **through polkit**, in a system dialog, with
  the operation and its arguments shown.

`polkitd` is in the shipping package set and `sudo` is installed for the
development image. The helper itself (`system/astrix-pkg-helper`) **has not
been written**. Until it exists, `astrix-package-manager` cannot install
anything, and the UI says so rather than pretending.

---

## 6. Updates

Not implemented. The design is in `docs/DESIGN.md` §7.2: A/B slots, signed
payloads, bootloader-mediated switching, boot-success confirmation, automatic
revert. Until that exists, **the honest security statement is that Astrix
cannot be updated on a device it produced**, and that must be fixed before any
real deployment.

---

## 7. Device loss and at-rest data

Currently: **nothing beyond what Debian provides.**

- No full-disk encryption. A lost, powered-off device yields its filesystem.
- No secure lock screen. The session autologins (§8).
- No remote wipe, because there is no device management.

Debian's `cryptsetup`/`fscrypt` and the kernel's `dm-crypt` are the intended
foundation for per-user encryption via `fscrypt`, which is the right shape for
a phone: it protects a user's data without forcing a full-device passphrase on
a device that has to boot unattended. None of this is built.

---

## 8. Autologin — a deliberate development default

`astrix-session.service` logs `astrix` in with no password, because a phone
goes straight to the home screen.

This is correct for a development image and **wrong for a shipping device**.
A production system must:

- require a screen lock, with the compositor not presenting a frame until it
  is unlocked;
- hold the session in a locked state so that the renderer is not driven at all
  while locked (otherwise a locked phone leaks its notification content);
- accept a PIN or biometric, with the secret verifier hardened appropriately.

None of that exists. The QEMU image is a development image and is not a
security model for anything.

---

## 9. Application sandboxing

**Not implemented.** Every app runs as `astrix`, with that user's full access
to that user's files. A malicious app is confined only by the uid boundary.

The intended model, for later:

- a per-app uid/gid, so apps are mutually invisible;
- a mount namespace per app, with only its data directory writable;
- a seccomp filter per app, from a profile rather than one blanket
  `SystemCallFilter`;
- `landlock` for filesystem access, which is the right primitive here because it
  is unprivileged and path-based;
- a declarative permission list shown to the user at install time.

---

## 10. Network security

Provided by stock Debian, and adequate for the parts that exist:

- `ca-certificates` for the trust store, so TLS verification actually works.
- `apt-transport-https` and `debsig-verify` so packages are fetched over TLS
  and their signatures checked beyond the apt keyring.
- `systemd-resolved` with DNS-over-TLS available.

Not implemented: certificate pinning, a VPN service, private DNS per-network.

---

## 11. Known gaps, in one list

| Gap | Severity | Status |
| --- | --- | --- |
| No full-disk / per-user encryption | High | Not implemented |
| No secure lock screen; passwordless autologin | High | Development default, documented |
| No per-app sandboxing; all apps share one uid | High | Not implemented |
| No A/B verified updates | High | Designed, not implemented |
| No secure boot / image signing on device | High | Not implemented |
| Android compatibility not confined to a sandbox | High | Not implemented (see `docs/ANDROID.md`) |
| No privileged package helper wired to polkit | Medium | Not implemented; UI says so |
| No `debsig-verify` enforcement in the shipped apt config | Medium | Package present, config not yet strict |
| No rate limiting on the lock screen | Medium | No lock screen to rate-limit |

None of these are hidden behind working-looking UI. That is deliberate: the
alternative is an OS that lies about its own security, which is worse than one
that admits it is early.
