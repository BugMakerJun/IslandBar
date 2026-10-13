# =====================================================================
#  probe-taskbar-appbar-release.ps1 -- can the hidden taskbar's AppBar claim be RELEASED?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-taskbar-appbar-release.ps1
#
#  WHY THIS EXISTS
#
#  The bar hides the shell's taskbar, but a hidden AppBar KEEPS ITS CLAIM: the shell's own taskbar is a
#  registered appbar whether or not it can be seen, so the desktop's work area stays inset by its thickness
#  -- and a maximized window therefore keeps an unusable strip at the top, with our island floating in the
#  middle of it and nothing on either side. That is what the user photographed -- "when I maximize an
#  application it should go full screen, not leave this blank band" -- and what this probe is about.
#
#  The three questions, in order:
#
#    1. what does the work area say now?
#    2. does SHAppBarMessage(ABM_REMOVE) on Shell_TrayWnd give the strip back to the work area?
#    3. can the reservation be PUT BACK afterwards (so a bar that exits, or an uninstall, leaves the
#       machine as it found it)?  ABM_NEW + ABM_SETPOS on the taskbar's own rectangle, which is what the
#       taskbar itself sends.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -Namespace AppBarProbe -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[StructLayout(LayoutKind.Sequential)] public struct APPBARDATA {
    public uint cbSize;
    public IntPtr hWnd;
    public uint uCallbackMessage;
    public uint uEdge;
    public RECT rc;
    public IntPtr lParam;
}
[DllImport("shell32.dll")] public static extern uint SHAppBarMessage(uint msg, ref APPBARDATA data);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint action, uint param, ref RECT outRect, uint flags);
// Two declarations of the same function, so the "set" one needs its entry point spelled out.
[DllImport("user32.dll", EntryPoint="SystemParametersInfoW")] public static extern bool SystemParametersInfoSet(uint action, uint param, ref RECT inRect, uint flags);
[DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonitorProc cb, IntPtr data);
public delegate bool MonitorProc(IntPtr monitor, IntPtr dc, IntPtr rect, IntPtr data);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr monitor, ref MONITORINFO info);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
[StructLayout(LayoutKind.Sequential)] public struct MONITORINFO { public uint cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; }
'@

$ABM_NEW = 0x00000000
$ABM_REMOVE = 0x00000001
$ABM_SETPOS = 0x00000003
$ABE_TOP = 1

function WorkArea {
    $rect = New-Object AppBarProbe.Win+RECT
    [void][AppBarProbe.Win]::SystemParametersInfo(0x0030, 0, [ref]$rect, 0)   # SPI_GETWORKAREA
    return ('{0},{1} - {2},{3}  (height {4})' -f $rect.L, $rect.T, $rect.R, $rect.B, ($rect.B - $rect.T))
}

function MonitorWork {
    $info = New-Object AppBarProbe.Win+MONITORINFO
    $info.cbSize = 40
    $script:primary = $null
    $cb = [AppBarProbe.Win+MonitorProc]{
        param([IntPtr]$mon, [IntPtr]$dc, [IntPtr]$rect, [IntPtr]$data)
        $mi = New-Object AppBarProbe.Win+MONITORINFO
        $mi.cbSize = 40
        if ([AppBarProbe.Win]::GetMonitorInfo($mon, [ref]$mi)) {
            $script:primary = ('monitor {0},{1}-{2},{3}; work {4},{5}-{6},{7} (work height {8})' -f
                $mi.rcMonitor.L, $mi.rcMonitor.T, $mi.rcMonitor.R, $mi.rcMonitor.B,
                $mi.rcWork.L, $mi.rcWork.T, $mi.rcWork.R, $mi.rcWork.B,
                ($mi.rcWork.B - $mi.rcWork.T))
        }
        return $false
    }
    [void][AppBarProbe.Win]::EnumDisplayMonitors([IntPtr]::Zero, [IntPtr]::Zero, $cb, [IntPtr]::Zero)
    return $script:primary
}

$tray = [AppBarProbe.Win]::FindWindow('Shell_TrayWnd', $null)
"Shell_TrayWnd: $tray"

# A sandboxed shell sees its own, empty desktop: FindWindow answers 0 and every measurement below is
# taken against nothing. That cost a whole round of readings once, so it is now a hard stop.
if ($tray -eq [IntPtr]::Zero) {
    'STOP: Shell_TrayWnd not found. This probe must run on the interactive desktop (outside the sandbox).'
    exit 2
}

"1. before      : " + (WorkArea)
"   primary     : " + (MonitorWork)

$data = New-Object AppBarProbe.Win+APPBARDATA
$data.cbSize = 48
$data.hWnd = $tray
"2. asking the shell to ask the taskbar for its AppBar rectangle ..."
$query = New-Object AppBarProbe.Win+APPBARDATA
$query.cbSize = 48
$query.hWnd = $tray
[void][AppBarProbe.Win]::SHAppBarMessage(0x00000004, [ref]$query)   # ABM_GETTASKBARPOS
"   taskbar says: edge {0}, rect {1},{2}-{3},{4}" -f $query.uEdge, $query.rc.L, $query.rc.T, $query.rc.R, $query.rc.B
$taskbarEdge = $query.uEdge
$taskbarRect = New-Object AppBarProbe.Win+RECT
$taskbarRect.L = $query.rc.L; $taskbarRect.T = $query.rc.T
$taskbarRect.R = $query.rc.R; $taskbarRect.B = $query.rc.B

$remove = New-Object AppBarProbe.Win+APPBARDATA
$remove.cbSize = 48
$remove.hWnd = $tray
$removed = [AppBarProbe.Win]::SHAppBarMessage($ABM_REMOVE, [ref]$remove)
"   ABM_REMOVE returned $removed"
"   after       : " + (WorkArea)
"   primary     : " + (MonitorWork)

# Put it back, so that a bar which exits (or an uninstall) leaves the machine as it was found.
$add = New-Object AppBarProbe.Win+APPBARDATA
$add.cbSize = 48
$add.hWnd = $tray
[void][AppBarProbe.Win]::SHAppBarMessage($ABM_NEW, [ref]$add)
$set = New-Object AppBarProbe.Win+APPBARDATA
$set.cbSize = 48
$set.hWnd = $tray
$set.uEdge = $taskbarEdge
$set.rc = $taskbarRect
[void][AppBarProbe.Win]::SHAppBarMessage($ABM_SETPOS, [ref]$set)
"3. put back    : " + (WorkArea)
"   primary     : " + (MonitorWork)

# --- and the classic route, since the AppBar one did nothing ------------------------------
#
# The strip is NOT an AppBar registration at all: ABM_REMOVE returned success and the work area did not
# move, and ABM_GETTASKBARPOS answers "edge 0, rect 0,0-0,0" -- the shell does not consider its taskbar an
# appbar. What a maximized window obeys is the WORK AREA, and the work area follows the taskbar WINDOW's
# position. So: move that window off the top edge and ask again.

$trayRect = New-Object AppBarProbe.Win+RECT
[void][AppBarProbe.Win]::GetWindowRect($tray, [ref]$trayRect)
$trayHeight = $trayRect.B - $trayRect.T
"4. taskbar window rect: {0},{1}-{2},{3} (height {4})" -f $trayRect.L, $trayRect.T, $trayRect.R, $trayRect.B, $trayHeight

$offscreen = 0 - $trayHeight - 8
[void][AppBarProbe.Win]::SetWindowPos($tray, [IntPtr]::Zero, $trayRect.L, $offscreen, 0, 0,
                                      0x0001 -bor 0x0004 -bor 0x0010)   # NOSIZE | NOZORDER | NOACTIVATE
Start-Sleep -Milliseconds 400
"   moved to y=$offscreen; work area: " + (WorkArea)
"   primary    : " + (MonitorWork)

[void][AppBarProbe.Win]::SetWindowPos($tray, [IntPtr]::Zero, $trayRect.L, $trayRect.T, 0, 0,
                                      0x0001 -bor 0x0004 -bor 0x0010)
Start-Sleep -Milliseconds 400
"5. moved back   ; work area: " + (WorkArea)
"   primary    : " + (MonitorWork)

# --- the auto-hide route -------------------------------------------------------------------
#
# Neither the AppBar message nor moving the window moved the work area, which leaves the shell's own
# bookkeeping: it reserves the strip because the taskbar is not set to AUTO-HIDE. ABS_AUTOHIDE is the
# documented state for exactly this, and the state is readable and restorable (ABM_GETSTATE / ABM_SETSTATE),
# which is what makes it usable by a bar that is only running for a while.

$ABS_AUTOHIDE = 1
$current = New-Object AppBarProbe.Win+APPBARDATA
$current.cbSize = 48
$state = [AppBarProbe.Win]::SHAppBarMessage(0x00000006, [ref]$current)   # ABM_GETSTATE
"6. taskbar state now: $state  (1 = autohide, 2 = always on top, 3 = both)"

$setState = New-Object AppBarProbe.Win+APPBARDATA
$setState.cbSize = 48
$setState.lParam = [IntPtr]($state -bor $ABS_AUTOHIDE)
[void][AppBarProbe.Win]::SHAppBarMessage(0x0000000A, [ref]$setState)     # ABM_SETSTATE
Start-Sleep -Milliseconds 600
"   with autohide on; work area: " + (WorkArea)
"   primary       : " + (MonitorWork)

$restore = New-Object AppBarProbe.Win+APPBARDATA
$restore.cbSize = 48
$restore.lParam = [IntPtr]$state
[void][AppBarProbe.Win]::SHAppBarMessage(0x0000000A, [ref]$restore)
Start-Sleep -Milliseconds 600
"7. state restored; work area: " + (WorkArea)
"   primary       : " + (MonitorWork)

# --- telling the shell what the work area IS ------------------------------------------------
#
# Nothing above moved it, so this asks the documented question directly: SPI_SETWORKAREA says what the work
# area is -- and it is what a maximized window obeys. Two things to find out: does it take, and does the
# shell put it back on the next taskbar event (which decides whether a bar would have to re-assert it).

$full = New-Object AppBarProbe.Win+RECT
$full.L = 0; $full.T = 0; $full.R = 2560; $full.B = 1440
[void][AppBarProbe.Win]::SystemParametersInfoSet(0x002F, 0, [ref]$full, 0x0002)   # SPI_SETWORKAREA | SPIF_SENDCHANGE
Start-Sleep -Milliseconds 500
"8. work area set to the whole screen: " + (WorkArea)
"   primary                        : " + (MonitorWork)
Start-Sleep -Seconds 3
"   ... three seconds later        : " + (WorkArea)
"   primary                        : " + (MonitorWork)

$back = New-Object AppBarProbe.Win+RECT
$back.L = 0; $back.T = 32; $back.R = 2560; $back.B = 1440
[void][AppBarProbe.Win]::SystemParametersInfoSet(0x002F, 0, [ref]$back, 0x0002)
Start-Sleep -Milliseconds 500
"9. put back to the 32 DIP strip      : " + (WorkArea)
'done'
