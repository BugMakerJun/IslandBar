# =====================================================================
#  toast-test.ps1 -- raise a REAL Windows toast, for the second channel
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\toast-test.ps1 `
#          -Title "Build finished" -Body "0 errors"
#
#  The bar reads Windows' own toast database, so testing that channel means creating a
#  toast the way an application does -- through the notification platform -- rather than
#  pushing to the bar's own pipe (which is the OTHER channel, and proves nothing about
#  this one).
#
#  Windows only lets an unpackaged process use an AppUserModelID it has registered; the
#  one below is registered on first use by the shell for a shortcut, so the toast may be
#  dropped on a machine where it does not exist. That is reported rather than hidden: a
#  toast that was refused and a reader that failed look identical from the bar.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [string]$Title = 'IslandBar toast test',
    [string]$Body  = 'If this shows up in the island, the system toast channel works.',
    [string]$AppId = 'IslandBar.ToastTest'
)

$ErrorActionPreference = 'Stop'

[void][Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType=WindowsRuntime]
[void][Windows.UI.Notifications.ToastNotification, Windows.UI.Notifications, ContentType=WindowsRuntime]
[void][Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType=WindowsRuntime]

$template = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent(
    [Windows.UI.Notifications.ToastTemplateType]::ToastText02)
$texts = $template.GetElementsByTagName('text')
[void]$texts.Item(0).AppendChild($template.CreateTextNode($Title))
[void]$texts.Item(1).AppendChild($template.CreateTextNode($Body))

$toast = [Windows.UI.Notifications.ToastNotification]::new($template)
$notifier = [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($AppId)
$notifier.Show($toast)

Write-Output "toast shown for app id '$AppId': $Title"
