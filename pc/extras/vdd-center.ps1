# Opt-in, not installed by install.ps1: only for a PC where Windows doesn't keep the tablet screen's position across
# streams (it normally does, per monitor set: arrange it once in Settings > System > Display).
# Keep the tablet's screen (VDD by MTT, monitor id MTT1337) centered directly below the primary monitor.
# Sunshine runs this at every stream start (global_prep_cmd). It returns at once and does the work in the background:
# wait up to 10 s for the tablet screen to be active, then move it only if it isn't already in place.
# It only ever moves the MTT1337 screen; it never changes any monitor's mode.
# To use it, elevated: copy this file to %ProgramData%\tabd, unzip MultiMonitorTool x64
# (www.nirsoft.net/utils/multimonitortool-x64.zip) into %ProgramData%\tabd\MultiMonitorTool, add this line to
# sunshine.conf, then restart SunshineService:
#   global_prep_cmd = [{"do":"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"C:\\ProgramData\\tabd\\vdd-center.ps1\"","undo":"","elevated":false}]
# install.ps1 keeps this line (it points into %ProgramData%\tabd); it removes only an older kit's line, which pointed
# somewhere else.
param([switch]$Work)
$here = Split-Path -Parent $PSCommandPath
if (-not $Work) {
    Start-Process powershell -WindowStyle Hidden -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Work"
    exit 0
}
$mmt = Join-Path $here 'MultiMonitorTool\MultiMonitorTool.exe'   # next to this file, see above
$log = Join-Path $here 'vdd-center.log'
function Log($m) { try { if ((Test-Path $log) -and (Get-Item $log).Length -gt 200KB) { Move-Item $log "$log.old" -Force }; Add-Content $log ("{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $m) } catch {} }
function Rows() {
    $csv = Join-Path $env:TEMP "vdd-center-$PID.csv"
    & $mmt /scomma $csv; Start-Sleep -Milliseconds 300
    $r = Import-Csv $csv; Remove-Item $csv -ErrorAction SilentlyContinue; $r
}
function Xy($s) { $p = $s -split ',\s*'; [int]$p[0], [int]$p[1] }
function Wh($s) { $p = $s -split '\s*X\s*'; [int]$p[0], [int]$p[1] }
try {
    for ($i = 0; $i -lt 20; $i++) {
        $rows = Rows
        $vdd = $rows | Where-Object { $_.'Short Monitor ID' -eq 'MTT1337' -and $_.Active -eq 'Yes' } | Select-Object -First 1
        $pri = $rows | Where-Object { $_.Primary -eq 'Yes' } | Select-Object -First 1
        if ($vdd -and $pri) {
            $pw, $ph = Wh $pri.Resolution; $px, $py = Xy $pri.'Left-Top'
            $vw, $vh = Wh $vdd.Resolution; $vx, $vy = Xy $vdd.'Left-Top'
            $tx = $px + [int](($pw - $vw) / 2); $ty = $py + $ph
            if ($vx -eq $tx -and $vy -eq $ty) { Log "already centered: $($vdd.Name) ${vw}x${vh} at $vx,$vy"; exit 0 }
            & $mmt /SetMonitors "Name=$($vdd.Name) BitsPerPixel=32 Width=$vw Height=$vh DisplayFrequency=$($vdd.Frequency) DisplayOrientation=0 PositionX=$tx PositionY=$ty"
            Log "moved $($vdd.Name) ${vw}x${vh} from $vx,$vy to $tx,$ty"
            exit 0
        }
        Start-Sleep -Milliseconds 500
    }
    Log 'tablet screen not active within 10 s; nothing to do'
} catch { Log ('error: ' + $_.Exception.Message) }
