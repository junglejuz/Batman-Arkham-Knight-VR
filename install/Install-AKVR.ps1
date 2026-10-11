<#
  Batman: Arkham Knight VR installer. The game draws both eyes itself (native stereo); no 3D fix is needed.

  Double-click Install-AKVR.bat. It finds the game by itself, backs up anything it replaces, saves your
  graphics settings for the uninstaller, and asks if it cannot find something. See the README, "Installing".

  Optional: -GameDir "<the game's folder>"   -Yes (no questions: an older geo-11 install is removed)
#>
param([string]$GameDir, [switch]$Yes)

$Game = @{
    Mod          = 'AKVR'
    Name         = 'Batman: Arkham Knight'
    Exe          = 'BatmanAK.exe'
    SteamFolder  = 'Batman Arkham Knight\Binaries\Win64'                      # under steamapps\common
    # The game's graphics settings the mod was tested with (BmSystemSettings.ini [SystemSettings]), written into
    # the VR copy of the settings on the first install only (the player's own stay for playing from Steam).
    # JJ's confirmed graphics menu: Max FPS 90 (the mod lifts the cap in VR anyway), texture resolution / shadow
    # quality / level of detail High, texture filtering 2x anisotropic, NVIDIA GameWorks effects off.
    GameGraphics = [ordered]@{
        'MaxFPS'                         = '90.000000'
        'TextureResolution'              = '2'
        'ShadowQuality'                  = '2'
        'LevelOfDetail'                  = '2'
        'MaxDrawDistanceScale'           = '1.200000'
        'SkeletalMeshDisplayFactorScale' = '0.800000'
        'TextureFiltering'               = '1'
        'bEnableInteractiveSmoke'        = '0'
        'bEnableInteractivePaperDebris'  = '0'
        'bEnableRainFX'                  = '0'
        'bEnableVolumetricLighting'      = '0'
    }
    # The same menu in NVIDIA's settings store (GFXSettings.BatmanArkhamKnight.xml), which the game reads at every
    # start and saves back into the ini. Only options already in the file are changed.
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
    # The mod's files: next to this script in the package, or the build folders when run from the project.
    ModFiles     = [ordered]@{
        'dinput8.dll'     = @('dinput8.dll', '..\build-dinput8\Release\dinput8.dll')
        'XINPUT9_1_0.dll' = @('XINPUT9_1_0.dll', '..\native\build\Release\XINPUT9_1_0.dll')
    }
    Launchers    = [ordered]@{
        'AKVR-Launch-VR.bat'         = 'Batman Arkham Knight (VR)'
        'AKVR-Launch-VR-SteamVR.bat' = 'Batman Arkham Knight (VR, SteamVR)'
    }
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


function Test-Ours([string]$path) {   # our proxy DLLs carry the mod's name; the file names are common to other mods
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $t = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($path))
    return $t.Contains('AKVR') -or $t.Contains('akn_api_version')
}

function Remove-IfThere([string]$path) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force; return 1 }
    return 0
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

# ---- 2. the package -----------------------------------------------------------------------

Step '2. Checking the download'
$files = Join-Path $PSScriptRoot 'files'
$src = @{}
foreach ($name in $Game.ModFiles.Keys) {
    $p = $Game.ModFiles[$name] | ForEach-Object { Join-Path $PSScriptRoot $_ } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $p) { Fail "$name is missing next to this script. Download the whole package again and unpack all of it." }
    $src[$name] = (Resolve-Path -LiteralPath $p).Path
}
foreach ($need in 'nvngx_dlss.dll', 'akvr_settings.ini') {
    if (-not (Test-Path (Join-Path $files $need))) { Fail "the 'files' folder next to this script is missing $need. Download the whole package again." }
}
foreach ($l in $Game.Launchers.Keys) {
    if (-not (Test-Path (Join-Path $PSScriptRoot $l))) { Fail "$l is missing next to this script. Download the whole package again." }
}
Say '   everything is there' 'Green'

# ---- 3. back up what is there now (before anything is removed: the old 3D fix's uninstall.bat also deletes XInput9_1_0.dll) ----------------------------------------------------------

Step '3. Backing up your current files'
$backup = Join-Path $GameDir ('vrmod_backup_' + (Get-Date -Format 'yyyy-MM-dd_HHmmss'))
$saved = 0
foreach ($n in $Game.ModFiles.Keys) {
    $p = Join-Path $GameDir $n
    # a file of the same name from another mod is kept; our own older copy needs no backup
    if ((Test-Path $p -PathType Leaf) -and -not (Test-Ours $p)) {
        New-Item -ItemType Directory -Force -Path $backup | Out-Null
        Copy-Item $p (Join-Path $backup $n); $saved++
    }
}
if ($saved -eq 0) { Say '   nothing to back up' }
else { Say "   saved in $backup" 'Green' }

# The player's own graphics settings, before the mod ever touches them, so the uninstaller can put them back
# exactly. Two places: the game's BmSystemSettings.ini, and NVIDIA's settings store in Documents (Documents may be
# moved; see Get-GfxStoreCandidates). Only once: never over a saved copy, never once the mod has run.
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
    @("BmSystemSettings.ini: $iniNote", "GFXSettings store: $storeNote") | Set-Content -LiteralPath (Join-Path $gfxBackup 'README.txt')
    Say '   your graphics settings saved (vrmod_graphics_backup) - the uninstaller puts them back' 'Green'
}

# ---- 4. an older version of this mod (geo-11) -----------------------------------------------
# Versions up to 0.2.1 used geo-11 and masterotaku's 3D fix (geo-11 renamed to geo11.dll by our installer). This
# version does not need them: remove them so nothing of the old setup is left behind.

$oldGeo = (Test-Path (Join-Path $GameDir 'geo11.dll')) -or (Test-Path (Join-Path $GameDir 'dxgi.dll.wrapmode'))
if ($oldGeo) {
    Step '4. An older version of the mod (with geo-11) is installed'
    Say '   This version does not need geo-11 or the 3D fix any more.'
    $go = $Yes
    if (-not $go) { $go = (Read-Host '   Remove geo-11 and the 3D fix now? Recommended. (Y/N)') -match '^\s*[Yy]' }
    if ($go) {
        if (Test-Path (Join-Path $GameDir 'uninstall.bat')) {
            # the fix author's own list; its DEL lines are relative, so it must run IN the game folder
            Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList '/c', 'uninstall.bat' -WorkingDirectory $GameDir -WindowStyle Hidden -Wait
        }
        $n = 0
        foreach ($f in 'geo11.dll', 'dxgi.dll.wrapmode', 'akvr_fix_backup', 'd3dx.ini', 'd3dxdm.ini', 'd3dx_user.ini', 'd3d11.dll', 'dxgi.dll',
                       'nvapi64.dll', 'd3d11_log.txt', 'dxgi_log.txt', 'nvapi_log.txt', 'ShaderFixes', 'ShaderFixesDM', 'ShaderCache',
                       'ShaderCacheDM', 'DMAutoPatchCache', 'DMAutoPatchFailures', 'uninstall.bat', 'AKVR-Launch-VR-EngineStereo.bat',
                       'AKVR-Launch-VR-EngineStereo-SteamVR.bat') {
            $n += Remove-IfThere (Join-Path $GameDir $f)
        }
        Say "   geo-11 and the 3D fix removed ($n files and folders)" 'Green'
    } else {
        Say '   kept. The mod works either way; the uninstaller removes them later.' 'Yellow'
    }
}

# ---- 5. the mod ---------------------------------------------------------------------------

Step '5. Installing the mod'
foreach ($n in $Game.ModFiles.Keys) {
    try { Copy-Item $src[$n] (Join-Path $GameDir $n) -Force }
    catch { Fail "could not copy $n into the game folder ($($_.Exception.Message)). Try right-clicking Install-AKVR.bat and 'Run as administrator'." }
}
New-Item -ItemType Directory -Force -Path (Join-Path $GameDir 'akn') | Out-Null
Copy-Item (Join-Path $files 'nvngx_dlss.dll') (Join-Path $GameDir 'akn\nvngx_dlss.dll') -Force
Say '   dinput8.dll, XINPUT9_1_0.dll and NVIDIA DLSS (akn\nvngx_dlss.dll) copied' 'Green'

# The uninstaller lives in the game folder, so players don't need to keep the download.
foreach ($u in 'Uninstall-AKVR.bat', 'Uninstall-AKVR.ps1') {
    $s = Join-Path $PSScriptRoot $u
    if (Test-Path $s) { Copy-Item $s (Join-Path $GameDir $u) -Force }
}
Say '   Uninstall-AKVR.bat put in the game folder' 'Green'

# The tested VR settings. Only on a first install: an existing file is the player's own. A settings file from a
# geo-11 version (no DLSS line) holds settings this version does not use: it is set aside and replaced.
$settings = Join-Path $GameDir 'akvr_settings.ini'
$tested   = Join-Path $files 'akvr_settings.ini'
$firstVr = -not (Test-Path $settings)
if (-not $firstVr -and -not (Select-String -LiteralPath $settings -Pattern '^dlss=' -Quiet)) {
    Move-Item -LiteralPath $settings -Destination (Join-Path $GameDir 'akvr_settings.from-older-version.ini') -Force
    $firstVr = $true
    Say '   the older version''s VR settings set aside (akvr_settings.from-older-version.ini)' 'Yellow'
}
if (-not $firstVr) { Say '   your existing VR settings kept (akvr_settings.ini)' 'Green' }
else {
    Copy-Item $tested $settings
    Say '   tested VR settings copied in (akvr_settings.ini)' 'Green'
    # VRLAUNCH: Steam starts the game normally (2D); the VR shortcut starts VR. The mod keeps each mode's graphics
    # settings (akvr_profiles\{2d,vr}) and swaps them when the mode changes, so the player's own settings stay as
    # they are for 2D. The tested VR graphics settings go into the VR copy only.
    $profVr = Join-Path $GameDir 'akvr_profiles\vr'
    New-Item -ItemType Directory -Force -Path $profVr | Out-Null
    $gen = Join-Path $GameDir '..\..\BmGame\Config\BmSystemSettings.ini'
    if (Test-Path $gen) {
        $vrIni = Join-Path $profVr 'BmSystemSettings.ini'
        if (-not (Test-Path $vrIni)) { Copy-Item $gen $vrIni -Force }
        $n = Set-SystemSettings $vrIni $Game.GameGraphics
        Say "   VR graphics settings prepared ($n changed): High detail, 2x anisotropic filtering, GameWorks off" 'Green'
    } else {
        Say '   the game has not been started yet: its VR graphics settings are made on the first VR start' 'Yellow'
    }
    $store = Get-GfxStoreCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($store) {
        $vrStore = Join-Path $profVr 'GFXSettings.BatmanArkhamKnight.xml'
        if (-not (Test-Path $vrStore)) { Copy-Item -LiteralPath $store $vrStore -Force }
        $n = Set-GfxStore $vrStore $Game.GameStore
        Say "   the same settings prepared for NVIDIA's settings store ($n changed)" 'Green'
    }
    $last = Join-Path $GameDir 'akvr_profiles\last.txt'
    if (-not (Test-Path $last)) { [IO.File]::WriteAllText($last, '2d') }   # the files the game holds now are the player's
    Say '   your normal graphics settings untouched (they stay for playing from Steam)' 'Green'
}

# ---- 6. the VR shortcuts ----------------------------------------------------------------------
# Starting the game from Steam plays it normally with nothing of the mod running; the shortcuts start it in VR.
Step '6. The VR shortcuts'
$made = 0
try {
    $shell = New-Object -ComObject WScript.Shell
    foreach ($l in $Game.Launchers.Keys) {
        Copy-Item (Join-Path $PSScriptRoot $l) (Join-Path $GameDir $l) -Force
        foreach ($place in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) {
            if (-not $place) { continue }
            $lnk = $shell.CreateShortcut((Join-Path $place ($Game.Launchers[$l] + '.lnk')))
            $lnk.TargetPath = Join-Path $env:SystemRoot 'System32\cmd.exe'
            $lnk.Arguments = '/c "' + (Join-Path $GameDir $l) + '"'
            $lnk.WorkingDirectory = $GameDir
            $lnk.WindowStyle = 7                                     # minimised: no console flashing up
            $lnk.IconLocation = (Join-Path $GameDir $Game.Exe) + ',0'
            $lnk.Description = "Start $($Game.Name) in VR (from Steam it starts normally)"
            $lnk.Save()
            $made++
        }
    }
    # and one for removing the mod, in the Start menu (as Batman City VR; PACKAGING_GUIDE section 12)
    $progs = [Environment]::GetFolderPath('Programs')
    if ($progs) {
        $lnk = $shell.CreateShortcut((Join-Path $progs 'Uninstall Batman Arkham Knight VR.lnk'))
        $lnk.TargetPath = Join-Path $env:SystemRoot 'System32\cmd.exe'
        $lnk.Arguments = '/c "' + (Join-Path $GameDir 'Uninstall-AKVR.bat') + '"'
        $lnk.WorkingDirectory = $GameDir
        $lnk.Description = "Remove $($Game.Name) VR and put your settings back"
        $lnk.Save()
    }
} catch { Say "   could not make the shortcuts: $($_.Exception.Message)" 'Yellow' }
if ($made) { Say '   "Batman Arkham Knight (VR)" and "Batman Arkham Knight (VR, SteamVR)" on your desktop and in the Start menu' 'Green' }
else { Say "   start VR with AKVR-Launch-VR.bat in $GameDir" 'Yellow' }

Write-Host ''
Write-Host '  Done.' -ForegroundColor Green
Write-Host "  To play in VR: start your headset's PC VR software, then the desktop shortcut 'Batman Arkham Knight (VR)'." -ForegroundColor White
Write-Host "  For SteamVR, use 'Batman Arkham Knight (VR, SteamVR)'. Starting the game from Steam plays it normally." -ForegroundColor White
Write-Host ''
