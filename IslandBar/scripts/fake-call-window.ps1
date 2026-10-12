# =====================================================================
#  fake-call-window.ps1 -- a WeChat CALL, without a phone.
#
#      $barDir = Join-Path $env:USERPROFILE 'IslandBar'          # the folder the bar RUNS from, outside
#      Copy-Item C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe `
#                (Join-Path $barDir 'WeixinCallProbe.exe') -Force
#      & (Join-Path $barDir 'WeixinCallProbe.exe') -NoProfile -ExecutionPolicy Bypass `
#            -File .\scripts\fake-call-window.ps1
#
#  WHY THIS EXISTS: the call strip (avatar + green timer + level meter) can only be reached by a window that
#  WeChat owns and titles "微信音视频通话", so "the call does not show" cannot be reproduced or verified from
#  a script -- the whole path would stay untested until the next real call. `WeChatWatcher` decides that by
#  the EXECUTABLE's name (any process whose image starts with "Weixin"/"WeChat") plus a visible window whose
#  title holds "音视频通话", so a copy of powershell.exe under a Weixin* name with a form titled that way is
#  exactly what the watcher is looking for.
#
#  TWO PROPERTIES MATTER, and both are chosen for the rules in WeChatWatch.cpp:
#   * the window is parked at (-4000,-4000): `IsWindowVisible` is a STYLE, not a position, so the watcher
#     sees a visible window while nobody has to look at it.
#   * it is 740x360, deliberately OUTSIDE the toast window's size range (140..720 x 48..340) so it cannot be
#     mistaken for a message popup -- otherwise the same window would announce "有新消息" as well.
#
#  ASCII-only on purpose: this file must not carry non-ASCII bytes (see HANDOFF 6.1). The title is built from
#  code points instead: U+5FAE U+4FE1 U+97F3 U+89C6 U+9891 U+901A U+8BDD = "微信音视频通话".
# =====================================================================

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# The console is hidden from INSIDE rather than with `Start-Process -WindowStyle Hidden`, and that is not a
# detail: -WindowStyle Hidden sets STARTF_USESHOWWINDOW/SW_HIDE for the process, which makes the FIRST
# ShowWindow call -- the form's -- create a window that is NEVER VISIBLE (`IsWindowVisible` stays 0, measured:
# `'微信音视频通话' (-4000,-4000) 740x360 visible=0`). The watcher requires a visible window, so the probe was
# silently ignored and the "call" it was meant to inject never happened.
Add-Type -Namespace CallProbe -Name Win -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr handle, int command);
'@
$console = [CallProbe.Win]::GetConsoleWindow()
if ($console -ne [IntPtr]::Zero) {
    [void][CallProbe.Win]::ShowWindow($console, 0)   # SW_HIDE
}

$call = [string]::Concat([char]0x5FAE, [char]0x4FE1, [char]0x97F3, [char]0x89C6, [char]0x9891,
                         [char]0x901A, [char]0x8BDD)

$form = New-Object System.Windows.Forms.Form
$form.Text = $call
$form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point(-4000, -4000)
$form.Size = New-Object System.Drawing.Size(740, 360)
[void]$form.ShowDialog()
