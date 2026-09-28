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
    Write-Host '  settings, puts back the files the first install replaced, and sets the game''s graphics' -ForegroundColor White
    Write-Host '  settings back to the game''s defaults.' -ForegroundColor White
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
$n = 0
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

# ---- 5. the game's graphics settings back to its defaults ------------------------------------------
Step '5. Setting the game''s graphics back to its defaults'
# The mod edits the game's config at every start (and the installer sets the tested graphics on a first
# install); both keep the untouched original as *.akvr-original. The template the game builds its
# settings from goes back to that original; the generated settings file is deleted, so the game makes a
# fresh default one on its next start, exactly as after a new install.
$cfg = Join-Path $GameDir '..\..\BmGame\Config'
if (Test-Path -LiteralPath $cfg) {
    $cfg = (Resolve-Path -LiteralPath $cfg).Path
    $tmpl = Join-Path $cfg 'DefaultSystemSettings.ini'
    if (Test-Path -LiteralPath "$tmpl.akvr-original") {
        Copy-Item -LiteralPath "$tmpl.akvr-original" -Destination $tmpl -Force
        Say '   the game''s settings template put back to its original' 'Green'
    }
    $n = Remove-IfThere (Join-Path $cfg 'BmSystemSettings.ini')
    foreach ($o in Get-ChildItem -LiteralPath $cfg -File -Filter '*.akvr-original') { Remove-Item -LiteralPath $o.FullName -Force }
    if ($n) { Say '   graphics settings reset: the game rebuilds its defaults the next time it starts' 'Green' }
    else    { Say '   already at the defaults' 'Green' }
} else {
    Say '   the game''s config folder was not found - skipped' 'Yellow'
}

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
