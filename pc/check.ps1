<#
Read-only check of the tablet monitor's PC side: PASS/FAIL per item, with a one-line fix for each FAIL. Exit 1 if any FAIL.
No elevation needed.
  powershell -ExecutionPolicy Bypass -File pc\check.ps1 [-TabletMac 02-..] [-WifiMac ..] [-TabletIp a.b.c.d] [-MainMode WxH@Hz]
Sunshine's base port is the one in its sunshine.conf (47989 if none).
The tablet has two links, each checked by its MAC: Ethernet (-TabletMac, fixed, the main link) and Wi-Fi (-WifiMac,
an optional backup); run.sh check on the tablet side prints the Ethernet one. Without -TabletMac the tablet
isn't checked. Without -MainMode the main monitor is checked against its current mode. Addresses come from
-TabletIp, the ARP table, and every A record of Android.local; ping only.
#>
param([string]$TabletIp, [string]$MainMode, [string]$TabletMac, [string]$WifiMac)
. "$PSScriptRoot\common.ps1"
$p = Get-ConfPort; Set-Port $(if ($p) { $p } else { 47989 })
if (-not $MainMode) { $MainMode = Main }
$script:fails = 0
function Item([bool]$ok, $what, $fix) { if ($ok) { Write-Host "PASS $what" } else { Write-Host "FAIL $what`n       fix: $fix"; $script:fails++ } }

$v = Get-SunVer
Item ($v -eq $SunVer) "Sunshine $SunVer installed (found: $(if ($v) { $v } else { 'none' }))" 'run install.ps1 (if another version is installed, uninstall it first: Settings > Apps)'
if (Get-Command winget -ErrorAction SilentlyContinue) {
    Item ((winget pin list --disable-interactivity | Out-String) -match 'LizardByte\.Sunshine') 'winget pin keeps Sunshine out of "winget upgrade --all"' 'winget pin add --id LizardByte.Sunshine --exact (or run install.ps1)'
}
$svc = Get-Service SunshineService -ErrorAction SilentlyContinue
Item ($svc.StartType -eq 'Automatic') "SunshineService starts automatically ($($svc.StartType))" 'elevated: Set-Service SunshineService -StartupType Automatic (or run install.ps1)'
Item ($svc.Status -eq 'Running') "SunshineService running ($($svc.Status))" 'elevated: Start-Service SunshineService'
$listen = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -in $Port, ($Port + 1) } | ForEach-Object LocalPort | Sort-Object -Unique)
Item ($listen.Count -eq 2) "Sunshine listening on $Port (stream) and $($Port + 1) (web UI)" "read $SunLog for port errors; if WSL or Docker took the ports: install.ps1 -Port $Port -ReservePorts"
Item (Test-Firewall) 'firewall lets Sunshine in (tcp + udp)' "elevated: & '$SunDir\scripts\add-firewall-rule.bat'"
$gaps = @(Get-PortGaps)
if ($gaps.Count -eq 4) { Write-Host "INFO ports $PortFirst-$PortLast not reserved (fine unless WSL's mirrored networking or Docker takes them: install.ps1 -Port $Port -ReservePorts)" }
else { Item (-not $gaps) "ports $PortFirst-$PortLast reserved, persistent (ipv4+ipv6, tcp+udp)$(if ($gaps) { "; missing: $($gaps -join ', ')" })" "install.ps1 -Port $Port -ReservePorts; if WSL or Docker holds the ports: wsl --shutdown, quit Docker Desktop, run it again" }
Item (Select-String -LiteralPath $SunLog -Pattern "Found HEVC encoder: $Encoder" -Quiet -ErrorAction SilentlyContinue) "Sunshine found the HEVC encoder ($Encoder)" "update the GPU driver (or set `$Encoder in common.ps1), restart Sunshine, read $SunLog"

$vdd = Get-Vdd
Item ($vdd -and $vdd.Status -eq 'OK') "VDD by MTT installed and OK$(if ($vdd) { " ($($vdd.Status))" })" 'run install.ps1; if installed but not OK: Device Manager > Display adapters > Virtual Display Driver'
Item ($vdd.Version -eq $VddVer) "VDD driver $VddVer (release 24.12.24; found: $($vdd.Version))" 'only 24.12.24 is tested here: uninstall the VDD (Device Manager), then run install.ps1'
Item (SameFile $VddXml "$Kit\vdd_settings.xml") "$VddXml matches pc\vdd_settings.xml" 'run install.ps1 (it restarts the VDD, guarded)'
Item (SameFile $SunApps "$Kit\sunshine-apps.json") 'apps.json is Desktop only' 'run install.ps1'
$id = Get-VddId
Item ([bool]$id) "Sunshine sees the VDD (output_name $id)" "restart Sunshine, then look for 'Currently available display devices' (friendly_name 'VDD by MTT') in $SunLog"
if ($id) {
    $have = @(Get-Content -LiteralPath $SunConf -ErrorAction SilentlyContinue)
    $miss = @((Want-Conf $id) -split "`r`n" | Where-Object { $have -cnotcontains $_ })
    Item (-not $miss) "sunshine.conf: port $Port, output_name = the VDD, ensure_active, resolution/refresh auto" "run install.ps1 (missing: $($miss -join ' | '))"
    Item (-not @($have | Where-Object { Old-Center $_ })) "sunshine.conf: no older kit's vdd-center.ps1 at stream start" 'run install.ps1 (it takes the line out; pc\extras\vdd-center.ps1 is the opt-in way to keep it)'
}
$code = Get-WebLogin
Item ($code -eq '401') "Sunshine web login is set (web UI answers $code)" "open $WebUi/ and set one there (Sunshine asks for it)"

$m = Main
Item ($m -eq $MainMode) "main monitor $m (expected $MainMode)" "Settings > System > Display: set $MainMode; if it changes when the tablet streams, see README > Troubleshooting"

# Two links, told apart only by MAC: Android.local can answer with either one's address.
if (-not $TabletMac) { Write-Host 'SKIP the tablet: pass -TabletMac (its Ethernet MAC, as run.sh check prints it) and -WifiMac' } else {
$seen = @{}
function Probe($ip) {
    if ($seen.ContainsKey($ip)) { return }
    $up = Test-Connection $ip -Count 2 -Quiet   # the ping also refreshes the ARP entry read next
    $seen[$ip] = [pscustomobject]@{ Ip = $ip; Up = $up; Mac = (Get-NetNeighbor -IPAddress $ip -ErrorAction SilentlyContinue | Select-Object -First 1).LinkLayerAddress }
}
function Answering($mac) { @($seen.Values | Where-Object { $_.Up -and $_.Mac -eq $mac }) }
if ($TabletIp) { Probe $TabletIp }
foreach ($m in @($TabletMac, $WifiMac) | Where-Object { $_ }) { Get-NetNeighbor -LinkLayerAddress $m -AddressFamily IPv4 -ErrorAction SilentlyContinue | ForEach-Object { Probe $_.IPAddress } }
if (-not (Answering $TabletMac) -or -not (Answering $WifiMac)) {
    Resolve-DnsName Android.local -Type A -ErrorAction SilentlyContinue | Where-Object Type -eq A | ForEach-Object { Probe $_.IPAddress }
}
$eth = Answering $TabletMac; $wifi = if ($WifiMac) { Answering $WifiMac }
Item ([bool]$eth) "tablet answers ping on Ethernet$(if ($eth) { " at $($eth.Ip -join ', ')" }) (MAC $TabletMac)" $(if ($wifi) { 'the tablet is on Wi-Fi only: check its Ethernet cable and the switch port' } else { 'check the tablet is on and its Ethernet cable, or pass -TabletIp <address>; arp -a' })
if ($wifi) { Item $true "tablet's Wi-Fi backup answers ping at $($wifi.Ip -join ', ') (MAC $WifiMac)" }
elseif ($WifiMac) { Write-Host "INFO tablet's Wi-Fi backup doesn't answer ping (MAC $WifiMac); it's optional, Ethernet is the main link" }
if ($TabletIp) {
    $t = $seen[$TabletIp]
    Item ($t.Up -and ($t.Mac -in $TabletMac, $WifiMac)) "-TabletIp $TabletIp is the tablet (answers ping: $($t.Up); MAC $($t.Mac))" "pass one of the tablet's addresses (arp -a: Ethernet MAC $TabletMac, Wi-Fi MAC $WifiMac), or leave -TabletIp out"
}
}

Write-Host $(if ($fails) { "`n$fails FAIL" } else { "`nAll PASS" })
exit [int]($fails -gt 0)
