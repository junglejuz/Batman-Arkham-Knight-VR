<#
  AKVR installer: sets up the Batman: Arkham Knight geo-11 3D fix for VR and installs the mod.

  Double-click Install-AKVR.bat. It finds the game and your downloads by itself, backs up
  anything it replaces, and asks if it cannot find something. See the README, "Installing".

  Optional, to skip the searching:
    -GameDir "<folder with BatmanAK.exe>" -FixArchive "<fix .7z>" -ModDll "<dinput8.dll>"
#>
param([string]$GameDir, [string]$FixArchive, [string]$ModDll)

$Game = @{
    Mod          = 'AKVR'
    Name         = 'Batman: Arkham Knight'
    Exe          = 'BatmanAK.exe'
    SteamFolder  = 'Batman Arkham Knight\Binaries\Win64'                      # under steamapps\common
    FixMatch     = 'arkham.*knight.*geo-?11.*\.(7z|zip)$'   # the fix's file name
    FixPage      = 'https://helixmod.blogspot.com/2020/12/batman-arkham-knight-dx11.html'
    AddNonSquare = $true                         # keep 3D on the tall VR picture (FIX_CHANGES.md section 3)
    FixPatches   = 'AKVR-fix-patches.ps1'       # the mod's HUD edits to the fix (FIX_CHANGES.md sections 1-2)
    OldProxy     = 'version.dll'
    BuildDirs    = @('build-dinput8', 'build')
}

# ---------------------------------------------------------------------------------------------
# Everything below is the same for every game. The settings for this game are in $Game above.
# ---------------------------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms

function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Step([string]$text) { Write-Host ''; Write-Host "  $text" -ForegroundColor Cyan }
function Fail([string]$text) { Write-Host ''; Write-Host "  STOPPED: $text" -ForegroundColor Red; Write-Host ''; exit 1 }

# ---- finding things -------------------------------------------------------------------------

function Get-DownloadsDir {
    try {
        $p = (New-Object -ComObject Shell.Application).NameSpace('shell:Downloads').Self.Path
        if ($p -and (Test-Path $p)) { return $p }
    } catch { }
    return (Join-Path $env:USERPROFILE 'Downloads')
}

function New-TopForm { $f = New-Object System.Windows.Forms.Form; $f.TopMost = $true; return $f }

function Select-Folder([string]$description) {
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = $description
    if ($d.ShowDialog((New-TopForm)) -eq [System.Windows.Forms.DialogResult]::OK) { return $d.SelectedPath }
    return $null
}

function Select-File([string]$title) {
    $d = New-Object System.Windows.Forms.OpenFileDialog
    $d.Title = $title
    $d.Filter = 'Archives (*.7z;*.zip)|*.7z;*.zip|All files (*.*)|*.*'
    $d.InitialDirectory = Get-DownloadsDir
    if ($d.ShowDialog((New-TopForm)) -eq [System.Windows.Forms.DialogResult]::OK) { return $d.FileName }
    return $null
}

function Find-SteamGameDir {
    $roots = @()
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $p = Get-ItemProperty $key -ErrorAction Stop
            if ($p.SteamPath)   { $roots += ($p.SteamPath -replace '/', '\') }
            if ($p.InstallPath) { $roots += $p.InstallPath }
        } catch { }
    }
    $libraries = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libraries += $r
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }
    foreach ($lib in ($libraries | Select-Object -Unique)) {
        $dir = Join-Path $lib ('steamapps\common\' + $Game.SteamFolder)
        if (Test-Path (Join-Path $dir $Game.Exe)) { return $dir }
    }
    return $null
}

# Where people put downloads: next to this script, the one or two folders above it (unzipping
# makes a folder inside Downloads, sometimes a folder inside that), and the Downloads folder.
function Find-Download([string]$pattern) {
    $up1 = Split-Path $PSScriptRoot -Parent
    $up2 = if ($up1) { Split-Path $up1 -Parent } else { $null }
    $dirs = @($PSScriptRoot, $up1, $up2, (Get-DownloadsDir)) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique
    return Get-ChildItem -Path $dirs -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $pattern } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

# ---- unpacking --------------------------------------------------------------------------------

function Expand-Any([string]$archive, [string]$dest) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        # Windows 11's built-in tar opens .7z and .zip.
        $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
        if (Test-Path $tar) {
            & $tar -xf $archive -C $dest 2>&1 | Out-Null
            if ($LASTEXITCODE -eq 0) { return }
        }
        # Older Windows: fall back to 7-Zip if it is installed.
        $sevenZip = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($sevenZip) {
            & $sevenZip x -y "-o$dest" $archive 2>&1 | Out-Null
            if ($LASTEXITCODE -eq 0) { return }
        }
    } finally { $ErrorActionPreference = $old }
    Fail "could not unpack $(Split-Path $archive -Leaf). Install 7-Zip (https://www.7-zip.org) and run this again."
}

# The fixes come as an archive holding another archive (FixFiles.7z). Unpack until we reach the
# folder that has d3dx.ini in it.
function Get-FixRoot([string]$dir, [int]$depth = 0) {
    if (Test-Path (Join-Path $dir 'd3dx.ini')) { return $dir }
    if ($depth -gt 3) { return $null }
    $inner = Get-ChildItem $dir -File | Where-Object { $_.Extension -in '.7z', '.zip' }
    $n = 0
    foreach ($a in $inner) {
        $n++
        $out = Join-Path $dir ("_inner$n")
        Expand-Any $a.FullName $out
        $r = Get-FixRoot $out ($depth + 1)
        if ($r) { return $r }
    }
    foreach ($sub in (Get-ChildItem $dir -Directory)) {
        $r = Get-FixRoot $sub.FullName ($depth + 1)
        if ($r) { return $r }
    }
    return $null
}

# ---- ini editing (keeps the file's own bytes, line endings and lack of a BOM) ---------------

$Latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Read-Ini([string]$path) {
    $text = [System.IO.File]::ReadAllText($path, $Latin1)
    $nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $list = New-Object 'System.Collections.Generic.List[string]'
    foreach ($l in ($text -split "`r?`n")) { $list.Add($l) }
    return @{ Path = $path; Lines = $list; NL = $nl }
}

function Save-Ini($doc) {
    [System.IO.File]::WriteAllText($doc.Path, ($doc.Lines -join $doc.NL), $Latin1)
}

# Sets every active "key = ..." line. If there is none, adds one under [section].
function Set-IniValue($doc, [string]$key, [string]$value, [string]$section) {
    $rx = '^\s*' + [regex]::Escape($key) + '\s*='
    $hit = $false
    for ($i = 0; $i -lt $doc.Lines.Count; $i++) {
        if ($doc.Lines[$i] -match $rx) { $doc.Lines[$i] = "$key = $value"; $hit = $true }
    }
    if ($hit) { return }
    if (-not $section) { Fail "$(Split-Path $doc.Path -Leaf) has no '$key' line. Is this the right fix?" }
    $srx = '^\s*\[' + [regex]::Escape($section) + '\]\s*$'
    for ($i = 0; $i -lt $doc.Lines.Count; $i++) {
        if ($doc.Lines[$i] -match $srx) { $doc.Lines.Insert($i + 1, "$key = $value"); return }
    }
    Fail "$(Split-Path $doc.Path -Leaf) has no [$section] section. Is this the right fix?"
}

function Add-IniBlock($doc, [string[]]$block) {
    while ($doc.Lines.Count -gt 0 -and $doc.Lines[$doc.Lines.Count - 1].Trim() -eq '') { $doc.Lines.RemoveAt($doc.Lines.Count - 1) }
    $doc.Lines.Add('')
    foreach ($l in $block) { $doc.Lines.Add($l) }
    $doc.Lines.Add('')
}

# =============================================================================================

Write-Host ''
Write-Host "  $($Game.Mod) installer - $($Game.Name) in VR" -ForegroundColor White
Write-Host '  ------------------------------------------------------------' -ForegroundColor DarkGray

# ---- 1. the game folder -------------------------------------------------------------------

Step '1. Finding the game'
if (-not $GameDir) { $GameDir = Find-SteamGameDir }
if (-not $GameDir) {
    Say "   Could not find $($Game.Name) in your Steam libraries."
    Say "   Please pick the folder that contains $($Game.Exe)."
    $GameDir = Select-Folder "Pick the $($Game.Name) folder that contains $($Game.Exe)"
}
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir $Game.Exe))) {
    Fail "$($Game.Exe) is not in that folder. It must be the folder that contains $($Game.Exe)."
}
$GameDir = (Resolve-Path $GameDir).Path
Say "   $GameDir" 'Green'

$proc = Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($Game.Exe)) -ErrorAction SilentlyContinue
if ($proc) { Fail "the game is running. Close it and run this again." }

# ---- 2. the downloads ---------------------------------------------------------------------

Step '2. Finding your downloads'
if (-not $FixArchive) { $f = Find-Download $Game.FixMatch; if ($f) { $FixArchive = $f.FullName } }
if (-not $FixArchive) {
    Say "   Could not find the $($Game.Name) geo-11 fix in your Downloads folder."
    Say "   Please pick the file you downloaded from $($Game.FixPage)"
    $FixArchive = Select-File "Pick the $($Game.Name) geo-11 fix you downloaded"
}
if (-not $FixArchive -or -not (Test-Path $FixArchive)) { Fail "no 3D fix to install. Download it from $($Game.FixPage)" }
Say "   3D fix: $FixArchive" 'Green'

$files = Join-Path $PSScriptRoot 'files'
foreach ($need in 'd3d11.dll', 'nvapi64.dll', 'd3dxdm.ini') {
    if (-not (Test-Path (Join-Path $files $need))) { Fail "the 'files' folder next to this script is missing $need. Download the whole install folder again." }
}
Say "   geo-11 $((Get-Item (Join-Path $files 'd3d11.dll')).VersionInfo.FileVersion) and the VR settings: $files" 'Green'

if (-not $ModDll) {
    $candidates = @(Join-Path $PSScriptRoot 'dinput8.dll')
    foreach ($b in $Game.BuildDirs) { $candidates += (Join-Path $PSScriptRoot "..\$b\Release\dinput8.dll") }
    $ModDll = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if ($ModDll) { $ModDll = (Resolve-Path $ModDll).Path; Say "   mod: $ModDll" 'Green' }
else { Say "   mod: dinput8.dll not found next to this script - the 3D fix will be set up, but copy the mod in afterwards." 'Yellow' }

# ---- 3. unpack ----------------------------------------------------------------------------

Step '3. Unpacking'
$work = Join-Path $env:TEMP ("$($Game.Mod)-install-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
Expand-Any $FixArchive (Join-Path $work 'fix')
$fixRoot = Get-FixRoot (Join-Path $work 'fix')
if (-not $fixRoot -or -not (Test-Path (Join-Path $fixRoot 'd3d11.dll'))) {
    Fail "that download does not look like a geo-11 fix (no d3dx.ini / d3d11.dll inside). Make sure you took the geo-11 version, not the 3D Vision one."
}
$fixCount = (Get-ChildItem $fixRoot -Recurse -File).Count
Say "   3D fix: $fixCount files"

# ---- 4. back up what is there now ----------------------------------------------------------

Step '4. Backing up your current files'
$backup = Join-Path $GameDir ('vrmod_backup_' + (Get-Date -Format 'yyyy-MM-dd_HHmmss'))
New-Item -ItemType Directory -Force -Path $backup | Out-Null
$names = @(Get-ChildItem $fixRoot -File | ForEach-Object { $_.Name }) +
         @('geo11.dll', 'dinput8.dll', 'dxgi.dll', 'dxgi.dll.wrapmode', 'd3dx_user.ini', 'version.dll')
$saved = 0
foreach ($n in ($names | Select-Object -Unique)) {
    $p = Join-Path $GameDir $n
    if (Test-Path $p -PathType Leaf) { Copy-Item $p (Join-Path $backup $n); $saved++ }
}
foreach ($d in (Get-ChildItem $fixRoot -Directory)) {
    $p = Join-Path $GameDir $d.Name
    if (Test-Path $p -PathType Container) {
        & robocopy $p (Join-Path $backup $d.Name) /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
        $saved++
    }
}
if ($saved -eq 0) { Remove-Item $backup; Say '   nothing to back up (fresh install)' }
else { Say "   saved in $backup" 'Green' }

# ---- 5. install the 3D fix ----------------------------------------------------------------

Step '5. Installing the 3D fix'
& robocopy $fixRoot $GameDir /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Fail "could not copy the fix into the game folder. Try right-clicking the .bat and 'Run as administrator'." }
Say "   copied into the game folder" 'Green'

# Our tested geo-11 (the fix may ship an older or newer one) and its settings for VR:
# output to the mod, world scale 1.00, automatic depth off. See files\README.txt.
foreach ($f in 'd3d11.dll', 'nvapi64.dll', 'd3dxdm.ini') { Copy-Item (Join-Path $files $f) (Join-Path $GameDir $f) -Force }
Say "   geo-11 $((Get-Item (Join-Path $files 'd3d11.dll')).VersionInfo.FileVersion) and its VR settings copied in" 'Green'

# ---- 6. set geo-11 up for the headset -----------------------------------------------------

Step '6. Setting geo-11 up for VR'

# geo-11 normally runs as the game's d3d11.dll. For VR it must be loaded by the mod instead,
# under the name geo11.dll, so Virtual Desktop can still find the headset.
$d3d11 = Join-Path $GameDir 'd3d11.dll'
$alias = Join-Path $GameDir 'geo11.dll'
if (Test-Path $alias) { Remove-Item $alias -Force }
Rename-Item $d3d11 'geo11.dll'
Say '   geo-11 renamed to geo11.dll (the mod loads it)' 'Green'

# The fix's own dxgi.dll is a second geo-11 piece that VR does not use. Park it.
if (Test-Path (Join-Path $fixRoot 'dxgi.dll')) {
    Move-Item (Join-Path $GameDir 'dxgi.dll') (Join-Path $GameDir 'dxgi.dll.wrapmode') -Force
    Say '   the fix''s dxgi.dll set aside (dxgi.dll.wrapmode)' 'Green'
}

$ini = Read-Ini (Join-Path $GameDir 'd3dx.ini')
Set-IniValue $ini 'load_library_redirect' '0' $null
if ($Game.AddNonSquare -and -not ($ini.Lines | Where-Object { $_ -match '^\s*match_width\s*=\s*!height' })) {
    Add-IniBlock $ini @(
        "; $($Game.Mod): keep 3D on pictures that are not 16:9 (the VR picture is taller)",
        '[TextureOverrideAllNonSquareRT]',
        'match_type = Texture2D',
        'match_bind_flags = +render_target',
        'match_width = !height',
        'StereoMode = 1')
}
Save-Ini $ini
Say '   d3dx.ini updated' 'Green'

# The mod's edits to the fix's own HUD shaders (HUD distance, steady HUD layer, reticles at scene depth).
# They are made on the player's copy of the fix, never shipped. Record: FIX_CHANGES.md.
if ($Game.FixPatches) {
    Step '6b. Adjusting the 3D fix''s HUD for VR'
    # The project's master copy (akvr\tools) when run from the project, else the copy shipped beside this script.
    $patch = @((Join-Path $PSScriptRoot "..\tools\$($Game.FixPatches)"), (Join-Path $PSScriptRoot $Game.FixPatches)) |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $patch) { Fail "$($Game.FixPatches) is missing next to this script. Download the whole install folder again." }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $patch -Mode apply -GameDir $GameDir
    if ($LASTEXITCODE -ne 0) {
        Fail "some HUD edits could not be applied (see above). This fix version may differ from the one the mod was tested with. Your previous files are in $backup."
    }
    Say '   HUD edits applied (the fix author''s originals are in akvr_fix_backup)' 'Green'
}

# ---- 7. the mod ---------------------------------------------------------------------------

Step '7. Installing the mod'
if ($Game.OldProxy) {
    $old = Join-Path $GameDir $Game.OldProxy
    if ((Test-Path $old) -and ([System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($old))).Contains($Game.Mod)) {
        Move-Item $old (Join-Path $GameDir ($Game.OldProxy + '.old-' + $Game.Mod)) -Force
        Say "   an older copy of the mod ($($Game.OldProxy)) was set aside" 'Green'
    }
}
if ($ModDll) {
    Copy-Item $ModDll (Join-Path $GameDir 'dinput8.dll') -Force
    Say '   dinput8.dll copied' 'Green'
} else {
    Say "   skipped - copy the mod's dinput8.dll into $GameDir yourself" 'Yellow'
}

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '  Done.' -ForegroundColor Green
Write-Host "  Start Virtual Desktop, connect your headset, then start $($Game.Name) from Steam." -ForegroundColor White
Write-Host "  The first start can take a few minutes while geo-11 prepares its shaders." -ForegroundColor White
Write-Host ''
