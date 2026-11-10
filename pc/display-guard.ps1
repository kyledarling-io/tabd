# Display guard: watch the main monitor once a second for -Seconds. If its RESOLUTION ever differs from -Expect ('WxH';
# default: what it is when the guard starts), stop Sunshine (it restores the topology) and, if still wrong,
# DisplaySwitch /internal. A refresh-rate-only change is logged, not reverted. Exits 3 if it had to revert.
# Never dies silently: every check is wrapped, errors are logged, and a heartbeat is written every 30 s.
param([int]$Seconds = 900, [string]$Log = (Join-Path $PSScriptRoot 'display-guard.log'), [string]$Expect)
. "$PSScriptRoot\common.ps1"   # Main
function Log($m) { try { Add-Content -Path $Log -Value ("{0:HH:mm:ss} {1}" -f (Get-Date), $m) } catch {} }
function Vdd() { (Get-CimInstance Win32_VideoController | Where-Object Name -match 'Virtual Display Driver' | Select-Object -First 1 | ForEach-Object { "$($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)@$($_.CurrentRefreshRate)" }) }
if (-not $Expect) { $Expect = (Main) -replace '@.*' }
$last = ''; $beat = Get-Date; $end = (Get-Date).AddSeconds($Seconds); $reverted = $false
Log "guard start ($Seconds s): expect main=$Expect, main=$(Main) vdd=$(Vdd)"
while ((Get-Date) -lt $end) {
    try {
        $m = Main; $v = Vdd
        if ("$m|$v" -ne $last) { Log "main=$m vdd=$v"; $last = "$m|$v" }
        if ($m -and $m -notlike "$Expect@*") {
            Log "MAIN MONITOR RESOLUTION CHANGED ($m): reverting"; $reverted = $true
            & sc.exe stop SunshineService | Out-Null
            Start-Sleep -Seconds 4
            if ((Main) -notlike "$Expect@*") { DisplaySwitch.exe /internal; Start-Sleep -Seconds 4 }
            Log "after revert: main=$(Main)"
            break
        }
        if (((Get-Date) - $beat).TotalSeconds -ge 30) { Log "heartbeat main=$m vdd=$v"; $beat = Get-Date }
    } catch { Log ("check error: " + $_.Exception.Message) }
    Start-Sleep -Seconds 1
}
Log 'guard end'
if ($reverted) { exit 3 }
