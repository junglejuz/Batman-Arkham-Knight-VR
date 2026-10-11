"""Build the AKVR download package (native stereo, 0.3.0 on): akvr/dist/AKVR-ArkhamKnight/ and akvr/dist/AKVR-ArkhamKnight.zip.

The zip has no top folder (Windows' "Extract All" already makes one named after the zip) and uses
standard forward-slash entry names. 2026-09-29: a zip made with PowerShell 5.1 (Compress-Archive /
.NET Framework ZipFile) stored "files\\d3d11.dll" with a backslash; 7-Zip opened it, but Windows'
built-in extractor refused the whole archive as "invalid" (JJ's test run).

Usage (from anywhere): python akvr/tools/make_package.py
"""
import os
import shutil
import zipfile

AKVR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(AKVR, 'dist')
PKG = os.path.join(DIST, 'AKVR-ArkhamKnight')
ZIP = PKG + '.zip'

FILES = {  # package path -> source path (relative to akvr/)
    'Install-AKVR.bat': 'install/Install-AKVR.bat',
    'Install-AKVR.ps1': 'install/Install-AKVR.ps1',
    'Uninstall-AKVR.bat': 'install/Uninstall-AKVR.bat',
    'Uninstall-AKVR.ps1': 'install/Uninstall-AKVR.ps1',
    'AKVR-Launch-VR.bat': 'install/AKVR-Launch-VR.bat',                  # the installer puts both in the game folder + shortcuts
    'AKVR-Launch-VR-SteamVR.bat': 'install/AKVR-Launch-VR-SteamVR.bat',
    'dinput8.dll': 'build-dinput8/Release/dinput8.dll',                  # head tracking, headset output, panel
    'XINPUT9_1_0.dll': 'native/build/Release/XINPUT9_1_0.dll',          # native stereo + DLSS
    'README.md': 'README.md',
    'LICENSE-luma-framework.md': 'LICENSE-luma-framework.md',            # Luma's licence: names must be kept
    'files/nvngx_dlss.dll': 'native/third_party/ngx/bin/nvngx_dlss.dll', # NVIDIA DLSS 310.9.1 (the tested copy)
    'files/akvr_settings.ini': 'install/files/akvr_settings.ini',       # the tested setup, first install only
    'files/README.txt': 'install/files/README.txt',
}


def main():
    # PACKAGING_GUIDE section 12: test tools behind one switch, off in the package
    with open(os.path.join(AKVR, 'install', 'files', 'akvr_settings.ini'), encoding='utf-8', errors='replace') as f:
        lines = [line.strip() for line in f]
        for bad in ('testtools=1', 'advancedpanel=1'):
            if bad in lines:
                raise SystemExit('refusing: install/files/akvr_settings.ini has ' + bad)
    if os.path.isdir(PKG):
        shutil.rmtree(PKG)
    for dst, src in FILES.items():
        s = os.path.join(AKVR, src)
        if not os.path.isfile(s):
            raise SystemExit('missing: ' + s)
        d = os.path.join(PKG, dst)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copy2(s, d)
    if os.path.exists(ZIP):
        os.remove(ZIP)
    with zipfile.ZipFile(ZIP, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dst in sorted(FILES):
            z.write(os.path.join(PKG, dst), dst)          # dst always uses '/'
    with zipfile.ZipFile(ZIP) as z:
        bad = z.testzip()
        raw = open(ZIP, 'rb').read()
        if bad or b'files\\' in raw:
            raise SystemExit('zip check failed: ' + str(bad or 'backslash entry name'))
        print('%s: %d files, %.1f MB, check OK' % (ZIP, len(z.namelist()), os.path.getsize(ZIP) / 2**20))


if __name__ == '__main__':
    main()
