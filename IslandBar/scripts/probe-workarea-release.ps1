# =====================================================================
#  probe-workarea-release.ps1 -- can SPI_SETWORKAREA give the strip back?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-workarea-release.ps1
#
#  WHY THIS EXISTS
#
#  The bar hides the shell's taskbar, and the work area stays inset by its 32 px, so a
#  maximized window keeps a dead band at the top with our island floating in the middle of it.
#  The user photographed exactly that: "when I maximize an application it should go full screen, not
#  leave this blank band".
#
#  ABM_REMOVE on the shell's taskbar changed nothing, moving the taskbar window off the top
#  changed nothing, and the shell is already in auto-hide (ABM_GETSTATE = 1) while still
#  reserving the strip -- so the reservation looks STALE rather than computed. This asks the
#  documented question directly: does the work area accept a value, and does the value survive?
#
#  The trick that makes the answer readable: set a DISTINCTIVE rectangle. If the read-back is
#  the value that was just set, the call was accepted. If the read-back is the old 0,32, the
#  shell recomputed it from the taskbar. If SPI returns FALSE, the call was refused.
#
#  MUST RUN ON THE INTERACTIVE DESKTOP. A sandboxed shell has its own empty desktop, where
#  FindWindow('Shell_TrayWnd') is 0 and every reading below is taken against nothing.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

Add-Type -Namespace WorkAreaProbe -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll", SetLastError = true)] public static extern bool SystemParametersInfo(uint action, uint param, ref RECT rect, uint flags);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
'@

function WorkArea {
    $rect = New-Object WorkAreaProbe.Win+RECT
    [void][WorkAreaProbe.Win]::SystemParametersInfo(0x0030, 0, [ref]$rect, 0)   # SPI_GETWORKAREA
    return ('{0},{1} - {2},{3}' -f $rect.L, $rect.T, $rect.R, $rect.B)
}

function SetWorkArea([int]$l, [int]$t, [int]$r, [int]$b, [uint32]$flags, [string]$label) {
    $rect = New-Object WorkAreaProbe.Win+RECT
    $rect.L = $l; $rect.T = $t; $rect.R = $r; $rect.B = $b
    $ok = [WorkAreaProbe.Win]::SystemParametersInfo(0x002F, 0, [ref]$rect, $flags)   # SPI_SETWORKAREA
    $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    # Write-Host on purpose: the caller captures the return value, and Write-Output here would
    # end up in that capture instead of on the console.
    Write-Host ("{0}: asked for {1},{2}-{3},{4} flags 0x{5:X}; returned {6} (err {7})" -f $label, $l, $t, $r, $b, $flags, $ok, $err)
    Write-Host ("   immediately  : " + (WorkArea))
    Start-Sleep -Milliseconds 800
    Write-Host ("   +0.8s        : " + (WorkArea))
    Start-Sleep -Seconds 2
    Write-Host ("   +2.8s        : " + (WorkArea))
    return $ok
}

$tray = [WorkAreaProbe.Win]::FindWindow('Shell_TrayWnd', $null)
"Shell_TrayWnd: $tray  visible: " + $(if ($tray -ne [IntPtr]::Zero) { [WorkAreaProbe.Win]::IsWindowVisible($tray) } else { 'n/a' })
if ($tray -eq [IntPtr]::Zero) {
    'STOP: Shell_TrayWnd not found. This probe must run on the interactive desktop (outside the sandbox).'
    exit 2
}

"0. baseline          : " + (WorkArea)

# A rectangle nothing else would produce: if this is what comes back, the value was accepted
# rather than recomputed from the taskbar.
"1. distinctive value, SPIF_UPDATEINIFILE|SPIF_SENDCHANGE (0x3)"
SetWorkArea 0 100 2560 1440 0x3 '   set'
"   after        : " + (WorkArea)

"2. the full screen, flags 0x3"
$full = SetWorkArea 0 0 2560 1440 0x3 '   set'

"3. the full screen, SPIF_SENDCHANGE only (0x2)"
SetWorkArea 0 0 2560 1440 0x2 '   set'

"4. the full screen, no flags (0x0)"
SetWorkArea 0 0 2560 1440 0x0 '   set'

"4b. does it survive the bar's own 5-second check cycle?"
Start-Sleep -Seconds 12
"    +12s         : " + (WorkArea)

"5. whatever is left       : " + (WorkArea)
if ($full) {
    '   (the full-screen call reported success; the shell is what put the strip back)'
} else {
    '   (the full-screen call reported failure; the shell refused the value)'
}

# Leave the machine the way it was found: the shell's own 32 px strip at the top.
$rect = New-Object WorkAreaProbe.Win+RECT
$rect.L = 0; $rect.T = 32; $rect.R = 2560; $rect.B = 1440
[void][WorkAreaProbe.Win]::SystemParametersInfo(0x002F, 0, [ref]$rect, 0x3)
Start-Sleep -Milliseconds 500
"6. restored to 0,32   : " + (WorkArea)
'done'
