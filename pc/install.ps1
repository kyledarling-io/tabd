<#
Tablet monitor, PC side: takes a fresh Windows 11 PC (NVIDIA tested; other GPUs: $GpuMatch and $Encoder in common.ps1) to a working Sunshine + virtual display setup.
Idempotent: each step checks first and acts only if needed, so on a configured PC it changes nothing and says so.
  elevated:  powershell -ExecutionPolicy Bypass -File pc\install.ps1 [-Port 47989] [-ReservePorts]
  dry run:   powershell -ExecutionPolicy Bypass -File pc\install.ps1 -WhatIf [...]   (prints the plan, changes nothing, no elevation)
  -Port N        Sunshine's base port; the web UI is N+1. Default: the one sunshine.conf already has (a configured PC
                 keeps its port), else 47989, Sunshine's own. The tablet's tabd.conf needs pc = <PC>:N or :N unless N
                 is 47989.
  -ReservePorts  reserve N-5..N+21 persistently first (netsh excludedportrange): only if WSL's mirrored networking or
                 Docker takes that range (they can reserve the default one).
A real run logs to %ProgramData%\tabd\install.log and ends with check.ps1.
The main monitor is protected where the topology changes: installing or restarting the VDD runs under display-guard.ps1,
which reverts if the main monitor's resolution changes; the install then stops there.
Not automated, by design: the Sunshine web login (you type it in a window this opens) and pairing (PIN on the tablet).
#>
# A plain switch, not ShouldProcess: $WhatIfPreference would leak into read-only CIM probes and make them return nothing.
param([Alias('DryRun')][switch]$WhatIf, [int]$Port, [switch]$ReservePorts)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
# the base port: -Port, else the one this PC's Sunshine already uses (a moved port must stay moved), else Sunshine's own
$portWhy = if ($PSBoundParameters.ContainsKey('Port')) { '-Port' } elseif ($Port = Get-ConfPort) { "kept from sunshine.conf" } else { $Port = 47989; "Sunshine's default" }
Set-Port $Port
$dry = [bool]$WhatIf
$logFile = "$State\install.log"; $guardLog = "$State\display-guard.log"   # $State: common.ps1
$tmp = Join-Path $env:TEMP "tablet-kit-$PID"
$done = New-Object Collections.Generic.List[string]; $plan = New-Object Collections.Generic.List[string]
$restartSun = $false; $sunNew = $false; $oldXml = $null

function Say($m) { Write-Host $m; if (-not $dry) { try { [IO.File]::AppendAllText($logFile, ("{0:yyyy-MM-dd HH:mm:ss} {1}`r`n" -f (Get-Date), $m)) } catch {} } }
function Step($what, [scriptblock]$act) {   # dry run: say what would happen; real run: do it
    if ($dry) { Say "PLAN $what"; $plan.Add($what); return }
    Say "DO   $what"; & $act; $done.Add($what)
}
function Guarded([scriptblock]$act) {   # run something that can switch the VDD on, then watch the main monitor for 20 s
    $want = (Main) -replace '@.*'
    & $act
    $global:LASTEXITCODE = 0
    & "$Kit\display-guard.ps1" -Seconds 20 -Expect $want -Log $guardLog
    if ($LASTEXITCODE -eq 3) { throw "the main monitor's resolution changed (it should be $want); the guard reverted it (now $(Main)). Stopped here: see $guardLog and README > Troubleshooting." }
}
function Fetch($url, $sha) {   # download into its own new temp folder, check the pinned hash (if any), return the file
    $d = Join-Path $tmp ([guid]::NewGuid()); New-Item -ItemType Directory -Force $d | Out-Null
    $f = Join-Path $d (Split-Path $url -Leaf)
    & curl.exe -fsSL -o $f $url
    if ($LASTEXITCODE) { throw "download failed (curl $LASTEXITCODE): $url" }
    if ($sha -and (Get-FileHash $f -Algorithm SHA256).Hash -ne $sha) { throw "hash mismatch, not using it: $url" }
    Say "     downloaded $url$(if ($sha) { ' (sha256 OK)' })"
    $f
}
function Browse($url) {   # explorer.exe hands the URL to your own (unelevated) shell; Start-Process from here would start the browser elevated
    explorer.exe $url; Say "     opened $url in your browser (if no tab appeared, open it yourself)"
}
function Unzip($zip) { $d = Join-Path (Split-Path $zip) 'x'; Expand-Archive $zip $d -Force; $d }
function Backup($f) { if (Test-Path -LiteralPath $f) { $b = "$f.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"; Copy-Item -LiteralPath $f $b; $b } }
function Get-Mmt {   # MultiMonitorTool, fetched once per run; its hash is recorded in $State, and a change is said
    if ($script:mmt) { return $script:mmt }
    $zip = Fetch $MmtUrl $null
    $h = (Get-FileHash $zip -Algorithm SHA256).Hash; $rec = "$State\multimonitortool.sha256"
    $old = Get-Content -LiteralPath $rec -ErrorAction SilentlyContinue
    if ($old -and $old -ne $h) { Say "     note: MultiMonitorTool's download changed (sha256 $old, now $h)" }
    Set-Content -LiteralPath $rec $h
    $script:mmt = Join-Path (Unzip $zip) 'MultiMonitorTool.exe'
    $script:mmt
}
function VddScreen {   # the VDD's screen (a MultiMonitorTool row) if it is on the desktop, else nothing
    New-Item -ItemType Directory -Force $tmp | Out-Null; $csv = Join-Path $tmp "mmt-$(Get-Random).csv"
    Start-Process (Get-Mmt) -ArgumentList "/scomma `"$csv`"" -Wait
    Import-Csv $csv | Where-Object { $_.'Short Monitor ID' -eq 'MTT1337' -and $_.Active -eq 'Yes' } | Select-Object -First 1
}
function VddOff([int]$wait) {   # take the VDD's screen off the desktop (waiting up to $wait s for it to appear); Sunshine switches it on per stream
    for ($i = 0; $i -le $wait; $i++) {
        $s = VddScreen
        if ($s) { Start-Process (Get-Mmt) -ArgumentList "/disable $($s.Name)" -Wait; Say "     took $($s.Name) (the VDD) off the desktop"; return }
        Start-Sleep 1
    }
}
function Restart-Vdd($id, $old) {   # make the VDD re-read vdd_settings.xml; put the old XML back if it doesn't come back OK
    & pnputil.exe /restart-device "$id" | Out-Null; Start-Sleep 4
    if ((Get-PnpDevice -InstanceId $id).Status -eq 'OK') { return }
    if ($old) { Copy-Item $old $VddXml -Force; & pnputil.exe /restart-device "$id" | Out-Null; Start-Sleep 4 }
    throw "the VDD didn't come back OK with pc\vdd_settings.xml$(if ($old) { '; the old XML is back' }) (status now $((Get-PnpDevice -InstanceId $id).Status))"
}
function Restart-Sunshine {   # (re)start Sunshine; wait up to 45 s for it to list the displays and test the encoders
    $t = (Get-Date).AddSeconds(-1)
    Restart-Service SunshineService -Force
    for ($i = 0; $i -lt 45; $i++) {   # Sunshine starts a fresh log each start
        $first = Get-Content -LiteralPath $SunLog -TotalCount 1 -ErrorAction SilentlyContinue
        if ($first -match '^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)' -and [datetime]$matches[1] -ge $t -and
            (Select-String -LiteralPath $SunLog -Pattern 'Found (HEVC|H\.264) encoder' -Quiet)) { return }
        Start-Sleep 1
    }
    Say '     note: Sunshine did not report its encoders within 45 s'
}

try {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin -and -not $dry) { throw 'run this from an elevated PowerShell (Run as administrator), or add -WhatIf for a dry run' }
    $main = Main
    if (-not $main) { throw "no display adapter matching '$GpuMatch' found: set `$GpuMatch in common.ps1 to your GPU's name (the display guard watches it)" }
    if (-not $dry) { New-Item -ItemType Directory -Force $state | Out-Null }
    Say "== tabd PC install $(Get-Date -Format s)$(if ($dry) { ', DRY RUN (-WhatIf): nothing will change' }); main monitor $main; Sunshine base port $Port ($portWhy)"

    # 1. Ports: with -ReservePorts, reserve Sunshine's range before it binds anything there. An existing reservation stays.
    $gaps = @(Get-PortGaps)
    if (-not $gaps) { Say "OK   ports $PortFirst-$PortLast reserved (persistent; ipv4+ipv6, tcp+udp)" }
    elseif (-not $ReservePorts) { Say "OK   ports $PortFirst-$PortLast not reserved (add -ReservePorts if WSL's mirrored networking or Docker takes them)" }
    else {
        $users = @(Get-PortUsers | Where-Object { $_ -notmatch '^sunshine ' })
        if ($users) { throw "ports $PortFirst-$PortLast are in use by $($users -join ', '), so Windows can't reserve them. If that's WSL or Docker: run 'wsl --shutdown' and quit Docker Desktop, then run install.ps1 again." }
        if (Get-PortUsers) { $restartSun = $true }   # Sunshine itself holds them: stopped for the reservation, started again below
        Step "reserve ports $PortFirst-$PortLast for $($gaps -join ', ') (netsh ... add excludedportrange ... store=persistent)" {
            if (Get-PortUsers) { Stop-Service SunshineService -Force; Start-Sleep 2 }
            foreach ($g in $gaps) {
                $ipv, $p = $g -split '/'
                $out = netsh int $ipv add excludedportrange protocol=$p startport=$PortFirst numberofports=$($PortLast - $PortFirst + 1) store=persistent
                Say "     $g`: $(($out | Where-Object { $_ }) -join ' ')"
            }
            $left = @(Get-PortGaps)
            if ($left) { throw "netsh could not reserve $($left -join ', '): a port is still in use, or another reservation overlaps (netsh int ipv4 show excludedportrange protocol=tcp). If it's WSL or Docker: run 'wsl --shutdown' and quit Docker Desktop. Wait a minute, then run install.ps1 again." }
        }
    }

    # 2. Sunshine at the pinned version.
    $v = Get-SunVer
    if ($v -eq $SunVer) { Say "OK   Sunshine $SunVer" }
    elseif ($v) { throw "Sunshine $v is installed, but the kit pins $SunVer (newer versions changed the input backends). Uninstall it (Settings > Apps > Sunshine), then run install.ps1 again." }
    else {
        $sunNew = $true
        Step "install Sunshine $SunVer, silent: $SunUrl (sha256 $SunSha)" {
            $p = Start-Process (Fetch $SunUrl $SunSha) -ArgumentList '/S' -Wait -PassThru
            if ((Get-SunVer) -ne $SunVer) { throw "the Sunshine installer exited $($p.ExitCode) and $SunExe is not $SunVer" }
        }
    }
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        if ((winget pin list --disable-interactivity | Out-String) -match 'LizardByte\.Sunshine') { Say 'OK   winget pin keeps Sunshine out of "winget upgrade --all"' }
        else {
            Step 'winget pin add LizardByte.Sunshine, so "winget upgrade --all" leaves the pinned version alone' {
                # Not --installed: with it winget says "Found Sunshine", exits 0 and pins nothing (winget 1.29).
                winget pin add --id LizardByte.Sunshine --exact --accept-source-agreements --disable-interactivity | Out-Null
                if (-not ((winget pin list --disable-interactivity | Out-String) -match 'LizardByte\.Sunshine')) { Say '     note: the winget pin did not take; harmless. Later: winget pin add --id LizardByte.Sunshine --exact' }
            }
        }
    }

    # 3. Firewall: Sunshine's installer adds the rules; put them back if they're missing.
    if (Test-Firewall) { Say 'OK   firewall lets Sunshine in (tcp + udp)' }
    else { Step "add Sunshine's inbound firewall rules ($SunDir\scripts\add-firewall-rule.bat)" { & "$SunDir\scripts\add-firewall-rule.bat" | Out-Null; if (-not (Test-Firewall)) { throw 'the firewall rules are still missing' } } }

    # 4. VDD settings, then the driver. On a fresh install the XML is in place before the driver first starts.
    $xmlOk = SameFile $VddXml "$Kit\vdd_settings.xml"
    if ($xmlOk) { Say "OK   $VddXml matches pc\vdd_settings.xml" }
    else {
        Step "write $VddXml from pc\vdd_settings.xml (the old one kept as .bak-<time>)" {
            New-Item -ItemType Directory -Force (Split-Path $VddXml) | Out-Null
            $script:oldXml = Backup $VddXml
            Copy-Item "$Kit\vdd_settings.xml" $VddXml -Force
        }
    }
    $vdd = Get-Vdd
    if (-not $vdd) {
        Step "install VDD by MTT $VddVer (release 24.12.24: $VddUrl, sha256 checked) with NefCon v1.14.0 ($NefUrl), trusting its signer (SignPath Foundation) like VDD's own installer does, then take the VDD's screen off the desktop until a stream needs it (MultiMonitorTool, $MmtUrl); guarded." {
            $drv = Unzip (Fetch $VddUrl $VddSha); $nef = Unzip (Fetch $NefUrl $NefSha)
            $store = New-Object Security.Cryptography.X509Certificates.X509Store 'TrustedPublisher', 'LocalMachine'
            $store.Open('ReadWrite'); $store.Add((Get-AuthenticodeSignature "$drv\mttvdd.cat").SignerCertificate); $store.Close()
            Guarded {
                $p = Start-Process "$nef\x64\nefconc.exe" -ArgumentList "install `"$drv\MttVDD.inf`" Root\MttVDD" -Wait -PassThru -NoNewWindow
                for ($i = 0; $i -lt 15 -and -not (Get-Vdd); $i++) { Start-Sleep 1 }
                if (-not (Get-Vdd)) { throw "the VDD did not install (nefconc exited $($p.ExitCode))" }
                VddOff 15
            }
        }
    } elseif ($vdd.Status -ne 'OK') { throw "the VDD is installed but its status is $($vdd.Status): see Device Manager > Display adapters > Virtual Display Driver" }
    else {
        Say "OK   VDD by MTT $($vdd.Version)$(if ($vdd.Version -ne $VddVer) { " (NOTE: the kit was built on $VddVer; left as it is)" })"
        if (-not $xmlOk) {
            Step 'restart the VDD so it reads the new modes (guarded; the old XML goes back if it fails)' {
                $on = [bool](VddScreen)   # a stream is running
                Guarded { Restart-Vdd $vdd.Id $script:oldXml; if (-not $on) { VddOff 5 } }
            }
        }
    }

    # 5. apps.json: Desktop only. tabd streams nothing else, and another app running would leave the tablet dark.
    if (SameFile $SunApps "$Kit\sunshine-apps.json") { Say 'OK   apps.json (Desktop only)' }
    else {
        $restartSun = $true
        Step "write $SunApps from pc\sunshine-apps.json: Desktop only (the old one kept as .bak-<time>)" { Backup $SunApps | Out-Null; Copy-Item "$Kit\sunshine-apps.json" $SunApps -Force }
    }

    # 6. sunshine.conf, with this PC's base port, and its VDD device id as output_name (Sunshine lists the displays in its log when it starts).
    $id = Get-VddId
    if (-not $id) {
        Step "restart Sunshine so its log lists the displays, to find the VDD's device id" { Restart-Sunshine }
        if (-not $dry) { $id = Get-VddId; if (-not $id) { throw "Sunshine's log ($SunLog) lists no 'VDD by MTT' display. Is the VDD installed and OK (Device Manager)?" } }
    }
    $want = Want-Conf $(if ($id) { $id } else { '<the VDD id from the log>' })
    $have = @(Get-Content -LiteralPath $SunConf -ErrorAction SilentlyContinue)
    if ((Norm ($have -join "`n")) -ceq (Norm $want)) { Say "OK   sunshine.conf (output_name $id)" }
    else {
        $restartSun = $true
        $add = @($want -split "`r`n" | Where-Object { $have -cnotcontains $_ }); $del = @($have | Where-Object { ($want -split "`r`n") -cnotcontains $_ })
        $chg = @($(if ($add) { "+ $($add -join ' | ')" }), $(if ($del) { "- $($del -join ' | ')" }) | Where-Object { $_ })
        Step "write sunshine.conf (the old one kept as .bak-<time>): $($chg -join '; ')" {
            Backup $SunConf | Out-Null
            [IO.File]::WriteAllText($SunConf, $want + "`r`n", (New-Object Text.UTF8Encoding $false))
        }
    }

    # 7. Start automatically, so the tablet works after a reboot, at the Windows login screen too.
    $svc = Get-Service SunshineService -ErrorAction SilentlyContinue
    if (-not $svc -and -not $dry) { throw 'SunshineService is missing: uninstall Sunshine, then run install.ps1 again' }
    if ($svc.StartType -eq 'Automatic') { Say 'OK   SunshineService starts automatically' }
    else { Step "set SunshineService to start automatically (now: $(if ($svc) { $svc.StartType } else { 'not installed yet' }))" { Set-Service SunshineService -StartupType Automatic } }

    # 8. Running, with the current settings.
    if ($restartSun) { Step 'restart Sunshine to load the new settings' { Restart-Sunshine } }
    elseif ((Get-Service SunshineService -ErrorAction SilentlyContinue).Status -ne 'Running') { Step 'start Sunshine' { Restart-Sunshine } }
    else { Say 'OK   Sunshine running' }

    # 9. The web login is yours to type, on Sunshine's own first-run page. The web UI answers 401 when one is set.
    # (a dry run asks Sunshine where it listens now: a real run has restarted it on $Port by here)
    $code = if ($dry -and $sunNew) { 'n/a' } elseif ($dry) { $cp = Get-ConfPort; Get-WebLogin "https://localhost:$(if ($cp) { $cp + 1 } else { 47990 })" } else { Get-WebLogin }
    if ($code -eq '401') { Say 'OK   Sunshine web login is set' }
    else { Step "open $WebUi/ in the browser: Sunshine asks for a new web login there (you type it; the web UI answered $code)" { Browse "$WebUi/" } }

    Say ''
    if ($dry) { Say $(if ($plan.Count) { "Dry run: $($plan.Count) change(s) planned, nothing done." } else { 'Dry run: nothing to change, this PC is set up.' }) }
    else { Say $(if ($done.Count) { "Done: $($done.Count) change(s). Log: $logFile" } else { 'Nothing to change: this PC is already set up.' }) }
    Say @"

Now do this (needs your hands):
  1. Sunshine web login: $(if ($code -eq '401') { 'already set.' } else { "set it at $WebUi/ (the browser tab this opened)." })
  2. Pair the tablet (once per Sunshine install): power it on with its Ethernet plugged in; it shows a PIN on its glass.
     Open $WebUi/pin, sign in, enter the PIN and a name (tablet). The picture appears within seconds.
  3. Check: powershell -ExecutionPolicy Bypass -File pc\check.ps1   (every line PASS)
"@
    if ($sunNew -and -not $dry) { Browse "$WebUi/pin" }
    if (-not $dry) { Say ''; & "$Kit\check.ps1"; exit $LASTEXITCODE }
} catch { Say "STOP $($_.Exception.Message)"; exit 1 }
finally { if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue } }
