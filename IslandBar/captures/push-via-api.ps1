# =====================================================================
#  push-via-api.ps1 -- push the local commits through the GitHub API
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\captures\push-via-api.ps1
#
#  WHY THIS EXISTS
#
#  github.com:443 is unreachable from this machine (the DNS answer times out, and
#  forcing a different GitHub address gets the connection reset), so `git push`
#  cannot run here at all -- while api.github.com answers normally. The API has no
#  receive-pack endpoint, so a push is recreated the long way round: every blob,
#  every tree and every commit is uploaded as an object, and then the branch is
#  moved to the new commit.
#
#  WHAT MAKES IT SAFE TO RUN
#
#  Git object hashes are a function of their content, so if the objects uploaded are
#  byte-identical to the local ones, the returned hashes ARE the local hashes: the
#  same commits appear on the remote, not copies of them. Every hash is asserted
#  against the local one, and the branch is only moved once every commit has been
#  recreated exactly. A mismatch stops the run, leaving the uploaded objects
#  unreferenced -- GitHub collects those.
#
#  The bytes of each file come from git itself (`git cat-file blob`), never from the
#  working tree: core.autocrlf is on here, so the files on disk have CRLF where the
#  committed blobs have LF, and uploading what is on disk would push a different
#  repository than the one that was tested.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [string]$Owner  = 'BugMakerJun',
    [string]$Repo   = 'IslandBar-DynamicIsland',
    [string]$Branch = 'main',
    [string]$Remote = 'origin',
    [string]$Gh     = 'C:\Users\18340\gh-cli\bin\gh.exe',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$Work = Join-Path $Root 'captures\api-push'
New-Item -ItemType Directory -Force -Path $Work | Out-Null

function Invoke-Git {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$GitArgs)
    $out = & git -C $Root @GitArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed: $out" }
    return $out
}

# Byte-exact, because a PowerShell pipeline is not: the child's stdout stream is
# copied as bytes rather than as text.
function Get-BlobBytes {
    param([string]$Sha)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'git'
    $psi.WorkingDirectory = $Root
    $psi.Arguments = "cat-file blob $Sha"
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stream = New-Object System.IO.MemoryStream
    $proc.StandardOutput.BaseStream.CopyTo($stream)
    $proc.WaitForExit()
    $bytes = $stream.ToArray()
    $stream.Dispose()
    if ($proc.ExitCode -ne 0) { throw "git cat-file blob $Sha failed" }

    # The hash of "blob <length>\0<content>" IS the object's name. Checking it here
    # means the plumbing above is proven before anything is uploaded with it.
    $header = [System.Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
    $all = New-Object byte[] ($header.Length + $bytes.Length)
    [Array]::Copy($header, 0, $all, 0, $header.Length)
    [Array]::Copy($bytes, 0, $all, $header.Length, $bytes.Length)
    $sha1 = [System.Security.Cryptography.SHA1]::Create()
    $hashed = ($sha1.ComputeHash($all) | ForEach-Object { $_.ToString('x2') }) -join ''
    if ($hashed -ne $Sha) { throw "the bytes of $Sha hash to $hashed" }
    return $bytes
}

function Invoke-Api {
    param([string]$Method, [string]$Path, [object]$Body)
    $cli = @('api', '--method', $Method, $Path)
    $jsonPath = $null
    if ($Body) {
        $jsonPath = Join-Path $Work ('body-' + [guid]::NewGuid().ToString('N') + '.json')
        $json = $Body | ConvertTo-Json -Depth 12 -Compress
        # No BOM: gh parses this file as JSON, and a byte-order mark breaks that.
        [System.IO.File]::WriteAllText($jsonPath, $json, (New-Object System.Text.UTF8Encoding($false)))
        $cli += @('--input', $jsonPath)
    }
    $text = & $Gh @cli 2>&1
    $code = $LASTEXITCODE
    if ($jsonPath) { Remove-Item -LiteralPath $jsonPath -Force -ErrorAction SilentlyContinue }
    if ($code -ne 0) { throw "gh api $Method $Path failed: $text" }
    return ($text -join "`n")
}

function ConvertTo-IsoDate {
    param([string]$Stamp, [string]$Offset)
    $sign = if ($Offset[0] -eq '-') { -1 } else { 1 }
    $hours = [int]$Offset.Substring(1, 2)
    $minutes = [int]$Offset.Substring(3, 2)
    $utc = [DateTimeOffset]::FromUnixTimeSeconds([long]$Stamp).UtcDateTime
    $local = $utc.AddMinutes($sign * ($hours * 60 + $minutes))
    return $local.ToString('yyyy-MM-ddTHH:mm:ss') + $Offset.Insert(3, ':')
}

function Get-Identity {
    param([string]$Line)
    $m = [regex]::Match($Line, '^(\w+) (.*) <(.*)> (\d+) ([+-]\d{4})$')
    if (-not $m.Success) { throw "cannot read an identity from: $Line" }
    return @{
        name  = $m.Groups[2].Value
        email = $m.Groups[3].Value
        date  = (ConvertTo-IsoDate $m.Groups[4].Value $m.Groups[5].Value)
    }
}

# PowerShell unwraps a one-element array from a function's output into a bare string, and
# indexing THAT gives a character -- so every use of a git command's output here is forced
# back into an array first. (`(git rev-parse HEAD)[0]` was "5", the first character of the
# hash, which is exactly the kind of thing that silently pushes the wrong commit.)
# --- the remote's current state ---------------------------------------
$ref = Invoke-Api -Method GET -Path "repos/$Owner/$Repo/git/ref/heads/$Branch" | ConvertFrom-Json
$remoteHead = $ref.object.sha
$localHead = @(Invoke-Git rev-parse HEAD)[0]
Write-Host "[api-push] remote $Branch = $remoteHead"
Write-Host "[api-push] local  HEAD    = $localHead"
if ($remoteHead -eq $localHead) { Write-Host '[api-push] already in sync'; return }

& git -C $Root merge-base --is-ancestor $remoteHead HEAD
if ($LASTEXITCODE -ne 0) {
    throw "$remoteHead is not an ancestor of HEAD: this is not a fast-forward, so nothing was sent"
}

$commits = @(Invoke-Git rev-list --reverse "$remoteHead..HEAD")
Write-Host "[api-push] commits to recreate: $($commits.Count)"

$parent = $remoteHead
foreach ($commit in $commits) {
    $raw = @(Invoke-Git cat-file commit $commit)
    $treeLine = @($raw | Where-Object { $_ -like 'tree *' })[0].Substring(5)
    $authorLine = @($raw | Where-Object { $_ -like 'author *' })[0]
    $committerLine = @($raw | Where-Object { $_ -like 'committer *' })[0]
    $blank = [Array]::IndexOf($raw, '')
    # The message keeps its own newlines, and git's stored message ends with one.
    $message = (($raw[($blank + 1)..($raw.Count - 1)]) -join "`n") + "`n"
    $author = Get-Identity $authorLine
    $committer = Get-Identity $committerLine

    # --- the blobs -----------------------------------------------------
    # --no-renames, and it is not a detail: with rename detection on (the default), a rename comes back as
    # ONE line -- the new path -- and the old path is never named, so the parent's copy of it was never
    # dropped from the rebuilt tree. Measured on 35aea99, which renamed a review file: reviews/ came back
    # from the API with a different hash, and the file that stayed behind was the raw transcript.
    $changed = @(Invoke-Git diff --no-renames --name-only $parent $commit)
    $blobs = @{}
    $deleted = @{}
    foreach ($path in $changed) {
        # A DELETED path has no blob in this commit, and `diff --name-only` names it all the same. Asking
        # for "$commit:$path" then fails outright ("path 'docs/TRAY-RESEARCH-RETIRED.md' does not exist in
        # <commit>") -- which is how the commit that deleted that file stopped the push. The absence IS the
        # change: nothing to upload, and the entry is simply missing from the rebuilt tree below.
        #
        # `--quiet` with stderr dropped for the same reason the tree probe below does it: a native
        # command's stderr becomes a TERMINATING error under this script's $ErrorActionPreference = 'Stop'
        # before the exit code is ever read.
        $probe = @(& git -C $Root rev-parse --verify --quiet "$($commit):$path" 2>$null)
        if ($LASTEXITCODE -ne 0 -or -not $probe) {
            $deleted[$path] = $true
            Write-Host ("[api-push]   deleted {0}" -f $path)
            continue
        }
        $sha = @($probe)[0]
        $bytes = Get-BlobBytes $sha
        $body = @{ content = [Convert]::ToBase64String($bytes); encoding = 'base64' }
        $uploaded = (Invoke-Api -Method POST -Path "repos/$Owner/$Repo/git/blobs" -Body $body | ConvertFrom-Json).sha
        if ($uploaded -ne $sha) { throw "blob '$path' uploaded as $uploaded, expected $sha" }
        $blobs[$path] = $sha
        Write-Host ("[api-push]   blob {0,-44} {1}" -f $path, $sha)
    }

    # --- the trees, deepest directory first ----------------------------
    # Every directory that contains a change, and every ancestor of those up to the root.
    $dirs = @{}
    foreach ($path in $changed) {
        $parts = $path -split '/'
        for ($i = 1; $i -lt $parts.Count; $i++) { $dirs[($parts[0..($i - 1)] -join '/')] = $true }
        $dirs[''] = $true
    }
    # DEEPEST FIRST, and the root strictly last: a directory's tree is built from the trees of the
    # directories under it, so building the root before "IslandBar" would put a null in where the new
    # IslandBar subtree belongs. (Sorting by "the negative number of parts" made the root and the
    # top-level directories tie, and the root went first -- caught by the hash assertion below.)
    $ordered = @(
        $dirs.Keys | ForEach-Object {
            $d = $_
            $depth = if ($d -eq '') { 0 } else { ($d -split '/').Count }
            [pscustomobject]@{ Dir = $d; Depth = $depth }
        } | Sort-Object -Property Depth -Descending | ForEach-Object { $_.Dir }
    )
    Write-Host "[api-push]   directories, deepest first: $(($ordered | ForEach-Object { if ($_ -eq '') { '<root>' } else { $_ } }) -join ' ')"
    $newTrees = @{}
    $goneDirs = @{}
    foreach ($dir in $ordered) {
        # A DELETED DIRECTORY is the file case one level up: a commit that removes "docs/" has no tree to
        # build for it, and it has to be simply missing from its parent's rebuilt tree. Asking for
        # "$commit:docs" is what told us so ("exists on disk, but not in <commit>") -- the second stop.
        $exists = $null
        if ($dir -ne '') {
            $probe = @(& git -C $Root rev-parse --verify --quiet "$($commit):$dir" 2>$null)
            if ($LASTEXITCODE -ne 0 -or -not $probe) {
                $goneDirs[$dir] = $true
                Write-Host ("[api-push]   directory {0} is gone in this commit" -f $dir)
                continue
            }
            $exists = @($probe)[0]
        }
        $prefix = if ($dir -eq '') { '' } else { "$dir/" }
        $entries = New-Object System.Collections.ArrayList

        # The files directly in this directory that changed ...
        foreach ($path in $changed) {
            if (-not $path.StartsWith($prefix)) { continue }
            $rest = $path.Substring($prefix.Length)
            if ($rest.Contains('/')) { continue }
            # A DELETED path is not "no entry": base_tree ADDS what it is given and can only be told to drop
            # something by naming it with a null sha. Leaving the entry out put the parent's copy straight
            # back into the rebuilt tree, and the hash assertion caught it.
            if ($deleted.ContainsKey($path)) {
                [void]$entries.Add(@{ path = $rest; mode = '100644'; type = 'blob'; sha = $null })
                continue
            }
            [void]$entries.Add(@{ path = $rest; mode = '100644'; type = 'blob'; sha = $blobs[$path] })
        }
        # ... and the subdirectories that had to be rewritten.
        foreach ($sub in $ordered) {
            if ($sub -eq '' -or -not $sub.StartsWith($prefix)) { continue }
            $rest = $sub.Substring($prefix.Length)
            if ($rest -eq '' -or $rest.Contains('/')) { continue }
            if ($goneDirs.ContainsKey($sub)) {                # deleted in this commit: drop the entry
                [void]$entries.Add(@{ path = $rest; mode = '040000'; type = 'tree'; sha = $null })
                continue
            }
            if (-not $newTrees[$sub]) { throw "subtree '$sub' was never built; the ordering is wrong" }
            [void]$entries.Add(@{ path = $rest; mode = '040000'; type = 'tree'; sha = $newTrees[$sub] })
        }

        # The base tree is the parent's version of this directory -- or NOTHING, when the commit being
        # pushed is the one that CREATES it.
        #
        # The probe is `--quiet` and its stderr goes to $null, and both halves of that matter: a native
        # command's stderr is turned into a TERMINATING error by this script's own
        # $ErrorActionPreference = 'Stop' before the exit code is ever looked at, so the honest answer
        # "this directory is not in that commit" arrived as "path 'IslandBar/setup' exists on disk, but
        # not in <parent>" and stopped the push twice.
        $base = $null
        if ($dir -eq '') {
            $base = @(Invoke-Git rev-parse "$($parent)^{tree}")[0]
        } else {
            $probe = & git -C $Root rev-parse --verify --quiet "$($parent):$dir" 2>$null
            if ($LASTEXITCODE -eq 0 -and $probe) { $base = @($probe)[0] }
        }
        $expected = if ($dir -eq '') { @(Invoke-Git rev-parse "$($commit)^{tree}")[0] }
                    else { $exists }
        $body = @{ tree = $entries }
        if ($base) { $body.base_tree = $base }
        $made = (Invoke-Api -Method POST -Path "repos/$Owner/$Repo/git/trees" -Body $body |
                 ConvertFrom-Json).sha
        if ($made -ne $expected) { throw "tree '$dir' came back as $made, expected $expected" }
        $newTrees[$dir] = $made
        $label = if ($dir -eq '') { '<root>' } else { $dir }
        Write-Host ("[api-push]   tree {0,-44} {1}" -f $label, $made)
    }
    if ($newTrees[''] -ne $treeLine) { throw "the root tree is $($newTrees['']), expected $treeLine" }

    # --- the commit ----------------------------------------------------
    $madeCommit = (Invoke-Api -Method POST -Path "repos/$Owner/$Repo/git/commits" -Body @{
        message   = $message
        tree      = $treeLine
        parents   = @($parent)
        author    = $author
        committer = $committer
    } | ConvertFrom-Json).sha
    if ($madeCommit -ne $commit) {
        throw "the commit came back as $madeCommit, expected $commit (the message or an identity differs)"
    }
    Write-Host "[api-push] commit $commit recreated exactly"
    $parent = $commit
}

# --- move the branch ---------------------------------------------------
if ($DryRun) { Write-Host '[api-push] -DryRun: the branch was NOT moved'; return }
$result = Invoke-Api -Method PATCH -Path "repos/$Owner/$Repo/git/refs/heads/$Branch" `
                    -Body @{ sha = $localHead; force = $false } | ConvertFrom-Json
Write-Host "[api-push] $Branch is now $($result.object.sha)"
if ($result.object.sha -ne $localHead) { throw 'the branch did not land on the local HEAD' }

# The remote-tracking ref is what `git status` reports, and a fetch cannot run while
# github.com is unreachable, so it is updated here instead of by fetching.
Invoke-Git update-ref "refs/remotes/$Remote/$Branch" $localHead | Out-Null
Write-Host "[api-push] $Remote/$Branch updated locally to match"
