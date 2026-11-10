# tabd: the PC side

Sunshine streams a virtual monitor ("VDD by MTT") with HEVC from the GPU's encoder (NVENC on the reference PC) to the
tablet. That monitor exists only while the tablet streams: Sunshine switches it on at the tablet's shape (its panel,
landscape or portrait). Arrange it once in Settings > System > Display (for example below the main monitor);
Windows keeps its position. The tablet side is in its device folder (the reference: `devices/rk3399-tve1030g/README.md`).

| File | What it is |
|---|---|
| `install.ps1` | Takes a fresh Windows 11 PC to a working PC side (NVIDIA tested; another GPU: set `$GpuMatch` and `$Encoder` in `common.ps1`). Idempotent; `-WhatIf` (or `-DryRun`) prints the plan only. |
| `check.ps1` | Read-only PASS/FAIL of everything below, with a one-line fix per FAIL. No elevation needed. |
| `common.ps1` | Pinned versions, URLs, hashes and read-only probes shared by the scripts. |
| `sunshine.conf` | The kit's Sunshine keys. `port` and `output_name` are placeholders: install.ps1 fills in the base port and this PC's VDD id. |
| `sunshine-apps.json`, `vdd_settings.xml` | Installed as they are. |
| `display-guard.ps1` | Watches the main monitor; reverts if its resolution changes. |
| `extras/vdd-center.ps1` | Opt-in, not installed: re-centers the tablet's screen at each stream start, for a PC where Windows doesn't keep its position. Its header says how to add it. |

## Fresh Windows: order of operations

1. **Windows 11 and your GPU's driver.** Set the main monitor to its native mode in Settings > System > Display.
2. **Get this repo:** `winget install -e --id Git.Git`, then `git clone <this repo's URL>` (any folder).
3. **Ports.** Sunshine's default base port is 47989 (web UI 47990). If you use WSL's mirrored networking or Docker,
   they can reserve that range: then move Sunshine (`-Port 40989`, say) and reserve its range (`-ReservePorts`), and
   run the installer before WSL or Docker start (or `wsl --shutdown` and quit Docker Desktop first). A moved port goes
   into the tablet's `tabd.conf` too: `pc = <PC address>:40989` or `pc = :40989`.
4. Dry run (no elevation): `powershell -ExecutionPolicy Bypass -File pc\install.ps1 -WhatIf [-Port N] [-ReservePorts]`.
   Without `-Port` it keeps the port sunshine.conf already has (47989 on a fresh install); its first line says which.
5. Real run, from an **elevated** PowerShell in the repo: `powershell -ExecutionPolicy Bypass -File pc\install.ps1 [-Port N] [-ReservePorts]`
6. **Your hands:**
   - If no web login is set, it opens Sunshine's web UI in the browser: type a new login there.
   - Power the tablet on; it shows a PIN on its glass. On `https://localhost:<port+1>/pin`, sign in, then enter the PIN
     and a name. The picture appears within seconds.
   - Arrange the tablet's screen once in Settings > System > Display while it streams.
7. **Verify:** `powershell -ExecutionPolicy Bypass -File pc\check.ps1` with `-TabletMac <the tablet's Ethernet MAC>`
   (and `-WifiMac`) shows every line PASS. Then reboot once: the tablet should show the Windows login screen without
   anyone signing in.

## What install.ps1 does

Each step checks first and acts only if needed. On a set-up PC it prints `OK` lines and "Nothing to change".

1. With `-ReservePorts`: reserves the base port's range (base-5 to base+21), TCP and UDP, IPv4 and IPv6, persistently
   (`netsh int ... add excludedportrange ... store=persistent`). If another program holds them, it stops and says how
   to free them; it never forces anything. A reservation already there is left alone.
2. Installs **Sunshine 2025.924.154138**, silently. This version is pinned: newer ones changed the input backends.
   It also adds a winget pin so `winget upgrade --all` leaves Sunshine alone. If any other Sunshine version is
   installed, it stops.
3. Checks Sunshine's inbound firewall rules (its installer adds them).
4. Writes `C:\VirtualDisplayDriver\vdd_settings.xml`.
   - VDD missing: installs VDD driver 11.30.4.434 (release 24.12.24) with NefCon, the way the VDD project's own
     silent installer does. It trusts the driver's signer (SignPath Foundation) in TrustedPublisher. Then it takes the
     new screen off the desktop until a stream needs it (with MultiMonitorTool, fetched for that).
   - VDD already installed and the XML changed: restarts the VDD, and puts the old XML back if the VDD doesn't come
     back OK.
5. Writes `apps.json`: Desktop only. tabd streams nothing else, and another app running leaves the tablet dark. The
   old one stays next to it as `apps.json.bak-<time>`, in `C:\Program Files\Sunshine\config`: other Moonlight clients'
   apps (a game launcher, say) are in it, to copy back if you want them.
6. Writes `sunshine.conf`:
   - `port` = the base port (`-Port`, else the one already there, else 47989)
   - `output_name` = the VDD's id, found in Sunshine's log (it restarts Sunshine once if the log doesn't list it)
   - `dd_configuration_option = ensure_active`
   - `dd_resolution_option = auto` and `dd_refresh_rate_option = auto`

   Your other Sunshine settings are kept; an older kit's `global_prep_cmd` (vdd-center.ps1) goes. Every file it
   replaces is kept as `.bak-<time>`.
   One that runs `%ProgramData%\tabd\vdd-center.ps1` stays, as the opt-in (earlier versions of this kit put it there
   too): to drop it, delete that `global_prep_cmd` line from `sunshine.conf` and restart Sunshine.
7. Sets SunshineService to start **Automatic**, so the tablet works after a reboot, at the login screen too.
8. (Re)starts Sunshine if anything changed.
9. If no web login is set, opens Sunshine's web UI, which asks you for one.
10. Runs `check.ps1`.

The guard: installing or restarting the VDD (the steps that change the display topology) runs under
`display-guard.ps1`. It reverts (it stops Sunshine, then runs `DisplaySwitch /internal`) if the main monitor's
resolution changes, and the install stops there. Logs go to `%ProgramData%\tabd\install.log` and
`display-guard.log`.

Downloads, each into its own temp folder (pinned ones are hash-checked):

| What | URL | SHA-256 |
|---|---|---|
| Sunshine 2025.924.154138 | `github.com/LizardByte/Sunshine/releases/download/v2025.924.154138/Sunshine-Windows-AMD64-installer.exe` | `51b62bb8…16c13` (same as winget) |
| VDD 24.12.24 | `github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/24.12.24/Signed-Driver-v24.12.24-x64.zip` | `f93e7ce3…82fa4` |
| NefCon 1.14.0 | `github.com/nefarius/nefcon/releases/download/v1.14.0/nefcon_v1.14.0.zip` | `a15557da…c13e669` |
| MultiMonitorTool x64, only when the VDD is installed or restarted | `www.nirsoft.net/utils/multimonitortool-x64.zip` | not pinned (NirSoft serves only the latest): the install records its hash in `%ProgramData%\tabd` and says when it changes |

Full hashes are in `common.ps1`. winget's id for Sunshine is `LizardByte.Sunshine`. For the VDD it is
`VirtualDrivers.Virtual-Display-Driver`, but that one is only the newer 25.x installer, which is untested here.

## Troubleshooting

- **Sunshine can't bind its ports, or the tablet can't connect after WSL or Docker was installed:** move and reserve
  the port as in step 3. Check with `netsh int ipv4 show excludedportrange protocol=tcp`. Windows can't add a
  reservation while ports are in use, so run `wsl --shutdown` and quit Docker Desktop first.
- **A Sunshine setting seems ignored.** The display-device option values are `auto`, `manual` and `disabled`. Any
  other value (`automatic`, say) is silently ignored, and the option stays off.
- **The main monitor's mode changes when the tablet streams.** First get it back: stop SunshineService (Sunshine puts
  the topology back), then `DisplaySwitch /internal`, or set the mode in Settings > Display. If it happens at every
  stream, Windows has stale stored layouts for monitor sets that include the VDD (monitor id MTT1337); a fresh Windows
  has none. Elevated:
  1. List them: `reg query HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Configuration | findstr MTT1337`, and
     the same for `...\Connectivity`.
  2. Back up both keys: `reg export HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers\Configuration %ProgramData%\tabd\display-Configuration.reg`,
     and the same for `...\Connectivity`.
  3. Delete only the subkeys of those two keys whose names contain `MTT1337`.
  4. Run `pc\display-guard.ps1 -Seconds 900` for the next stream.

  Undo: `reg import` the two files.
- **No picture, or Sunshine streams the wrong screen.** `output_name` must be the VDD's id, which changes when Windows
  or the VDD is reinstalled. Re-run install.ps1, which finds it again. By hand: in Sunshine's log, after the line
  "Currently available display devices", the entry with `friendly_name` "VDD by MTT" has it as `device_id`.
- **Don't add apps or prep commands that disable displays.** tabd streams Desktop only.
- **Anything that switches the VDD on** (restarting it, changing its modes, testing layouts) runs with the guard
  first, from an elevated PowerShell: `powershell -ExecutionPolicy Bypass -File pc\display-guard.ps1 -Seconds 900`.
- **A tablet with two links** (Ethernet and Wi-Fi) has a MAC on each, and `Android.local` may resolve to either, so
  `check.ps1` tells them apart by MAC (`-TabletMac`, `-WifiMac`; `run.sh check` prints the Ethernet one). "on Wi-Fi
  only" means the cable or the switch port is out.
- **The tablet's screen moves between streams.** Arrange it once in Settings > Display while it streams. If Windows
  still doesn't keep it, use `extras/vdd-center.ps1`.
- **Sunshine got upgraded.** Uninstall it, then re-run install.ps1. You may have to pair again.
