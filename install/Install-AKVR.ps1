<#
  AKVR installer: sets up the Batman: Arkham Knight geo-11 3D fix for VR and installs the mod.

  Double-click Install-AKVR.bat. It finds the game and your downloads by itself, backs up
  anything it replaces, and asks if it cannot find something. See the README, "Installing".

  Optional, to skip the searching:
    -GameDir "<the game's folder>" -FixArchive "<fix .7z>" -ModDll "<dinput8.dll>" -Picture Low|Medium|High
#>
param([string]$GameDir, [string]$FixArchive, [string]$ModDll, [string]$Picture)

$Game = @{
    Mod          = 'AKVR'
    Name         = 'Batman: Arkham Knight'
    Exe          = 'BatmanAK.exe'
    SteamFolder  = 'Batman Arkham Knight\Binaries\Win64'                      # under steamapps\common
    FixMatch     = 'arkham.*knight.*geo-?11.*\.(7z|zip)$'   # the fix's file name
    FixPage      = 'https://masterotaku.s3.amazonaws.com/Batman+Arkham+Knight/Batman_Arkham_Knight_geo11_fix.7z'
    AddNonSquare = $true                         # keep 3D on the tall VR picture (FIX_CHANGES.md section 3)
    FixPatches   = 'AKVR-fix-patches.ps1'       # the mod's HUD edits to the fix (FIX_CHANGES.md sections 1-2)
    # The game's graphics settings the mod was tested with (BmSystemSettings.ini [SystemSettings]),
    # set on the first install only. = JJ's confirmed graphics menu, 2026-09-29: Max FPS 90, texture
    # resolution / shadow quality / level of detail High, texture filtering 2x anisotropic (as the game
    # saves them). The stock 60 fps cap was the cause of the blur and the changed pose delay.
    GameGraphics = [ordered]@{
        'MaxFPS'                         = '90.000000'
        'TextureResolution'              = '2'
        'ShadowQuality'                  = '2'
        'LevelOfDetail'                  = '2'
        'MaxDrawDistanceScale'           = '1.200000'
        'SkeletalMeshDisplayFactorScale' = '0.800000'
        'TextureFiltering'               = '1'
        # GameWorks off (JJ, 2026-09-29: a player who had them on kept them on - the stock install
        # has them off, so the first list never needed them).
        'bEnableInteractiveSmoke'        = '0'
        'bEnableInteractivePaperDebris'  = '0'
        'bEnableRainFX'                  = '0'
        'bEnableVolumetricLighting'      = '0'
    }
    # The same menu in NVIDIA's settings store (GFXSettings.BatmanArkhamKnight.xml), which the game reads
    # at every start and saves back into the ini (that is how MotionBlur came back, FIX_CHANGES.md section 6).
    # Only options already in the file are changed; no file = the game builds it from the ini.
    GameStore    = [ordered]@{
        'Texture_Resolution'       = '2'
        'Shadow_Quality'           = '2'
        'Level_Of_Detail'          = '2'
        'TextureFiltering'         = '1'
        'Interactive_Smoke'        = 'false'
        'Interactive_Paper_Debris' = 'false'
        'Rain_FX'                  = 'false'
        'Volumetric_Lighting'      = 'false'
    }
    # The picture size per eye offered on a first install (the height; the mod derives the width from
    # the headset's shape, ~1.01 wide:tall on a Quest 3). High = the tested setup (RTX 4070 Ti, 45 fps).
    # Written as engineres + rendersize in akvr_settings.ini; the panel's "picture height per eye" later.
    PictureSizes = [ordered]@{
        'Low'    = @{ Height = 2016; Note = 'for most graphics cards' }
        'Medium' = @{ Height = 2432; Note = 'for fast graphics cards' }
        'High'   = @{ Height = 2860; Note = 'what the mod was tested with (RTX 4070 Ti)' }
    }
    PictureDefault = 'Medium'
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

# The folder with the game's .exe, from any folder of the game the player picked: the game's own top
# folder, Binaries, or the .exe's folder itself (the .exe sits two folders down, which nobody guesses).
# Several copies below the pick (e.g. the whole Steam "common" folder) = ask again.
function Resolve-GameDir([string]$picked) {
    if (-not $picked -or -not (Test-Path -LiteralPath $picked -PathType Container)) { return @() }
    if (Test-Path -LiteralPath (Join-Path $picked $Game.Exe)) { return @($picked) }
    return @(Get-ChildItem -LiteralPath $picked -Filter $Game.Exe -Recurse -Depth 4 -File -ErrorAction SilentlyContinue |
        ForEach-Object { $_.DirectoryName } | Select-Object -Unique)
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

# Sets Key=Value lines in the [SystemSettings] section of a UE3 config (the game's own format: no
# spaces around '='; other sections, e.g. the quality buckets, are left alone). Returns lines changed.
function Set-SystemSettings([string]$path, $pairs) {
    $doc = Read-Ini $path
    $inSec = $false; $secEnd = -1; $secStart = -1; $seen = @{}; $changed = 0
    for ($i = 0; $i -lt $doc.Lines.Count; $i++) {
        $l = $doc.Lines[$i]
        if ($l -match '^\s*\[') {
            if ($inSec) { $secEnd = $i; break }
            $inSec = $l -match '^\s*\[SystemSettings\]\s*$'
            if ($inSec) { $secStart = $i }
            continue
        }
        if (-not $inSec) { continue }
        foreach ($k in $pairs.Keys) {
            if ($l -match ('^' + [regex]::Escape($k) + '=')) {
                $want = "$k=$($pairs[$k])"
                if ($l -ne $want) { $doc.Lines[$i] = $want; $changed++ }
                $seen[$k] = $true
            }
        }
    }
    if ($secStart -lt 0) { return 0 }
    if ($secEnd -lt 0) { $secEnd = $doc.Lines.Count }
    foreach ($k in $pairs.Keys) {
        if (-not $seen[$k]) { $doc.Lines.Insert($secStart + 1, "$k=$($pairs[$k])"); $changed++ }
    }
    if ($changed) { Save-Ini $doc }
    return $changed
}

# Sets Value="..." on <OPTION Name="..."> entries of NVIDIA's settings store (UTF-16, no byte-order mark,
# as the game writes it). Options missing from the file are left out. Returns options changed.
function Set-GfxStore([string]$path, $pairs) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $bom = $bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE
    $enc = New-Object System.Text.UnicodeEncoding($false, $bom)
    $text = [System.IO.File]::ReadAllText($path, $enc)
    $changed = 0
    foreach ($k in $pairs.Keys) {
        $rx = '(<OPTION Name="' + [regex]::Escape($k) + '"[^>]*?Value=")([^"]*)(")'
        $m = [regex]::Match($text, $rx)
        if ($m.Success -and $m.Groups[2].Value -ne $pairs[$k]) {
            $text = $text.Substring(0, $m.Groups[2].Index) + $pairs[$k] + $text.Substring($m.Groups[2].Index + $m.Groups[2].Length)
            $changed++
        }
    }
    if ($changed) { [System.IO.File]::WriteAllText($path, $text, $enc) }
    return $changed
}

# Where NVIDIA's settings store for the game can be: the CURRENT user's Documents as Windows reports it
# (follows a moved or OneDrive folder - the game's own lookup), the registry entry for a moved Documents,
# then the usual defaults. First existing file wins.
function Get-GfxStoreCandidates {
    $docs = @([Environment]::GetFolderPath('MyDocuments'))
    try {
        $reg = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders' -ErrorAction Stop).Personal
        if ($reg) { $docs += [Environment]::ExpandEnvironmentVariables($reg) }
    } catch { }
    $docs += (Join-Path $env:USERPROFILE 'Documents'), (Join-Path $env:USERPROFILE 'OneDrive\Documents')
    return $docs | Where-Object { $_ } | Select-Object -Unique |
        ForEach-Object { Join-Path $_ 'WB Games\Batman Arkham Knight\GFXSettings.BatmanArkhamKnight.xml' }
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
if ($GameDir) {
    $found = @(Resolve-GameDir $GameDir)
    if ($found.Count -eq 0) { Fail "$($Game.Exe) is not in $GameDir or the folders inside it." }
    if ($found.Count -gt 1) { Fail "$GameDir holds more than one copy of the game: $($found -join '; '). Give the one to install into." }
    $GameDir = $found[0]
} else { $GameDir = Find-SteamGameDir }
if (-not $GameDir) {
    Say "   Could not find $($Game.Name) in your Steam libraries."
    Say "   Please pick the game's folder (the one named after the game is fine)."
    while (-not $GameDir) {
        $picked = Select-Folder "Pick the $($Game.Name) folder"
        if (-not $picked) { Fail "no game folder picked. Run the installer again and pick the game's folder." }
        $found = @(Resolve-GameDir $picked)
        if ($found.Count -eq 1) { $GameDir = $found[0] }
        elseif ($found.Count -eq 0) { Say "   $($Game.Exe) is not in $picked or the folders inside it. Please pick the game's folder." 'Yellow' }
        else {
            Say "   That folder holds more than one copy of the game:" 'Yellow'
            $found | ForEach-Object { Say "     $_" 'Yellow' }
            Say '   Please pick the one to install into.' 'Yellow'
        }
    }
}
$GameDir = (Resolve-Path $GameDir).Path
Say "   $GameDir" 'Green'

$proc = Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($Game.Exe)) -ErrorAction SilentlyContinue
if ($proc) { Fail "the game is running. Close it and run this again." }

# The picture size, asked only when the tested VR settings will be copied in (first install).
$pictureH = $null
if ($Game.PictureSizes -and -not (Test-Path (Join-Path $GameDir 'akvr_settings.ini'))) {
    $names = @($Game.PictureSizes.Keys)
    if ($Picture) {
        $Picture = $names | Where-Object { $_ -eq $Picture } | Select-Object -First 1
        if (-not $Picture) { Fail "-Picture must be one of: $($names -join ', ')" }
    } else {
        Step 'Picture sharpness'
        Say '   Sharper pictures need a faster graphics card (every picture is drawn twice).'
        Say '   You can change it later: F8, "picture height per eye".'
        for ($i = 0; $i -lt $names.Count; $i++) {
            $s = $Game.PictureSizes[$names[$i]]
            Say ("     {0}  {1,-7} {2} pixels tall per eye - {3}" -f ($i + 1), $names[$i], $s.Height, $s.Note)
        }
        $def = [array]::IndexOf($names, $Game.PictureDefault) + 1
        while (-not $Picture) {
            $a = (Read-Host "   Type 1-$($names.Count) and press Enter (just Enter = $def, $($Game.PictureDefault))").Trim()
            if ($a -eq '') { $a = "$def" }
            $n = 0
            if ([int]::TryParse($a, [ref]$n) -and $n -ge 1 -and $n -le $names.Count) { $Picture = $names[$n - 1] }
        }
    }
    $pictureH = $Game.PictureSizes[$Picture].Height
    Say "   $Picture ($pictureH pixels tall per eye)" 'Green'
}

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

# The player's own graphics settings, before the mod ever touches them, so the uninstaller can put them
# back exactly (JJ, 2026-09-29: "game defaults" are not what a player had - resolution, GameWorks,
# texture filtering). Two places: the game's BmSystemSettings.ini, and NVIDIA's settings store in
# Documents (Documents may be moved; see Get-GfxStoreCandidates). Only once: never over a saved copy.
$gfxBackup = Join-Path $GameDir 'vrmod_graphics_backup'
if (-not (Test-Path $gfxBackup) -and -not (Test-Path (Join-Path $GameDir 'akvr_settings.ini'))) {
    New-Item -ItemType Directory -Force -Path $gfxBackup | Out-Null
    $ini = Join-Path $GameDir '..\..\BmGame\Config\BmSystemSettings.ini'
    $iniNote = 'absent'
    if (Test-Path $ini) { Copy-Item $ini (Join-Path $gfxBackup 'BmSystemSettings.ini'); $iniNote = 'saved' }
    $storeNote = 'absent'
    foreach ($s in Get-GfxStoreCandidates) {
        if (Test-Path -LiteralPath $s) { Copy-Item -LiteralPath $s (Join-Path $gfxBackup 'GFXSettings.BatmanArkhamKnight.xml'); $storeNote = "saved from $s"; break }
    }
    # What was (not) there, so the uninstaller can also remove files the player never had.
    @("BmSystemSettings.ini: $iniNote", "GFXSettings store: $storeNote") | Set-Content -LiteralPath (Join-Path $gfxBackup 'README.txt')
    Say '   your graphics settings saved (vrmod_graphics_backup) - the uninstaller puts them back' 'Green'
}

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

# The tested VR setup (eye order, render size and shape, HUD size, pose delay, ...). The mod's
# built-in defaults are NOT that setup (JJ's fresh-install test, 2026-09-29: eyes swapped, flat
# 16:9 picture). Only on a first install: an existing settings file is the player's own.
# The uninstaller lives in the game folder, so players don't need to keep the download (JJ).
foreach ($u in 'Uninstall-AKVR.bat', 'Uninstall-AKVR.ps1') {
    $s = Join-Path $PSScriptRoot $u
    if (Test-Path $s) { Copy-Item $s (Join-Path $GameDir $u) -Force }
}
if (Test-Path (Join-Path $GameDir 'Uninstall-AKVR.bat')) { Say '   Uninstall-AKVR.bat put in the game folder' 'Green' }

$settings = Join-Path $GameDir 'akvr_settings.ini'
$tested   = Join-Path $files 'akvr_settings.ini'
if (Test-Path $settings) { Say '   your existing VR settings kept (akvr_settings.ini)' 'Green' }
elseif (Test-Path $tested) {
    Copy-Item $tested $settings
    Say '   tested VR settings copied in (akvr_settings.ini)' 'Green'
    if ($pictureH) {
        $doc = Read-Ini $settings
        foreach ($k in 'engineres', 'rendersize') {
            for ($i = 0; $i -lt $doc.Lines.Count; $i++) { if ($doc.Lines[$i] -match "^$k=") { $doc.Lines[$i] = "$k=$pictureH" } }
        }
        Save-Ini $doc
        Say "   picture size set: $Picture" 'Green'
    }
    # The game's own graphics settings the mod was tested with (JJ's fresh-install test,
    # 2026-09-29: the stock 60 fps cap and higher detail blurred head movement). First install
    # only, so later choices in the game's menu stand. FIX_CHANGES.md section 6.
    if ($Game.GameGraphics) {
        $cfg  = Join-Path $GameDir '..\..\BmGame\Config'
        $gen  = Join-Path $cfg 'BmSystemSettings.ini'
        $tmpl = Join-Path $cfg 'DefaultSystemSettings.ini'
        # The generated file if the game has made one; before the first start, the template it is
        # made from (editing the template once the generated file exists makes the game rebuild it).
        $target = if (Test-Path $gen) { $gen } elseif (Test-Path $tmpl) { $tmpl } else { $null }
        if ($target) {
            $n = Set-SystemSettings $target $Game.GameGraphics
            Say "   tested graphics settings applied ($n changed): Max FPS 90, High detail, 2x anisotropic filtering, GameWorks off" 'Green'
        }
    }
    if ($Game.GameStore) {
        $store = Get-GfxStoreCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if ($store) {
            $n = Set-GfxStore $store $Game.GameStore
            Say "   the same settings put in NVIDIA's settings store ($n changed)" 'Green'
        }
    }
}

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '  Done.' -ForegroundColor Green
Write-Host "  Start Virtual Desktop, connect your headset, then start $($Game.Name) from Steam." -ForegroundColor White
Write-Host "  The first start can take a few minutes while geo-11 prepares its shaders." -ForegroundColor White
Write-Host ''
