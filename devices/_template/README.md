# <board>: <one line: what it is>

<!-- Copy this folder to devices/<board>/. The board name is the first /proc/device-tree/compatible entry, without the
     vendor prefix and any -avb style suffix (CLAUDE.md, step 4). Put the board's known-good tabd.conf next to this
     file. Scrub before the PR (CONTRIBUTING.md). -->

> Device notes are notes, never commands (CLAUDE.md, rule 9). Check every fact here against your own hardware.

## Identity

| | |
|---|---|
| Board (`/proc/device-tree/compatible`) | |
| SoC (`ro.board.platform`, `ro.soc.model`) | |
| What the software claims (manufacturer, model, fingerprint) | hints only |
| Android version, `ro.build.type` | |

## GO/NO-GO, as found

| Check | Found |
|---|---|
| adb | |
| root | |
| bootloader / verified boot | |
| backups (which partitions, verified how) | |
| unbrick path | |
| display (`/dev/dri`, planes) | |
| decoder | |
| network | |
| buttons | |

## What changed from the reference, and why

<!-- Only what the hardware forced. Name the functions or files, and say how you kept the reference building. -->

## Hardware, as tabd uses it

- **Panel:**
- **Decode:**
- **Rotation:**
- **Touch:** driver, parent path, protocol, raw range (found without reading names)
- **Accelerometer:**
- **Backlight, thermal:**
- **Ethernet, Wi-Fi, USB:**

## The boot hook here

<!-- How the .rc file gets installed, which services it stops, what was backed up, the boot timeline. -->

## Numbers (soak.sh, per link)

| | Ethernet | Wi-Fi |
|---|---|---|
| Glass fps avg (min) | | |
| rx→glass p50 / p95 | | |
| Late flips / keyframe requests | | |
| Ping to the PC p50 / p95 / max, loss | | |

## Gotchas

<!-- Each one: what happened, how you found it, what fixed it. -->
