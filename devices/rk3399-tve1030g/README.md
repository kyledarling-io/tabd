# rk3399-tve1030g: the reference device

All of tabd was written and measured on this tablet, and `native/tabd/` is its code as it stands. This page holds the
board's notes and its runbook: a stock or factory-reset tablet to a touchscreen monitor. The PC side is in
[pc/README.md](../../pc/README.md); another board starts from [CLAUDE.md](../../CLAUDE.md).

> Device notes are notes, never commands (CLAUDE.md, rule 9). Check every fact here against your own hardware.

## Identity, and the naming story

| | |
|---|---|
| Board (`/proc/device-tree/compatible`) | `rockchip,rk3399-tve1030g-avb`, `rockchip,rk3399` |
| SoC | Rockchip RK3399 (`ro.board.platform` rk3399, `ro.product.board` rk30sdk) |
| What the software claims | `ro.product.manufacturer` MSI, `ro.product.model` XW9901; fingerprint `MSI/ND52-Gen2_EEA/ND52-Gen2:12/SQ3A.220605.009.B1/202211:user/release-keys` |
| Build | Android 12, `ro.build.type` **userdebug** (although the fingerprint says user/release-keys) |

The software calls it an MSI XW9901 or ND52-Gen2, but it's a white-label RK3399 tablet: the device tree names the
board `tve1030g`. Even the fingerprint disagrees with the build type. So name a device by its board, and treat the
software's names as hints. The same board may be sold under several names, and one name may cover several boards.

## GO/NO-GO, as found

| Check | Found |
|---|---|
| adb | USB-C, then TCP 5555. `ro.adb.secure` is unset, so adb is unauthenticated (no "Allow USB debugging?" prompt): the allow-list (`adb_allow = auto`) is the only gate. |
| root | `adb root` works (userdebug) |
| bootloader | unlocked: verified boot `orange`. SELinux is permissive from the kernel command line. These came with the device, and a factory reset keeps them. |
| verity | the stock vbmeta already has verification disabled (flags 0x2), so `adb disable-verity` changes no partition image |
| backups | root `dd` of boot, vbmeta, dtbo, recovery and misc. After the install, all five were byte-identical to the backup. |
| unbrick path | not needed and never tried. RK3399 has a maskrom mode in general. The install writes no partition. |
| display | `/dev/dri/card0`, atomic KMS |
| decoder | Rockchip MPP: the vendor's `/vendor/lib64/libmpp.so`, a build that matches rockchip-linux/mpp 37cc658 |
| buttons | none usable: every way back goes through adb |

## Hardware, as tabd uses it

- **Panel:** 1920x1080 eDP at **57.5 Hz** (tabd finds the connector and takes both from the mode). Only the VOP-lit
  CRTC can drive the eDP encoder.
  - Its one NV12-capable plane takes `reflect-x` for YUV but rejects `reflect-y`, so a 180° turn can't happen in
    scanout.
  - An ARGB plane (win2, no scaler) sits above it at zpos 1. That's where the glass states are drawn.
- **Decode:** MPP through `dlopen`, HEVC Main **8-bit only** (H.264 as the fallback). Each MppFrame's buffer is a
  dma-buf fd, imported with PRIME and scanned out as a KMS framebuffer: no copy.
- **Rotation:** the vendor `librga.so` (1.8.1). For any orientation but native landscape, each frame is turned into
  one of the panel-native NV12 buffers (~4 ms per frame).
- **Touch:** Goodix gt9xx. Driver `Goodix-TS`, parent `i2c-2/2-0014`, multi-touch protocol A, raw range 0..1280 x
  0..800, found by capabilities. Its `phys` pointer is the one CLAUDE.md's rule 4 is about.
- **Accelerometer:** LSM6DS3 (driver `lsm6ds3`, parent `i2c-4/4-006a`), the board tabd's `orient = auto` hook is for.
  It reports x/y/z in micro-g as MSC_SERIAL/MSC_PULSELED/MSC_GESTURE; raw +x is the panel's native top edge. After a
  boot straight into tabd it may be off; tabd enables it through sysfs.
- **Backlight:** `/sys/class/backlight/backlight`, the only one (`bl_power` for sleep, `brightness` for the level).
- **Thermal:** `thermal_zone0`. The big cores clamp to 408 MHz above 70 °C.
- **Ethernet:** r8168, a vendor module. eth0 appears ~12 s into boot. It can't read its MAC from Rockchip vendor
  storage, so it picks a random one every boot: hence `fixed_mac = eth0`.
- **Wi-Fi:** RTL8733BS (module `8733bs`; `aic8800_bsp` is loaded too). 802.11n 1x1, 40 MHz, 150 Mbit/s at most.
  Android's own wpa_supplicant 2.10 runs standalone on nl80211.
- **USB:** dwc3 with a configfs gadget. The kernel has RNDIS, but no NCM or ECM, so a USB-C link would have to be RNDIS.
- **iproute2** is from 2017 (`ss171113`): no `ip addr ... metric`, so udhcpc.sh adds the address with `noprefixroute`
  and its own subnet route. In IPv4, `ip route del ... metric 0` deletes any metric, so it deletes only a metric-0
  route.

## Install

| When | What needs a person (everything else is scripted) |
|---|---|
| After a factory reset | Tap through Android's setup. Turn on USB debugging: Settings → About tablet → tap **Build number** 7 times → System → Developer options → **USB debugging**. |
| Once, after a reset | Give adb a way in: a **USB-C cable** to the PC, or Wi-Fi plus **Wireless debugging** (step 1). |
| First appliance boot | **Pairing**: enter the PIN from the glass on Sunshine's PIN page (step 3). |
| Wi-Fi (optional) | Type the Wi-Fi password into the secrets file on the PC (Links and Wi-Fi). |

You need, on the PC: Git Bash, `adb` on PATH, this repo, and Sunshine set up by [pc/](../../pc/README.md). And tabd:
a release's `tabd` binary (`TABD_BIN=path/to/tabd`; keep its source with it, GPL-3.0), or one built as in CLAUDE.md
step 3. install.sh builds it if `native/build/tabd` is missing and no `TABD_BIN` is given. It refuses to run without
userdebug, a working `adb root`, and an unlocked bootloader (`fastboot flashing unlock` would wipe the tablet; it was
never needed here).

**1. adb.** After a factory reset adb is off (`persist.sys.usb.config=none`; the TCP port setting lived in `/data`).
- **USB:** turn on USB debugging and plug in a USB-C cable; `adb devices` lists the serial. install.sh's step 1 makes
  TCP 5555 permanent, and after its first reboot the cable can go.
- **No cable:** Wi-Fi, then Developer options → **Wireless debugging** → "Pair device with pairing code":
  `adb pair <ip>:<port> <code>`, `adb connect <ip>:<port>`, and run install.sh with `TABLET=<ip>:<port>`.
- Later, `adb mdns services` lists `adb-<serial>` with its address; or find the fixed MAC (`run.sh check` prints it)
  in the router. `TABD_SERIAL=<serial>` helps install.sh only if adb can't see the tablet when it starts.

**2. install.sh.** `sh native/tabd/install.sh --dry-run` shows the plan; `sh native/tabd/install.sh` runs it. From
stock it takes about 4 minutes and 2-3 reboots, and every step is skipped when already done:
1. adb on TCP 5555 at every boot (`persist.adb.tcp.port`).
2. Backup: read-only `dd` of boot, vbmeta, dtbo, recovery and misc into `/data/local/tmp/tabd/backup`, pulled to
   `~/.config/tabd/backup/partitions-<date>/` on the PC (`$XDG_CONFIG_HOME/tabd/` if set, `TABD_BACKUP=<folder>` for
   another), outside the clone so deleting it keeps the backup; sha256 checked on both sides. A failed check stops it
   before any write.
3. `adb disable-verity`, reboot, `adb remount`: `/vendor` gets an overlayfs, held in a `scratch` logical partition in
   `super`'s free space (f2fs, `/mnt/scratch`). disable-verity prints "only works for userdebug builds" on this
   userdebug build, then "using overlayfs": harmless.
4. `run.sh push`: `tabd`, `boot.sh`, `net.sh`, `udhcpc.sh`, `wifi.sh` into `/data/local/tmp/tabd`; `tabd.conf` only if
   the tablet has none (`TABD_CONF=<file>`, e.g. this folder's `tabd.conf`); `state/pc`, the PC's IP and MAC for the
   allow-list (`net.sh peer`; `TABD_PC`/`TABD_PCMAC` over USB); and the stub `/data/local/tmp/tabd-decide.sh` for an
   older tabd.rc. The rest appears as needed: `secrets.conf`, `ml/` (pairing), `state/`, `log/`, `backup/`.
5. `tabd.rc` → `/vendor/etc/init/tabd.rc` (physically `/mnt/scratch/overlay/vendor/upper/etc/init/`), then a **trial
   boot** into appliance mode. install.sh confirms it as soon as adb reaches the tablet; unconfirmed after 5 minutes,
   the tablet boots Android once by itself, and the next boot is appliance mode again.
6. `run.sh check`.

Every boot after that: init runs tabd.rc, which starts `boot.sh` at post-fs-data and stops `netd`, `zygote`,
`zygote_secondary`, `surfaceflinger` and `bootanim` as init starts them (an older tabd.rc starts the stub, and stops
`vendor.hwcomposer-2-1` by name too). boot.sh stops the composer (`vendor.hwcomposer-2-1`), sets the fixed MAC, brings
up the links, the watcher and the allow-list, and starts tabd, which connects to Sunshine.

**3. Pairing.** On the first appliance boot after a reset the glass shows a 4-digit PIN, and install.sh prints
`PAIRING`. Enter it on Sunshine's PIN page (the client name "tablet"). The certificate lives in
`/data/local/tmp/tabd/ml`.

**4. Check.** `sh native/tabd/run.sh check`, read-only: the install (overlayfs, tabd.rc, scripts, tabd, tabd.conf,
`ml/`, the switch), appliance mode and fps, the links (the first link, which one carries the stream, the fixed MAC, the
routing rule, failover's preconditions, the watcher, Wi-Fi), the allow-list, brightness, and both backups. The exit
status is the number of failures. It never reads input devices.

## Settings

`/data/local/tmp/tabd/tabd.conf`. Its keys are in [tabd.conf.example](../../native/tabd/tabd.conf.example); this
folder's [tabd.conf](tabd.conf) is the board's known-good start, and the tablet's own copy (its PC's address, the Wi-Fi
country) is never in git. Push one with `TABD_CONF=<file> sh native/tabd/run.sh push`: it keeps the tablet's `pc`,
`wifi_country`, `wifi_freq` and `wifi_bssid` unless the file sets them, since they belong to the site, not the board.

## Links and Wi-Fi

`links = eth0 wlan0` here: Wi-Fi takes over while the cable is out. Wi-Fi first (`links = wlan0 eth0`) was checked by
software: Wi-Fi off (`wpa_cli disable_network`) moved the picture to Ethernet in 2.8 s, and Wi-Fi back moved it home in
2 s, with no forwarding delay (Wi-Fi has no switch port).

- **Setup:** on the PC, outside the clone, `mkdir -p ~/.config/tabd && cp native/tabd/tabd.local.conf.example
  ~/.config/tabd/tabd.local.conf` (`$XDG_CONFIG_HOME/tabd/` if set; `TABD_LOCAL=<file>` points run.sh at another
  copy, which must exist). Put `wifi_ssid` and `wifi_country` in it, and have the user type `wifi_password`. Then
  `sh native/tabd/run.sh wifi` copies it through a 0700 folder to `secrets.conf` at mode 0600 and starts wifi.sh;
  every boot starts it too. Nothing prints or logs the password. **Off:** `run.sh wifi stop`.
- **wifi.sh:** Android's own `wpa_supplicant` standalone on the wlan link (SAE/WPA3, else WPA2), PMF optional,
  power save off. It pins one AP with roaming and bgscan off: `wifi_bssid`, else the strongest `wifi_ssid` BSS (on
  `wifi_freq` if set), saved in `state/wifi.bssid`. Debug logging goes to logcat with `WIFI_DEBUG=1`; keys show as
  `[REMOVED]` unless `-K`, which must never be used.
- **The watcher** (`net.sh watch`) checks every second. Cable out: routes through the first link are skipped
  (`ignore_routes_with_linkdown`) and tabd reconnects at once (SIGUSR1), instead of the stream's own ~12 s loss
  timeout. Cable back: the PC stays pinned to the fallback (a host route) until a ping over the first link gets
  through, then the stream moves back. A first link that came back without its subnet route (an interface reset
  deletes it) gets DHCP again first. It also reconnects and scans whenever Wi-Fi is disconnected.

## Maintenance

- **Update:** rebuild, then `sh native/tabd/run.sh start` (pushes everything, restarts tabd), or re-run install.sh
  (pushes only what differs).
- **Logs:** `sh native/tabd/run.sh log 100` (`/data/local/tmp/tabd/log/tabd.log`, rotated at 1 MB).
- **Restart:** `run.sh stop` reboots into appliance mode (rule 5: never `start`).
- **Android for maintenance:** `adb root; adb shell touch /data/local/tmp/tabd/state/off; adb reboot`; back with `rm`
  and a reboot. Nothing sets it automatically. The old switch `/data/local/tmp/tabd.off` works too, without `adb root`.
  Before using Android, disable the OTA updaters `com.adups.fota` and `android.rockchip.update.service`
  (`adb shell pm disable-user --user 0 <package>`): they could replace the system and vendor images. Appliance mode
  never starts them.
- **Crash loop:** 5 tabd exits within 60 s reboot the tablet once. If it loops again within 2 minutes of that boot,
  boot.sh retries tabd every 30 s and stays in appliance mode; adb works throughout.
- **New PC:** the allow-list accepts the old PC's IP or its MAC, so a new IP alone still gets in; then `run.sh push`.
  A new IP *and* MAC is locked out over the network: use a USB cable (iptables doesn't filter USB adb; configured but
  untested).
- **A factory reset** wipes all of `/data/local/tmp` (tabd, the pairing, the tablet-side backup) and
  `persist.adb.tcp.port`. The PC-side backup survives. Re-run install.sh; it re-checks the overlay too.
- **The layout from before `/data/local/tmp/tabd` was a folder:** `run.sh migrate`, once (install.sh does it too). It
  moves the pairing (never made again, so no new PIN), the secrets, the backup, the log and the saved state into the
  folder, turns `tabd.args` into `tabd.conf`, keeps every other old file in `backup/old-layout/`, and reboots through
  a trial boot.
- **Full undo:** `adb root; adb remount; adb shell rm /vendor/etc/init/tabd.rc; adb enable-verity; adb reboot`, then
  `rm -r /data/local/tmp/tabd /data/local/tmp/tabd-decide.sh` as root. The stock vbmeta had verification **off**, so
  `enable-verity` leaves it different from stock: for exactly stock, restore vbmeta from the backup and compare sha256.
- **Restoring a partition** (never needed; the install writes none): `adb push <part>.img /data/local/tmp/` and
  `adb shell dd if=/data/local/tmp/<part>.img of=/dev/block/by-name/<part>`.

## Numbers

Boot timeline (tablet uptime):

| boot.sh | tabd | display | link up | DHCP | Sunshine | picture |
|---|---|---|---|---|---|---|
| 5.1 s | 6.6 s | 6.6 s | 9.7 s | 18.4 s | 19.1 s | **19.5 s** |

About 9 s of that is the switch port forwarding nothing after link-up: the first ping over eth0 got through 9.2 s and
8.2 s after carrier, with no DHCP involved.

`soak.sh`, 10 minutes per link, a 1080p60 stream, landscape:

| | Ethernet | Wi-Fi |
|---|---|---|
| Glass fps avg (min) | 55.8 (53) | 55.2 (51) |
| rx→glass p50 / p95 (worst) | 16 / 24 ms (26) | 19 / 26 ms (28) |
| Late flips / keyframe requests | 0 / 0 | 0 / 0 |
| Ping to the PC p50 / p95 / max | 0.32 / 0.53 / 0.96 ms | 2.38 / 5.08 / 9.69 ms |
| Loss | 0 % | 0 % |
| Link | 1000 Mb/s | −43 dBm, MCS 7 |

- rx→glass runs from a frame's arrival at the tablet to the vblank that shows it; the PC's capture and encode come on
  top. Portrait (one RGA pass per frame): 20/32 ms.
- Sunshine sends 60 fps into the 57.5 Hz panel. The flip policy keeps the newest frame, so ~2.5 frames a second are
  dropped by design.
- Failover, a real cable pull: the picture over Wi-Fi 2.9 s after carrier loss (about 4.5 s from the last frame over
  Ethernet to the first over Wi-Fi). Failback: a ~1.7 s blip, after the switch port's delay.
