# =====================================================================
#  watch-weixin.ps1 -- find out HOW WeChat tells the user about a message
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\watch-weixin.ps1 -Seconds 90
#
#  WHY THIS EXISTS
#  ---------------
#  WeChat (the new 4.x "Weixin", Qt-based) does not use the Windows notification
#  platform at all: it is not in HKCU\...\Notifications\Settings, it has never written
#  a row to wpndatabase.db, and it does not raise a tray balloon. It draws its own popup.
#  So the bar has to find that popup, and the popup only exists while a message is
#  arriving -- which is why this watcher needs a message to be sent while it runs.
#
#  WHAT IT REPORTS, for every window that appears or disappears in a Weixin process:
#  its class, title, rectangle, visibility, whether it takes clicks, and -- for a new
#  VISIBLE one -- whether it can be read through UI Automation and whether PrintWindow
#  can photograph it. Those three answers decide which hook is possible at all.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [int]$Seconds = 90,
    [int]$IntervalMs = 120,
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ([string]::IsNullOrEmpty($Out)) {
    $Out = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) 'weixin-watch.txt'
}
$shotDir = Join-Path (Split-Path -Parent $Out) 'captures'
New-Item -ItemType Directory -Force -Path $shotDir | Out-Null

$code = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public struct RECT { public int L, T, R, B; }
public static class Watcher {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int c);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int c);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int index);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public static List<IntPtr> Windows = new List<IntPtr>();
  public static HashSet<uint> Pids = new HashSet<uint>();
  public static bool Cb(IntPtr h, IntPtr l) {
    uint pid; GetWindowThreadProcessId(h, out pid);
    if (Pids.Contains(pid)) Windows.Add(h);
    return true;
  }
  public static bool CbChild(IntPtr h, IntPtr l) {
    uint pid; GetWindowThreadProcessId(h, out pid);
    if (Pids.Contains(pid)) Windows.Add(h);
    return true;
  }
  public static string Describe(IntPtr h) {
    var cls = new StringBuilder(256); GetClassName(h, cls, 256);
    var txt = new StringBuilder(512); GetWindowTextW(h, txt, 512);
    RECT r; GetWindowRect(h, out r);
    int style = GetWindowLong(h, -16);          // GWL_STYLE
    int ex    = GetWindowLong(h, -20);          // GWL_EXSTYLE
    return string.Format("hwnd={0} pid={1} class='{2}' title='{3}' visible={4} rect=[{5},{6} {7}x{8}] style=0x{9:X8} ex=0x{10:X8}",
      h, GetPid(h), cls, txt, IsWindowVisible(h), r.L, r.T, r.R-r.L, r.B-r.T, style, ex);
  }
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
  public static uint GetPid(IntPtr h) { uint pid; GetWindowThreadProcessId(h, out pid); return pid; }
}
'@
Add-Type -TypeDefinition $code -Language CSharp

$log = New-Object System.Text.StringBuilder
function Say([string]$text) {
    Write-Output $text
    [void]$log.AppendLine($text)
}

$procs = Get-Process | Where-Object { $_.ProcessName -match 'Weixin|WeChat' }
if (-not $procs) {
    Say 'WeChat is not running -- start it first.'
    exit 1
}
foreach ($p in $procs) { [void][Watcher]::Pids.Add([uint32]$p.Id) }
Say ("watching {0} WeChat process(es): {1}" -f $procs.Count, (($procs | ForEach-Object { $_.Id }) -join ', '))
# ASCII only: the repo rule for .ps1 files. The Chinese name of the File Transfer chat is written here as a
# description rather than as the text itself.
Say 'NOW SEND A MESSAGE: from your phone to the File Transfer chat (wen jian chuan shu zhu shou), or have someone message you.'
Say ("watching for {0} seconds ..." -f $Seconds)

$seen = @{}
$deadline = (Get-Date).AddSeconds($Seconds)

# --- can the content be read at all? -------------------------------------------------
#
# WeChat's window is a self-drawn MMUI surface: UI Automation exposes the frame and nothing
# inside it, so the only way to read a message would be to photograph the popup and OCR it.
# Windows ships that engine, so the question is whether it is usable from here and whether a
# Chinese recogniser is installed -- reported before the wait, because the answer decides
# whether the bar can show "who said what" or only "WeChat has a message".
try {
    [void][Windows.Media.Ocr.OcrEngine, Windows.Foundation, ContentType=WindowsRuntime]
    $langs = [Windows.Media.Ocr.OcrEngine]::AvailableRecognizerLanguages
    $names = @()
    foreach ($l in $langs) { $names += $l.LanguageTag }
    Say ("OCR engine available; recogniser languages: {0}" -f ($names -join ', '))
} catch {
    Say ("OCR engine NOT available from here: {0}" -f $_.Exception.Message)
}

while ((Get-Date) -lt $deadline) {
    [Watcher]::Windows.Clear()
    [void][Watcher]::EnumWindows([Watcher+EnumProc]{ param($h, $l) [Watcher]::Cb($h, $l) }, [IntPtr]::Zero)
    # Children as well: a Qt popup is usually its own top-level window, but a MMUI surface may hang a child
    # off the main window instead, and a hook that only watched top-level windows would never see it.
    foreach ($parent in @([Watcher]::Windows)) {
        [void][Watcher]::EnumChildWindows($parent, [Watcher+EnumProc]{ param($h, $l) [Watcher]::CbChild($h, $l) }, [IntPtr]::Zero)
    }
    $snapshot = @{}
    foreach ($h in [Watcher]::Windows) { $snapshot[[int64]$h] = $true }

    foreach ($h in [Watcher]::Windows) {
        $key = [int64]$h
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        $line = 'NEW  ' + [Watcher]::Describe($h)
        Say $line

        # A new VISIBLE window is the one that matters: photograph it and try to read it.
        if ([Watcher]::IsWindowVisible($h)) {
            $r = New-Object RECT
            [void][Watcher]::GetWindowRect($h, [ref]$r)
            $w = $r.R - $r.L; $ht = $r.B - $r.T
            if ($w -gt 40 -and $ht -gt 20 -and $w -lt 2000 -and $ht -lt 1200) {
                try {
                    $bmp = New-Object System.Drawing.Bitmap $w, $ht
                    $g = [System.Drawing.Graphics]::FromImage($bmp)
                    $hdc = $g.GetHdc()
                    $ok = [Watcher]::PrintWindow($h, $hdc, 2)   # 2 = PW_RENDERFULLCONTENT
                    $g.ReleaseHdc($hdc)
                    $file = Join-Path $shotDir ("weixin-win-{0}.png" -f $key)
                    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
                    $g.Dispose(); $bmp.Dispose()
                    Say ("     PrintWindow={0} -> {1}" -f $ok, $file)
                } catch { Say ("     PrintWindow threw: {0}" -f $_.Exception.Message) }

                try {
                    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes -ErrorAction SilentlyContinue
                    $el = [System.Windows.Automation.AutomationElement]::FromHandle($h)
                    if ($el) {
                        Say ("     UIA root name='{0}' type='{1}'" -f $el.Current.Name, $el.Current.ControlType.ProgrammaticName)
                        $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
                        $child = $walker.GetFirstChild($el)
                        $n = 0
                        while ($child -and $n -lt 12) {
                            Say ("       UIA child '{0}' [{1}]" -f $child.Current.Name, $child.Current.ControlType.ProgrammaticName)
                            $child = $walker.GetNextSibling($child)
                            $n++
                        }
                    }
                } catch { }
            }
        }
    }

    foreach ($key in @($seen.Keys)) {
        if (-not $snapshot.ContainsKey($key)) {
            Say ("GONE hwnd={0}" -f $key)
            $seen.Remove($key)
        }
    }
    Start-Sleep -Milliseconds $IntervalMs
}

[System.IO.File]::WriteAllText($Out, $log.ToString(), [System.Text.Encoding]::UTF8)
Say ("log written to {0}" -f $Out)
