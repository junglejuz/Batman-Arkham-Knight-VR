"""Build the AKVR download package: akvr/dist/AKVR-ArkhamKnight/ and akvr/dist/AKVR-ArkhamKnight.zip.

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
    'AKVR-fix-patches.ps1': 'tools/AKVR-fix-patches.ps1',
    'dinput8.dll': 'build-dinput8/Release/dinput8.dll',
    'README.md': 'README.md',
    'files/d3d11.dll': 'install/files/d3d11.dll',
    'files/nvapi64.dll': 'install/files/nvapi64.dll',
    'files/d3dxdm.ini': 'install/files/d3dxdm.ini',
    'files/akvr_settings.ini': 'install/files/akvr_settings.ini',   # the tested setup, first install only
    'files/README.txt': 'install/files/README.txt',
}


def main():
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
