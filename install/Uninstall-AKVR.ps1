<#
  AKVR uninstaller: removes the VR mod and the geo-11 3D fix from Batman: Arkham Knight and puts
  back the files the first install replaced.

  It also sets the game's graphics settings back to the game's own defaults.

  Double-click Uninstall-AKVR.bat. Optional: -GameDir "<folder with BatmanAK.exe>", -Yes (no questions,
  backups kept), -RemoveBackups (also delete the vrmod_backup folders).
#>
param([string]$GameDir, [switch]$Yes, [switch]$RemoveBackups)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms

$Exe         = 'BatmanAK.exe'
$SteamFolder = 'Batman Arkham Knight\Binaries\Win64'

function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Step([string]$text) { Write-Host ''; Write-Host "  $text" -ForegroundColor Cyan }
function Fail([string]$text) { Write-Host ''; Write-Host "  STOPPED: $text" -ForegroundColor Red; Write-Host ''; exit 1 }

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
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) { $libraries += ($m.Groups[1].Value -replace '\\\\', '\') }
        }
    }
    foreach ($lib in ($libraries | Select-Object -Unique)) {
        $dir = Join-Path $lib ('steamapps\common\' + $SteamFolder)
        if (Test-Path (Join-Path $dir $Exe)) { return $dir }
    }
    return $null
}

function Select-Folder([string]$description) {
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = $description
    $f = New-Object System.Windows.Forms.Form; $f.TopMost = $true
    if ($d.ShowDialog($f) -eq [System.Windows.Forms.DialogResult]::OK) { return $d.SelectedPath }
    return $null
}

function Remove-IfThere([string]$path) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force; return 1 }
    return 0
}

Write-Host ''
Write-Host '  AKVR uninstaller - Batman: Arkham Knight VR' -ForegroundColor White
Write-Host '  ------------------------------------------------------------' -ForegroundColor DarkGray

# ---- 1. the game folder -----------------------------------------------------------------------
Step '1. Finding the game'
# The installer puts this uninstaller in the game folder itself (JJ, 2026-09-29).
if (-not $GameDir -and (Test-Path (Join-Path $PSScriptRoot $Exe))) { $GameDir = $PSScriptRoot }
if (-not $GameDir) { $GameDir = Find-SteamGameDir }
if (-not $GameDir) {
    Say "   Could not find Batman: Arkham Knight in your Steam libraries. Please pick the folder that contains $Exe."
    $GameDir = Select-Folder "Pick the Batman: Arkham Knight folder that contains $Exe"
}
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir $Exe))) { Fail "$Exe is not in that folder." }
$GameDir = (Resolve-Path $GameDir).Path
Say "   $GameDir" 'Green'
if (Get-Process -Name 'BatmanAK' -ErrorAction SilentlyContinue) { Fail 'the game is running. Close it and run this again.' }

if (-not $Yes) {
    Write-Host ''
    Write-Host '  This removes the VR mod and the geo-11 3D fix from that folder, including your VR' -ForegroundColor White
    Write-Host '  settings and the "Batman Arkham Knight (VR)" shortcuts, puts back the files the first' -ForegroundColor White
    Write-Host '  install replaced, and leaves the game with your normal (2D) graphics settings.' -ForegroundColor White
    $answer = Read-Host '  Continue? (Y/N)'
    if ($answer -notmatch '^\s*[Yy]') { Say '   Nothing was changed.' 'Yellow'; exit 0 }
}

# ---- 2. the 3D fix ----------------------------------------------------------------------------
Step '2. Removing the 3D fix'
# The fix author's own uninstall.bat lists every file the fix can add; it deletes itself at the end.
if (Test-Path (Join-Path $GameDir 'uninstall.bat')) {
    # Its DEL lines are relative, so it must run IN the game folder (Push-Location does not move cmd).
    Start-Process -FilePath "$env:SystemRoot\System32\cmd.exe" -ArgumentList '/c', 'uninstall.bat' -WorkingDirectory $GameDir -WindowStyle Hidden -Wait
    Say '   removed with the fix''s own uninstall.bat' 'Green'
} else {
    Say '   the fix''s uninstall.bat is not there - removing its files directly' 'Yellow'
}
# What the fix's list does not cover (and a safety net if it was missing).
$n = 0
foreach ($f in 'd3dx.ini', 'd3dxdm.ini', 'd3dx_user.ini', 'd3d11.dll', 'dxgi.dll', 'nvapi64.dll', 'd3d11_log.txt', 'dxgi_log.txt', 'nvapi_log.txt',
               'ShaderFixes', 'ShaderFixesDM', 'ShaderCache', 'ShaderCacheDM', 'DMAutoPatchCache', 'DMAutoPatchFailures', 'uninstall.bat') {
    $n += Remove-IfThere (Join-Path $GameDir $f)
}
if ($n) { Say "   $n more fix file(s) or folder(s) removed" 'Green' }

# ---- 3. the mod ---------------------------------------------------------------------------------
Step '3. Removing the mod'
# VRLAUNCH (2026-10-02): Steam starts the game in 2D, the VR shortcut in VR, and the mod keeps each mode's graphics
# settings in akvr_profiles\{2d,vr}. Read which mode ran last and keep the 2D copy before akvr_* goes (step 5 uses it).
$profLast = $null
$prof2d = Join-Path $env:TEMP ('akvr-2d-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$lastFile = Join-Path $GameDir 'akvr_profiles\last.txt'
if (Test-Path -LiteralPath $lastFile) { $profLast = (Get-Content -LiteralPath $lastFile -Raw).Trim().ToLower() }
if (Test-Path -LiteralPath (Join-Path $GameDir 'akvr_profiles\2d')) {
    & robocopy (Join-Path $GameDir 'akvr_profiles\2d') $prof2d /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
}
# The VR launcher and its shortcuts.
$n = 0
$n += Remove-IfThere (Join-Path $GameDir 'AKVR-Launch-VR.bat')
foreach ($place in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) {
    if ($place) { $n += Remove-IfThere (Join-Path $place 'Batman Arkham Knight (VR).lnk') }
}
# dinput8.dll only if it is ours (the name is common to other mods).
$proxy = Join-Path $GameDir 'dinput8.dll'
if ((Test-Path $proxy) -and ([System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($proxy))).Contains('AKVR')) {
    Remove-Item -LiteralPath $proxy -Force; $n++
}
foreach ($f in 'geo11.dll', 'dxgi.dll.wrapmode', 'akvr_fix_backup') { $n += Remove-IfThere (Join-Path $GameDir $f) }
# The mod's settings, logs and captures.
foreach ($item in Get-ChildItem -LiteralPath $GameDir -Filter 'akvr_*') { Remove-Item -LiteralPath $item.FullName -Recurse -Force; $n++ }
Say "   $n file(s) removed" 'Green'

# ---- 4. put back what was there before --------------------------------------------------------------
Step '4. Putting back your original files'
# Only a backup taken BEFORE the mod was ever installed holds the player's own files. A backup made by
# a reinstall holds the mod and the fix themselves (geo11.dll, the mod's dinput8.dll): restoring that
# would put them straight back (JJ's clean-slate request, 2026-09-29). Files only; nothing is overwritten.
function Test-PreModBackup([string]$dir) {
    if (Test-Path -LiteralPath (Join-Path $dir 'geo11.dll')) { return $false }
    $p = Join-Path $dir 'dinput8.dll'
    if ((Test-Path -LiteralPath $p) -and ([System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($p))).Contains('AKVR')) { return $false }
    return $true
}
$backups = @(Get-ChildItem -LiteralPath $GameDir -Directory -Filter 'vrmod_backup_*' | Sort-Object Name | Where-Object { Test-PreModBackup $_.FullName })
if ($backups.Count -eq 0) {
    Say '   nothing to put back (no backup from before the mod was installed)' 'Green'
} else {
    $first = $backups[0].FullName
    $restored = 0
    foreach ($item in Get-ChildItem -LiteralPath $first -Recurse -File) {
        $rel = $item.FullName.Substring($first.Length + 1)
        $target = Join-Path $GameDir $rel
        if (-not (Test-Path -LiteralPath $target)) {
            New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
            Copy-Item -LiteralPath $item.FullName -Destination $target
            $restored++
        }
    }
    Say "   $restored file(s) put back from $(Split-Path $first -Leaf)" 'Green'
}

# ---- 5. the game's graphics settings: the player's own back, else the game's defaults -------------------
Step '5. Putting back your graphics settings'
# Two places hold them: the game's BmSystemSettings.ini, and NVIDIA's settings store in Documents, which
# the game reads at every start (display mode, resolution, detail, GameWorks, blur). While the mod runs,
# its answers (windowed + the VR size) are saved into both; after removing the mod the game opened in a
# square window (JJ, 2026-09-29). The installer saves the player's own copies first
# (vrmod_graphics_backup); those go back exactly. Without them (an install older than that), both are
# reset so the game starts from its defaults. Documents can be moved (JJ's is D:\Documents): the current
# user's Documents as Windows reports it (the game's own lookup), the registry entry, then the defaults.
$docs = @([Environment]::GetFolderPath('MyDocuments'))
try {
    $reg = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders' -ErrorAction Stop).Personal
    if ($reg) { $docs += [Environment]::ExpandEnvironmentVariables($reg) }
} catch { }
$docs += (Join-Path $env:USERPROFILE 'Documents'), (Join-Path $env:USERPROFILE 'OneDrive\Documents')
$stores = @($docs | Where-Object { $_ } | Select-Object -Unique |
            ForEach-Object { Join-Path $_ 'WB Games\Batman Arkham Knight\GFXSettings.BatmanArkhamKnight.xml' })
$mainStore = $stores[0]   # the Documents folder Windows reports for this user

$cfg = Join-Path $GameDir '..\..\BmGame\Config'
$cfg = if (Test-Path -LiteralPath $cfg) { (Resolve-Path -LiteralPath $cfg).Path } else { $null }
$ini = if ($cfg) { Join-Path $cfg 'BmSystemSettings.ini' } else { $null }
$gfxBackup = Join-Path $GameDir 'vrmod_graphics_backup'

# The settings template: the mod edits it on a brand-new install and keeps the original.
if ($cfg) {
    $tmpl = Join-Path $cfg 'DefaultSystemSettings.ini'
    if (Test-Path -LiteralPath "$tmpl.akvr-original") { Copy-Item -LiteralPath "$tmpl.akvr-original" -Destination $tmpl -Force }
}

if ($profLast -eq '2d') {
    # The last start was from Steam (2D): the files the game holds now ARE the player's 2D settings, newer than the
    # copy from before the first install - keep them.
    if (Test-Path -LiteralPath $gfxBackup) { Remove-Item -LiteralPath $gfxBackup -Recurse -Force }
    Say '   your graphics settings kept (the game was last played normally, in 2D)' 'Green'
} elseif ($profLast -eq 'vr' -and (Test-Path -LiteralPath (Join-Path $prof2d 'BmSystemSettings.ini'))) {
    # The last start was in VR: the game holds the VR settings; the mod kept the 2D ones it had before.
    if ($ini) { Copy-Item -LiteralPath (Join-Path $prof2d 'BmSystemSettings.ini') -Destination $ini -Force }
    $saved2dStore = Join-Path $prof2d 'GFXSettings.BatmanArkhamKnight.xml'
    if (Test-Path -LiteralPath $saved2dStore) { Copy-Item -LiteralPath $saved2dStore -Destination $mainStore -Force }
    else { foreach ($s in $stores) { [void](Remove-IfThere $s) } }   # the player had none: VR's would open a square window
    if (Test-Path -LiteralPath $gfxBackup) { Remove-Item -LiteralPath $gfxBackup -Recurse -Force }
    Say '   your 2D graphics settings put back (as the game had them when you last played normally)' 'Green'
} elseif (Test-Path -LiteralPath $gfxBackup) {
    $savedIni   = Join-Path $gfxBackup 'BmSystemSettings.ini'
    $savedStore = Join-Path $gfxBackup 'GFXSettings.BatmanArkhamKnight.xml'
    if ($ini) {
        if (Test-Path -LiteralPath $savedIni) { Copy-Item -LiteralPath $savedIni -Destination $ini -Force }
        else { [void](Remove-IfThere $ini) }          # the player had none: the game makes its own again
    }
    # The store goes back where it came from (recorded by the installer), else to this user's Documents.
    $from = $null
    $note = Join-Path $gfxBackup 'README.txt'
    if (Test-Path -LiteralPath $note) {
        $m = [regex]::Match((Get-Content -LiteralPath $note -Raw), 'saved from (.+?GFXSettings\.BatmanArkhamKnight\.xml)')
        if ($m.Success) { $from = $m.Groups[1].Value.Trim() }
    }
    foreach ($s in $stores) { [void](Remove-IfThere $s) }
    if (Test-Path -LiteralPath $savedStore) {
        $to = if ($from -and (Test-Path -LiteralPath (Split-Path $from))) { $from } else { $mainStore }
        New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
        Copy-Item -LiteralPath $savedStore -Destination $to -Force
    }
    Remove-Item -LiteralPath $gfxBackup -Recurse -Force
    Say '   your own graphics settings put back (as they were before the first install)' 'Green'
} else {
    if ($ini) { [void](Remove-IfThere $ini) }
    $n = 0
    foreach ($s in $stores) { $n += Remove-IfThere $s }
    Say '   no saved copy of your graphics settings (installed before the uninstaller kept one):' 'Yellow'
    Say '   reset instead - the game starts from its own defaults; set resolution and effects in its menu' 'Yellow'
}
if ($cfg) { foreach ($o in Get-ChildItem -LiteralPath $cfg -File -Filter '*.akvr-original') { Remove-Item -LiteralPath $o.FullName -Force } }
if (Test-Path -LiteralPath $prof2d) { Remove-Item -LiteralPath $prof2d -Recurse -Force }

# ---- 6. the backup folders ---------------------------------------------------------------------------
$all = @(Get-ChildItem -LiteralPath $GameDir -Directory -Filter 'vrmod_backup_*')
if ($all.Count -gt 0) {
    Step '6. Backup folders'
    $del = $RemoveBackups
    if (-not $del -and -not $Yes) {
        $answer = Read-Host "  Delete the $($all.Count) vrmod_backup folder(s) as well, for a completely clean game folder? (Y/N)"
        $del = $answer -match '^\s*[Yy]'
    }
    if ($del) { foreach ($b in $all) { Remove-Item -LiteralPath $b.FullName -Recurse -Force }; Say "   $($all.Count) backup folder(s) deleted" 'Green' }
    else      { Say '   kept; delete them yourself when you no longer need them' 'Gray' }
}

# Run from the game folder: remove this uninstaller too (the .bat deletes itself when this file is gone).
if ((Resolve-Path $PSScriptRoot).Path -eq $GameDir) { Remove-Item -LiteralPath $PSCommandPath -Force }

Write-Host ''
Write-Host '  Done. Batman: Arkham Knight is back to normal.' -ForegroundColor Green
Write-Host '  If anything still looks wrong, use Steam: right-click the game, Properties, Installed Files,' -ForegroundColor White
Write-Host '  "Verify integrity of game files".' -ForegroundColor White
Write-Host ''
