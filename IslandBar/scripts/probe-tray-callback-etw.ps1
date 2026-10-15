# =====================================================================
#  probe-tray-callback-etw.ps1 -- WHAT MESSAGE DOES THE SHELL SEND WHEN A TRAY ICON IS CLICKED?
#
#      (as administrator)
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-tray-callback-etw.ps1 -Cell 6 -Right
#
#  WHY THIS EXISTS
#
#  A press currently moves the pointer onto the icon and clicks -- a real click, which works, and which the
#  user watches happen. The way out is to send the application the same message the shell sends when its
#  icon is clicked: no pointer, no panel, no flash. That message is the application's own registered
#  `uCallbackMessage`, and it cannot be looked up: the shell's registry entry for a tray icon carries UID,
#  IconGuid, ExecutablePath, InitialTooltip, IconSnapshot, IsPromoted and Publisher and nothing else
#  (checked), and it cannot be OBSERVED from the shell's side either -- a WH_GETMESSAGE hook inside Explorer
#  sees only messages retrieved by Explorer's own loop, while the shell-to-application notification is a
#  cross-process delivery that lands in the APPLICATION's queue.
#
#  So it is learned from a trace instead. This machine's Win32k ETW provider publishes the keyword
#  `Microsoft-Windows-Win32k/Messages` (0x0400000000000000), which carries the message traffic:
#
#    1. the application's windows are enumerated first (their handles are what the decoded events get
#       matched on);
#    2. a short trace is started with that one keyword;
#    3. a press is made with the EXISTING real-click route -- the same thing the user's own click does, and
#       it is known to work;
#    4. the trace is stopped and decoded with tracerpt (which reads the provider's registered manifest, so
#       no consumer of our own is needed).
#
#  WHAT IT PRINTS: every decoded event whose window is one of the application's, with its message number.
#  A tray callback stands out as a message in the WM_USER/WM_APP range (0x0400-0xBFFF) arriving right after
#  the click, and it is the one to cache.
#
#  NEEDS ADMINISTRATOR, because starting a trace session does. It changes nothing else: the session is
#  stopped at the end whatever happens.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

param(
    [string]$AppImage = 'wegame.exe',
    [int]$Cell = 6,
    [switch]$Right,
    [int]$Seconds = 3,
    [string]$Exe = "$env:USERPROFILE\IslandBar\island_bar.exe",
    [string]$Session = 'IslandBarTray',
    # -Watch: do not press anything; trace for -Seconds and let somebody else press by hand. Used to learn
    # from a dock that DOES press by posting the application's own callback (MyDockFinder does: measured --
    # its tray module trayico.exe imports PostMessage and nothing else, so its press is a message, and the
    # application then RETRIEVES that message, which is the one event that carries the message number).
    [switch]$Watch
)

$ErrorActionPreference = 'Continue'
Add-Type -Namespace TrayEtW -Name Win -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
'@

$captures = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'captures'
if (-not (Test-Path $captures)) { New-Item -ItemType Directory -Path $captures | Out-Null }
$etl = Join-Path $captures 'tray-callback.etl'
$csv = Join-Path $captures 'tray-callback.csv'
$report = Join-Path $captures 'tray-callback-report.txt'

# EVERYTHING IS ALSO WRITTEN TO A REPORT FILE, because this has to be run from an elevated shell that nobody
# is reading over the developer's shoulder: the console is for the person running it, the file is for the
# analysis afterwards.
Start-Transcript -Path $report -Force | Out-Null

function Get-AppWindows([string]$image) {
    $names = @([System.IO.Path]::GetFileNameWithoutExtension($image))
    $pids = @(Get-Process -Name $names -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $list = @()
    if ($pids.Count -eq 0) { return $list }
    $cb = [TrayEtW.Win+EnumProc]{
        param([IntPtr]$h, [IntPtr]$p)
        $procId = 0
        [void][TrayEtW.Win]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($script:pids -contains $procId) {
            $cls = New-Object System.Text.StringBuilder 256
            [void][TrayEtW.Win]::GetClassName($h, $cls, 256)
            $script:list += [pscustomobject]@{
                hwnd = $h; dec = $h.ToInt64(); hex = ('{0:X}' -f $h.ToInt64()); cls = $cls.ToString()
            }
        }
        return $true
    }
    $script:pids = $pids
    $script:list = @()
    [void][TrayEtW.Win]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:list
}

"the application's windows (these handles are what the events are matched on):"
$windows = Get-AppWindows $AppImage
foreach ($w in $windows) { "  hwnd $($w.hex) [$($w.cls)]" }
if ($windows.Count -eq 0) { "no window of $AppImage is open -- is it running?"; exit 1 }

Remove-Item -LiteralPath $etl -ErrorAction SilentlyContinue
& logman stop $Session -ets *> $null
$create = & logman create trace $Session -p "{8C416C79-D49B-4F01-A467-E56D3AA8234C}" 0x0400000000000000 0x5 -o $etl -bs 1024 -nb 64 -ets 2>&1
$create | ForEach-Object { "logman: $_" }
if (-not (Test-Path $etl)) { 'the trace did not start (this needs administrator)' ; exit 1 }

"pressing cell $Cell with the real-click route while the trace runs"
$arguments = @('--tray-press', "$Cell", '--fire')
if ($Right) { $arguments += '--right' }
if ($Watch) {
    "no press from this script: press a tray icon BY HAND now ($Seconds seconds)"
    Start-Sleep -Seconds $Seconds
} else {
    $press = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
    Start-Sleep -Seconds $Seconds
    if (-not $press.HasExited) { Start-Sleep -Seconds 2 }
}
& logman stop $Session -ets | Out-Null
Start-Sleep -Milliseconds 500
"trace stopped: " + [math]::Round((Get-Item $etl).Length / 1MB, 1) + " MB"

& tracerpt $etl -o $csv -of CSV -y *> $null
if (-not (Test-Path $csv)) { 'tracerpt produced nothing'; exit 1 }
"decoded to CSV: " + [math]::Round((Get-Item $csv).Length / 1MB, 1) + " MB"

# The window handles are what ties a decoded event to this application. They appear in the CSV in whichever
# spelling the provider uses, so both are looked for.
$patterns = @()
foreach ($w in $windows) { $patterns += $w.hex; $patterns += "$($w.dec)" }
"--- decoded events naming one of those windows ---"
$hits = Select-String -Path $csv -Pattern ($patterns -join '|') -SimpleMatch:$false |
        Select-Object -First 40
foreach ($h in $hits) { $h.Line }
"--- message numbers seen (column hints for the next look) ---"
$all = Get-Content $csv
"CSV lines: $($all.Count)"
foreach ($line in ($all | Select-Object -First 6)) { $line }

# THE NUMBERS OF INTEREST, WHATEVER THE COLUMN NAMES ARE ON THIS BUILD: a tray callback is a message in the
# application's own range (WM_USER 0x400 .. WM_APP 0xBFFF). Counting them and their target windows is what
# makes the one event that matters stand out when the handle match above found nothing.
"--- CSV columns ---"
$columns = $all[0] -split ','
"  " + ($columns -join ' | ')
"--- message-like fields in the WM_USER..WM_APP range (0x400-0xBFFF), this trace only ---"
$candidates = @{}
foreach ($line in $all) {
    foreach ($field in ($line -split ',')) {
        $f = $field.Trim('"', ' ')
        if ($f -match '^(0x)?[0-9A-Fa-f]{3,4}$') {
            $value = if ($f -like '0x*') { [Convert]::ToInt32($f.Substring(2), 16) } else { [int]$f }
            if ($value -ge 0x400 -and $value -le 0xBFFF) {
                if ($candidates.ContainsKey($value)) { $candidates[$value]++ } else { $candidates[$value] = 1 }
            }
        }
    }
}
$candidates.GetEnumerator() | Sort-Object -Property Value -Descending | Select-Object -First 15 |
    ForEach-Object { "  0x{0:X4} ({0}) x{1}" -f $_.Key, $_.Value }
if ($hits.Count -eq 0) {
    'no event named one of those windows: either the trace missed it or the message is carried somewhere else'
}
'=== done ==='
Stop-Transcript | Out-Null
"report written to $report"
