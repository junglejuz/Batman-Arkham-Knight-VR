# AKVR — switch geo-11 between WRAP mode and HOOK mode.
#
#   WRAP  = geo-11 IS d3d11.dll (how it ships). True stereo works. VR does NOT —
#           Virtual Desktop cannot find the headset inside the game process.
#   HOOK  = geo-11 is renamed to geo11.dll and AKVR loads it by that name.
#           The genuine Windows d3d11.dll is then present and whole, and geo-11
#           taps only two functions inside it. This is the experiment.
#
# Nothing is deleted — files are renamed back and forth, and d3dx.ini is edited
# in place with a .wrapmode copy kept beside it.
#
# Usage:  AKVR-geo11-HOOK.bat   /   AKVR-geo11-WRAP.bat   (double-click either)

param([Parameter(Mandatory=$true)][ValidateSet('hook','wrap','status')][string]$Mode)

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$d3d11  = Join-Path $dir 'd3d11.dll'
$alias  = Join-Path $dir 'geo11.dll'
$dxgi   = Join-Path $dir 'dxgi.dll'
$dxgiOff= Join-Path $dir 'dxgi.dll.wrapmode'
$ini    = Join-Path $dir 'd3dx.ini'

function Set-IniKey([string]$path, [string]$key, [string]$value) {
    if (-not (Test-Path $path)) { return }
    $lines = [System.IO.File]::ReadAllLines($path)
    $hit = $false
    for ($i = 0; $i -lt $lines.Length; $i++) {
        if ($lines[$i] -match "^\s*$key\s*=") { $lines[$i] = "$key=$value"; $hit = $true }
    }
    if (-not $hit) { return }
    # No BOM, ever. UE3 dies on one and there is no reason to risk 3DMigoto's parser
    # either — see the AKVR memory `geo11-works-on-ak-fresh-install`.
    [System.IO.File]::WriteAllLines($path, $lines, (New-Object System.Text.UTF8Encoding($false)))
}

function Show-Status {
    Write-Host ''
    Write-Host '  geo-11 files in this folder:' -ForegroundColor Cyan
    foreach ($f in @('d3d11.dll','geo11.dll','dxgi.dll','dxgi.dll.wrapmode')) {
        $p = Join-Path $dir $f
        $mark = if (Test-Path $p) { 'present' } else { '-' }
        Write-Host ("    {0,-20} {1}" -f $f, $mark)
    }
    if (Test-Path $ini) {
        $r = (Select-String -Path $ini -Pattern '^\s*load_library_redirect\s*=' | Select-Object -First 1).Line
        Write-Host "    d3dx.ini             $r"
    }
    $mode = if (Test-Path $alias) { 'HOOK' } elseif (Test-Path $d3d11) { 'WRAP' } else { 'geo-11 NOT INSTALLED' }
    Write-Host ''
    Write-Host "  Current mode: $mode" -ForegroundColor Yellow
    Write-Host ''
}

if ($Mode -eq 'status') { Show-Status; return }

if ($Mode -eq 'hook') {
    if (Test-Path $alias) { Write-Host 'Already in HOOK mode.' -ForegroundColor Yellow; Show-Status; return }
    if (-not (Test-Path $d3d11)) { Write-Host 'ERROR: no d3d11.dll here - geo-11 is not installed.' -ForegroundColor Red; return }

    Rename-Item $d3d11 'geo11.dll'
    # geo-11's dxgi.dll is a second wrapper and is NOT needed for stereo on Arkham
    # (measured 2026-08-07). In hook mode we want the genuine dxgi.dll, so park it.
    if (Test-Path $dxgi) { Move-Item $dxgi $dxgiOff -Force }
    if (-not (Test-Path "$ini.wrapmode")) { Copy-Item $ini "$ini.wrapmode" }
    # Pointless in hook mode: its job was to stop nvapi bypassing the WRAPPER by
    # loading d3d11.dll straight out of System32. In hook mode the System32 copy is
    # the one we patched, so there is nothing to bypass - and with geo-11 renamed
    # there is no d3d11.dll in this folder to redirect to anyway.
    Set-IniKey $ini 'load_library_redirect' '0'
    Write-Host 'Switched to HOOK mode.' -ForegroundColor Green
    Show-Status
}

if ($Mode -eq 'wrap') {
    if (-not (Test-Path $alias)) { Write-Host 'Already in WRAP mode.' -ForegroundColor Yellow; Show-Status; return }
    Rename-Item $alias 'd3d11.dll'
    if (Test-Path $dxgiOff) { Move-Item $dxgiOff $dxgi -Force }
    Set-IniKey $ini 'load_library_redirect' '2'
    Write-Host 'Switched back to WRAP mode (the known-good stereo setup).' -ForegroundColor Green
    Show-Status
}
