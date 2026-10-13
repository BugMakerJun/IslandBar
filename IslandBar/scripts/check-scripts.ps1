# =====================================================================
#  check-scripts.ps1 -- every .ps1 in this folder parses, and is ASCII-only
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\check-scripts.ps1
#
#  WHY THIS EXISTS
#
#  The scripts in this folder must stay pure ASCII: a non-ASCII character turns into mojibake the
#  moment a script is read or written with the wrong code page, and the failure is not "a strange
#  character on screen" -- it is a script that no longer parses, or a literal that no longer
#  matches. That rule is easy to state and easy to break with one copy-paste, so it is checked.
#
#  Exit code = the number of files that failed either check.
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'

$folder = Split-Path -Parent $PSCommandPath
$files = Get-ChildItem -Path $folder -Filter *.ps1 | Sort-Object Name

$failed = 0
foreach ($file in $files) {
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$null, [ref]$errors)

    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)

    # A UTF-8 BOM is allowed and is not mojibake: PowerShell reads it correctly, and one file here
    # (snapshot.ps1) has carried one for a long time. Everything after it must still be ASCII.
    $start = 0
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $start = 3
    }

    $nonAscii = 0
    for ($i = $start; $i -lt $bytes.Length; $i++) { if ($bytes[$i] -gt 127) { $nonAscii++ } }

    $problems = @()
    if ($errors -and $errors.Count -gt 0) { $problems += "parse: $($errors[0].Message)" }
    if ($nonAscii -gt 0) { $problems += "non-ASCII: $nonAscii byte(s)" }

    if ($problems.Count -eq 0) {
        "  ok   $($file.Name)"
    } else {
        "  FAIL $($file.Name) -- " + ($problems -join '; ')
        $failed++
    }
}

"$($files.Count) script(s) checked, $failed with problems"
exit $failed
