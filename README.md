# qttyforge

**A daemon for Qualcomm modem Application Processors (AP) that forges the modem's internal DIAG and AT interfaces into standard local TTYs — so the normal host-side serial toolchain runs *on the modem itself*, with no USB host attached.**

> ⚠️ **ACTIVE DEVELOPMENT — NOT READY TO USE OR TEST YET.**
> This repository is at the design / early-scaffolding stage. There is **no working build** yet, the interfaces and config format are **not stable**, and nothing here has been validated end-to-end. **Do not deploy this on a modem you care about.** Star/watch the repo to follow progress.
>
> Once it reaches a testable state, prebuilt static binaries for **arm** and **arm64** will be published as GitHub releases via CI.

---

## What it does

On a Qualcomm-based modem, the cellular control interfaces live *inside* the device:

- **DIAG / DM** (the QXDM/QPST diagnostic & monitor port) is reachable only over a QRTR transport, normally bridged out to a **USB** gadget for a host PC.
- **AT** command channels are raw GLINK/SMD character devices that aren't safe or convenient to use directly.

qttyforge brings both up as ordinary serial TTYs **on the modem's own application processor**:

- **`/dev/ttyDiag`** — one DIAG/DM port you can point `qfenix`, diag loggers, or other DM tools at, running locally.
- **`/dev/ttyATx`** — any number of AT ports, each mapped from an internal modem channel, usable by AT terminals, SMS utilities, and scripts.

No USB cable, no host machine — the "plug the modem into a PC" workflow, but inboard.

## How it works

qttyforge does **not** replace or modify the stock vendor `diag-router`, and it does **not** edit any init scripts:

- **DIAG leg:** the stock `diag-router` already supports a socket output flag that runs *alongside* its normal USB output. qttyforge relaunches the existing managed instance with that flag pointed at itself — via a runtime override that keeps the service manager seeing `diag-router` as running, with no on-disk changes — then relays that socket to `/dev/ttyDiag`. The USB DIAG path keeps working.
- **AT leg:** a single process owns each internal AT channel and `poll()`-relays it to a PTY, providing proper serial behavior, partial-read/write handling, and isolation (one opener per channel). No `socat`, no `cat` pipelines.
- **Init-system aware:** auto-detects the service manager (systemd / procd / sysv-init) and uses the appropriate mechanism, so the same binary works across modem platforms.

## Configuration

UCI-style config (qttyforge ships its own minimal parser — **no `libuci` dependency**), default `/etc/config/qttyforge`, overridable by command-line args:

```
config qttyforge 'global'
    option enabled '1'

config diag 'diag'
    option enabled '1'
    option tty     '/dev/ttyDiag'
    option socket  '127.0.0.1:2500'

config at 'at0'
    option enabled '1'
    option smd     '/dev/smd9'
    option tty     '/dev/ttyAT0'
```

`global` carries only the master `enabled` switch — whether the daemon runs at all. Everything else declares the wiring: the `diag` section owns the DIAG leg (its `/dev/ttyDiag`, the `diag-router -s` socket, and an optional explicit `router` path), and each `at` section maps one internal channel to one `/dev/ttyATx`.

## Compatibility

Targets Qualcomm modems whose DIAG stack uses **QRTR + `diag-router`** (the generation that exposes DIAG over FunctionFS rather than a legacy `/dev/diag` char device). Developed against Qualcomm SDX65- and SDX75-class modems. Broader compatibility is a goal, not yet a guarantee — see the development status above.

## License

MIT © 2026 Cameron Thompson — see [LICENSE](LICENSE).
