$ErrorActionPreference = 'Continue'

# =====================================================================
#  Does WeChat's call appear as a GSMTC session?
#
#  This decides the whole shape of the feature. If WeChat publishes a
#  Communication session with a Position, then the C++/WinRT media path the bar
#  ALREADY uses can read the call and its elapsed time, and the window probe is
#  only a fallback. If it does not, the call can only be detected from the
#  window, and the elapsed time has to be counted locally from first sight.
#
#  Read-only. Uses the WinRT metadata directly (no PowerShell projection, which
#  this machine's PowerShell 5.1 cannot load).
# =====================================================================

$out = New-Object System.Collections.ArrayList

try {
    [void][Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
    $mgrType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]

    # RequestAsync() returns IAsyncOperation; block on it through the WinRT
    # helper rather than the C# async bridge, which PS 5.1 lacks.
    $asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    })
    if (-not $asTaskGeneric) { throw "WindowsRuntimeSystemExtensions.AsTask not available" }

    $op = $mgrType::RequestAsync()
    $asTask = $asTaskGeneric[0].MakeGenericMethod($mgrType)
    $task = $asTask.Invoke($null, @($op))
    [void]$task.Wait(5000)
    $mgr = $task.Result

    $sessions = $mgr.GetSessions()
    Write-Output ("GSMTC sessions: " + $sessions.Count)
    foreach ($s in $sessions) {
        [void]$out.Add($s)
    }
} catch {
    Write-Output ("  WinRT path failed: " + $_.Exception.Message)
    Write-Output "  falling back to the C++ probe instead"
    exit 2
}

foreach ($s in $out) {
    $appId = $s.SourceAppUserModelId
    Write-Output ""
    Write-Output ("  appId : " + $appId)
    try {
        $props = $s.TryGetMediaPropertiesAsync().GetResults()
        Write-Output ("    PlaybackType : " + $props.PlaybackType)
        Write-Output ("    Title        : " + $props.Title)
        Write-Output ("    Artist       : " + $props.Artist)
        Write-Output ("    AlbumTitle   : " + $props.AlbumTitle)
        $th = $props.Thumbnail
        Write-Output ("    Thumbnail    : " + $(if ($th) { "present" } else { "none" }))
    } catch { Write-Output ("    media properties failed: " + $_.Exception.Message) }
    try {
        $info = $s.GetPlaybackInfo()
        Write-Output ("    Status       : " + $info.PlaybackStatus)
    } catch { }
    try {
        $tl = $s.GetTimelineProperties()
        Write-Output ("    Position     : " + $tl.Position + "   End: " + $tl.EndTime + "   Start: " + $tl.StartTime)
    } catch { }
}
