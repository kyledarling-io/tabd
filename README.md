# tabd: turn a rootable Android into a dedicated monitor

An old Android tablet becomes a wired touchscreen monitor for your Windows PC, and nothing else: no Android UI, no apps.
Point your Claude at this repo and it adapts it to your tablet.

<!-- DEMO: a photo or short clip of the tablet below the main monitor goes here -->
> _Photo or clip coming._

## What it is

On the PC, [Sunshine](https://github.com/LizardByte/Sunshine) streams a virtual monitor as HEVC. On the tablet,
`tabd`, one native C daemon, takes the stream from the network to the hardware decoder to a display plane with zero
copies, then sends multi-touch back to Windows. The tablet boots straight into tabd: Android's display and app stack
never starts. Windows sees an ordinary extra monitor, with touch.

It also does:
- "glass states" like a real monitor: connecting, no signal, then the backlight off; the pairing PIN on the glass;
- brightness on the glass, like a monitor's own buttons: rest one finger in the bottom-right corner (about a 2 cm
  square) for half a second, then swipe two more fingers up or down; a full picture height is 100 points, and the
  level stays between 5 and 100 %. Windows gets a cancel for every finger, so nothing clicks, and fingers that land
  together (ordinary three-finger gestures) never trigger it. The level survives a reboot;
- links in order: Ethernet, Wi-Fi or both, the next one taking over while the first is down;
- portrait or landscape, fixed in its settings, or picked at boot from the accelerometer on the reference tablet (an
  RK3399 board, `rk3399-tve1030g`).

## Measured

On the reference tablet the stream reaches the glass about a frame after it arrives, over Ethernet or Wi-Fi,
with no late flips: [the numbers](devices/rk3399-tve1030g/README.md#numbers).

## Who it's for

You need:
- **A rootable Android tablet** whose bootloader you can unlock, or that already runs a userdebug build, plus a way
  to back up its partitions. Ethernet (USB or built in) is best; Wi-Fi works.
- **A Windows PC with a GPU encoder that Sunshine supports.** The PC kit is tested on NVIDIA only; AMD and Intel
  need two settings changed.
- **[Claude Code](https://claude.com/claude-code).** The adaptation is Claude's job. You do the physical things:
  cables, buttons, an unlock, typing passwords, and entering the pairing PIN.

It's a hobby project, not a product. Root and partition work can brick a device. The playbook checks first and stops
when something looks risky, but the risk stays yours.

## How to start

1. Clone this repo and open Claude Code in it.
2. Plug the tablet into the PC with USB debugging on (Settings → About → tap Build number 7 times → Developer options).
3. Say: **"turn my tablet into a monitor"**.

Claude follows [CLAUDE.md](CLAUDE.md). It runs a read-only go/no-go check on your tablet before it changes anything.
If your board is already in [`devices/`](devices/), it starts from those notes. If not, it adapts the reference and
writes the notes for the next person.

By hand, on the reference tablet: [pc/README.md](pc/README.md) for the PC, then
[devices/rk3399-tve1030g](devices/rk3399-tve1030g/README.md) for the tablet.

## Help wanted

New boards above all, and the list in [CONTRIBUTING.md](CONTRIBUTING.md).

## License

GPL-3.0 ([LICENSE](LICENSE)), because tabd links moonlight-common-c, which is GPL-3.0. The vendored libraries keep
their own licenses: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). This project isn't affiliated with Sunshine,
Moonlight, Rockchip, or any tablet maker.
