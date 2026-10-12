# =====================================================================
#  probe-start.ps1 -- find out which keystroke actually opens the Start menu
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-start.ps1
#
#  WHY THIS EXISTS
#
#  The bar's Start button reports that it sent the Windows key ("start click: sent")
#  and the Start menu does not appear. Sending the key is therefore not the same as
#  the shell acting on it, and the difference cannot be seen from inside the bar --
#  SendInput returns success either way, because success is "the event entered the
#  input queue", not "the Start menu opened".
#
#  So this tries each plausible route ON ITS OWN, with a pause between them, and
#  prints what the shell did after each. One of them will work or none will, and
#  either answer is worth having: "none of them works from a synthetic keystroke"
#  would mean the whole approach is wrong rather than mistuned.
#
#  KEY POINTS ABOUT EACH ROUTE
#
#  * Ctrl+Esc is the OTHER documented Start shortcut, and it does not go through the
#    Windows key's modifier logic at all -- which is exactly the logic that can turn
#    a Win press into a keyboard shortcut instead of the Start action.
#  * SendInput and keybd_event enter the queue through different paths. If one is
#    filtered and the other is not, that tells us which filter is in the way.
#  * A longer gap after mouse-up matters: the bar opens the menu while the button is
#    still logically down, and Windows treats a key pressed during a held button as
#    part of a chord.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [int]$GapMs = 2500
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace Probe -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[StructLayout(LayoutKind.Sequential)] public struct MONITORINFO {
    public int cbSize; public RECT monitor; public RECT work; public uint flags; }

[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
[DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern IntPtr MonitorFromPoint(POINT p, uint f);
[DllImport("user32.dll")] public static extern bool GetMonitorInfo(IntPtr h, ref MONITORINFO i);

public const byte VK_LWIN = 0x5B;
public const byte VK_CONTROL = 0x11;
public const byte VK_ESCAPE = 0x1B;
public const uint KEYUP = 0x0002;

public static bool SendKey(ushort vk, bool up) {
    INPUT input = new INPUT();
    input.type = 1;   // INPUT_KEYBOARD
    input.ki.wVk = vk;
    input.ki.wScan = 0;
    input.ki.dwFlags = up ? KEYUP : 0;
    return SendInput(1, ref input, Marshal.SizeOf(typeof(INPUT))) == 1;
}

[StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT {
    public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Sequential)] public struct INPUT {
    public uint type; public KEYBDINPUT ki; }
[DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, ref INPUT p, int size);
'@

function Show-State([string]$when) {
    $mon = New-Object Probe.Native+MONITORINFO
    $mon.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($mon)
    $pt = New-Object Probe.Native+POINT
    $pt.X = 0; $pt.Y = 0
    [Probe.Native]::GetMonitorInfo([Probe.Native]::MonitorFromPoint($pt, 0), [ref]$mon) | Out-Null

    $fg = [Probe.Native]::GetForegroundWindow()
    $sb = New-Object System.Text.StringBuilder 128
    [Probe.Native]::GetClassName($fg, $sb, 128) | Out-Null

    # A modifier that is logically down is the thing that turns Win into a shortcut, so
    # it is reported rather than assumed absent.
    $ctrl = ([Probe.Native]::GetAsyncKeyState([Probe.Native]::VK_CONTROL) -band 0x8000) -ne 0
    $win  = ([Probe.Native]::GetAsyncKeyState([Probe.Native]::VK_LWIN)    -band 0x8000) -ne 0

    Write-Host ("  [{0}] work.top={1}  foreground='{2}'  ctrl-down={3} win-down={4}" -f `
                $when, $mon.work.T, $sb.ToString(), $ctrl, $win)
}

function Reset-Modifiers {
    # Every modifier is released before each attempt. A held modifier is the single most
    # likely reason a synthetic Win press does nothing, and it is invisible otherwise.
    foreach ($vk in 0x11, 0x10, 0x12, 0x5B, 0x5C) {
        [Probe.Native]::keybd_event([byte]$vk, 0, [Probe.Native]::KEYUP, [IntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 80
}

Write-Host '=== which keystroke opens the Windows 11 Start menu? ===' -ForegroundColor Cyan
Write-Host ''
Write-Host 'Nothing will be typed anywhere: only Ctrl+Esc and the Windows key are sent.'
Write-Host 'Watch the screen after each attempt.'
Write-Host ''

Show-State 'before anything'

# --- attempt 1: Ctrl+Esc, through SendInput -------------------------------
Write-Host ''
Write-Host '[1/4] Ctrl+Esc (SendInput) -- the other documented Start shortcut' -ForegroundColor Yellow
Reset-Modifiers
[Probe.Native]::SendKey([Probe.Native]::VK_CONTROL, $false) | Out-Null
[Probe.Native]::SendKey([Probe.Native]::VK_ESCAPE, $false) | Out-Null
Start-Sleep -Milliseconds 40
[Probe.Native]::SendKey([Probe.Native]::VK_ESCAPE, $true) | Out-Null
[Probe.Native]::SendKey([Probe.Native]::VK_CONTROL, $true) | Out-Null
Start-Sleep -Milliseconds $GapMs
Show-State 'after Ctrl+Esc'
# Closed again if it opened, so the next attempt starts from a known state.
[Probe.Native]::SendKey([Probe.Native]::VK_ESCAPE, $false) | Out-Null
[Probe.Native]::SendKey([Probe.Native]::VK_ESCAPE, $true) | Out-Null
Start-Sleep -Milliseconds 600

# --- attempt 2: the Windows key, through SendInput ------------------------
Write-Host ''
Write-Host '[2/4] Windows key (SendInput), with no Ctrl tap first' -ForegroundColor Yellow
Reset-Modifiers
[Probe.Native]::SendKey([Probe.Native]::VK_LWIN, $false) | Out-Null
Start-Sleep -Milliseconds 40
[Probe.Native]::SendKey([Probe.Native]::VK_LWIN, $true) | Out-Null
Start-Sleep -Milliseconds $GapMs
Show-State 'after Win (SendInput)'
[Probe.Native]::SendKey([Probe.Native]::VK_LWIN, $false) | Out-Null
[Probe.Native]::SendKey([Probe.Native]::VK_LWIN, $true) | Out-Null
Start-Sleep -Milliseconds 600

# --- attempt 3: the Windows key, through keybd_event ----------------------
Write-Host ''
Write-Host '[3/4] Windows key (keybd_event) -- a different path into the queue' -ForegroundColor Yellow
Reset-Modifiers
[Probe.Native]::keybd_event([Probe.Native]::VK_LWIN, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 40
[Probe.Native]::keybd_event([Probe.Native]::VK_LWIN, 0, [Probe.Native]::KEYUP, [IntPtr]::Zero)
Start-Sleep -Milliseconds $GapMs
Show-State 'after Win (keybd_event)'
[Probe.Native]::keybd_event([Probe.Native]::VK_LWIN, 0, 0, [IntPtr]::Zero)
[Probe.Native]::keybd_event([Probe.Native]::VK_LWIN, 0, [Probe.Native]::KEYUP, [IntPtr]::Zero)
Start-Sleep -Milliseconds 600

# --- attempt 4: Ctrl+Esc, through keybd_event -----------------------------
Write-Host ''
Write-Host '[4/4] Ctrl+Esc (keybd_event)' -ForegroundColor Yellow
Reset-Modifiers
[Probe.Native]::keybd_event([Probe.Native]::VK_CONTROL, 0, 0, [IntPtr]::Zero)
[Probe.Native]::keybd_event([Probe.Native]::VK_ESCAPE, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 40
[Probe.Native]::keybd_event([Probe.Native]::VK_ESCAPE, 0, [Probe.Native]::KEYUP, [IntPtr]::Zero)
[Probe.Native]::keybd_event([Probe.Native]::VK_CONTROL, 0, [Probe.Native]::KEYUP, [IntPtr]::Zero)
Start-Sleep -Milliseconds $GapMs
Show-State 'after Ctrl+Esc (keybd_event)'
[Probe.Native]::keybd_event([Probe.Native]::VK_ESCAPE, 0, 0, [IntPtr]::Zero)
[Probe.Native]::keybd_event([Probe.Native]::VK_ESCAPE, 0, [Probe.Native]::KEYUP, [IntPtr]::Zero)

Write-Host ''
Write-Host 'Now press the Windows key ON YOUR KEYBOARD.' -ForegroundColor Cyan
Write-Host 'If that opens it and none of the four above did, the shell is ignoring'
Write-Host 'synthetic keys and the approach has to change rather than be retuned.'
Write-Host ''
Write-Host 'Which of the four opened the Start menu?'
