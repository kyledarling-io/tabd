# Shared by install.ps1, check.ps1 and display-guard.ps1 (dot-source it): the kit's pinned versions, paths and
# read-only probes. Nothing here changes the PC.

# Sunshine, pinned on purpose: newer versions changed the input backends. Same file and hash as winget's manifest.
$SunVer  = '2025.924.154138'
$SunUrl  = "https://github.com/LizardByte/Sunshine/releases/download/v$SunVer/Sunshine-Windows-AMD64-installer.exe"
$SunSha  = '51B62BB830FD2A4DD4AEB1CAF418876B0A2C1EBF38D33D8A4294521F61516C13'
$SunDir  = 'C:\Program Files\Sunshine'
$SunExe  = "$SunDir\sunshine.exe"
$SunConf = "$SunDir\config\sunshine.conf"
$SunApps = "$SunDir\config\apps.json"
$SunLog  = "$SunDir\config\sunshine.log"
# VDD by MTT (github.com/VirtualDrivers/Virtual-Display-Driver), release 24.12.24 = driver 11.30.4.434, installed with
# NefCon v1.14.0 the way the project's own "Community Scripts/silent-install.ps1" does it.
$VddVer  = '11.30.4.434'
$VddUrl  = 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/24.12.24/Signed-Driver-v24.12.24-x64.zip'
$VddSha  = 'F93E7CE3D640C83C419B1B72D2C83B2D8E34D83E8B7BA11F3328A40D2DA82FA4'
$NefUrl  = 'https://github.com/nefarius/nefcon/releases/download/v1.14.0/nefcon_v1.14.0.zip'
$NefSha  = 'A15557DA24A9EFCA203158DE3B43B0EAF982DB231F0194031F1ED428BC13E669'
$VddXml  = 'C:\VirtualDisplayDriver\vdd_settings.xml'   # the driver's default settings path
# MultiMonitorTool takes the VDD's screen off the desktop after its install or restart. Fetched only then, unpinned
# (NirSoft serves only the latest): install.ps1 records its hash and says when it changes.
$MmtUrl  = 'https://www.nirsoft.net/utils/multimonitortool-x64.zip'
$State   = Join-Path $env:ProgramData 'tabd'   # the kit's logs and records
# Your GPU, as Win32_VideoController names it, and its Sunshine HEVC encoder. Only NVIDIA is tested.
#   AMD: 'AMD|Radeon' and 'hevc_amf'; Intel: 'Intel' and 'hevc_qsv'.
$GpuMatch = 'NVIDIA'
$Encoder  = 'hevc_nvenc'
$Kit = $PSScriptRoot
# Sunshine's base port: install.ps1 -Port (default Sunshine's own, 47989); check.ps1 reads it from sunshine.conf.
# Sunshine uses base-5 .. base+21; the web UI is base+1. WSL's mirrored networking and Docker can reserve ranges that
# cover 47989: then move it (e.g. -Port 40989) and reserve it (-ReservePorts).
function Set-Port([int]$p) { $script:Port = $p; $script:PortFirst = $p - 5; $script:PortLast = $p + 21; $script:WebUi = "https://localhost:$($p + 1)" }
function Get-ConfPort {   # the base port in the installed sunshine.conf; nothing if it sets none
    $l = @(Get-Content -LiteralPath $SunConf -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\s*port\s*=\s*\d+\s*$' })
    if ($l) { [int]($l[-1] -replace '\D', '') }
}

# Text equality that ignores CRLF vs LF and trailing whitespace (git checks these files out with CRLF).
function Norm($text) { ("$text" -replace "`r", '') -replace '\s+$', '' }
function SameFile($a, $b) { (Test-Path -LiteralPath $a) -and ((Norm (Get-Content -Raw -LiteralPath $a)) -ceq (Norm (Get-Content -Raw -LiteralPath $b))) }

# The main monitor's mode, 'WxH@Hz', as the GPU's adapter ($GpuMatch) reports it.
function Main { Get-CimInstance Win32_VideoController | Where-Object Name -match $GpuMatch | Select-Object -First 1 | ForEach-Object { "$($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)@$($_.CurrentRefreshRate)" } }

function Get-SunVer { if (Test-Path $SunExe) { (Get-Item $SunExe).VersionInfo.ProductVersion } }

# The VDD device: instance id, driver version, PnP status. Nothing if it isn't installed.
function Get-Vdd {
    $d = Get-CimInstance Win32_PnPSignedDriver | Where-Object HardWareID -eq 'Root\MttVDD' | Select-Object -First 1
    if ($d) { [pscustomobject]@{ Id = $d.DeviceID; Version = $d.DriverVersion; Status = (Get-PnpDevice -InstanceId $d.DeviceID).Status } }
}

# The displays in the newest "Currently available display devices" list in Sunshine's log (Sunshine writes it a few
# seconds after it starts): device_id, friendly_name, edid, info.primary, ...
function Get-SunDisplays {
    if (-not (Test-Path $SunLog)) { return }
    $lines = @(Get-Content -LiteralPath $SunLog)
    for ($i = $lines.Count - 1; $i -ge 0; $i--) { if ($lines[$i] -match 'Currently available display devices:') { break } }
    if ($i -lt 0) { return }
    $json = for ($j = $i + 1; $j -lt $lines.Count -and $lines[$j] -notmatch '^\[\d{4}-'; $j++) { $lines[$j] }
    try { $devs = ($json -join "`n") | ConvertFrom-Json } catch { return }
    $devs
}
# The VDD's Sunshine device id: sunshine.conf's output_name.
function Get-VddId { (Get-SunDisplays | Where-Object friendly_name -eq 'VDD by MTT' | Select-Object -First 1).device_id }

# The sunshine.conf this PC should have: the user's other settings stay, the kit's keys come from pc\sunshine.conf
# with this PC's base port and VDD id. An older kit's global_prep_cmd (vdd-center.ps1 anywhere but %ProgramData%\tabd,
# the opt-in place pc\extras\vdd-center.ps1 names) goes; any other one stays.
$Owned = '^\s*(port|output_name|dd_configuration_option|dd_resolution_option|dd_manual_resolution|dd_refresh_rate_option|dd_manual_refresh_rate)\s*='
function Old-Center($line) { $line -match '^\s*global_prep_cmd\s*=.*vdd-center' -and $line -notmatch 'ProgramData\\+tabd\\+vdd-center' }
function Want-Conf($id) {
    $keep = @(if (Test-Path $SunConf) { Get-Content -LiteralPath $SunConf | Where-Object { $_.Trim() -and $_ -notmatch $Owned -and -not (Old-Center $_) } })
    $ours = @(Get-Content "$Kit\sunshine.conf" | Where-Object { $_.Trim() } | ForEach-Object { ($_ -replace '^(\s*output_name\s*=\s*).*$', "`${1}$id").Replace('{PORT}', "$Port") })
    ($keep + $ours) -join "`r`n"
}

# Which of ipv4/ipv6 x tcp/udp lack a persistent (administered, '*') exclusion covering $PortFirst-$PortLast.
function Get-PortGaps {
    foreach ($ipv in 'ipv4', 'ipv6') { foreach ($p in 'tcp', 'udp') {
        $ok = netsh int $ipv show excludedportrange protocol=$p | Where-Object { $_ -match '^\s*(\d+)\s+(\d+)\s+\*' -and [int]$matches[1] -le $PortFirst -and [int]$matches[2] -ge $PortLast }
        if (-not $ok) { "$ipv/$p" }
    } }
}

# Processes holding a local port in $PortFirst-$PortLast (netsh can't reserve ports that are in use). TIME_WAIT leftovers
# (owned by pid 0) clear by themselves within minutes.
function Get-PortUsers {
    $ids = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object State -ne 'TimeWait') + @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue) |
        Where-Object { $_.LocalPort -ge $PortFirst -and $_.LocalPort -le $PortLast } | ForEach-Object OwningProcess | Sort-Object -Unique
    foreach ($id in $ids) { "$((Get-Process -Id $id -ErrorAction SilentlyContinue).ProcessName) (pid $id)" }
}

# Sunshine's installer adds inbound allow rules for sunshine.exe, one tcp and one udp.
function Test-Firewall {
    $r = @(Get-NetFirewallApplicationFilter -Program $SunExe -ErrorAction SilentlyContinue | Get-NetFirewallRule |
        Where-Object { $_.Direction -eq 'Inbound' -and $_.Action -eq 'Allow' -and $_.Enabled -eq 'True' })
    $p = @($r | Get-NetFirewallPortFilter | ForEach-Object Protocol)
    ($p -contains 'TCP') -and ($p -contains 'UDP')
}

# The web UI's answer to an unauthenticated request: 401 = a login is set, 307 = none yet (it sends you to /welcome),
# 000 = Sunshine isn't answering. Sends no credentials and reads nothing secret.
function Get-WebLogin($ui = $WebUi) { & curl.exe -k -s -o NUL -w '%{http_code}' "$ui/" }

