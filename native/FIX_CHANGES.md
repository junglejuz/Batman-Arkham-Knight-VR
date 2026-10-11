# AKN (engine stereo) - changes to game / third-party files

- 2026-10-08 DLSS D0b test (temporary, restored by the test script): `BmGame\Config\BmSystemSettings.ini`
  [SystemSettings] `ScreenPercentage=100.000000` -> `67.000000` for one fake-headset launch; backup
  `Binaries\Win64\akn\BmSystemSettings_before_D0b.ini`. 2026-10-08 D0: `akvr_settings.ini` engineres 2860 -> 1916
  for one launch (AKVR's own file), restored.
- 2026-10-08 after D0/D0b: the invalid 67 % run had saved a wrong eye shape and size -> restored by hand:
  `akvr_settings.ini` engineshape 813 -> 925 (JJ's value before D0); `BmSystemSettings.ini` [SystemSettings]
  ResX/ResY 1552/1916 -> 2640/2860 (the full eye size AKVR forces anyway).

## 2026-10-08 LENSFLAREOFF (game config, by AKVR at start-up)
- BmGame\Config\BmSystemSettings.ini (+ the other config files AKVR's comfort list covers): every `LensFlares=True` and
  `DefaultLensFlares=True` line forced to `False` (earlyres.cpp kComfortKeys). JJ: lens flares to be removed (left eye
  only flare ring, capture c194645). Original kept as `*.akvr-original` (first edit only). Applies to both versions.
- Test-only, restored byte-exact afterwards (no lasting change): RF3 runs set AllowImageReflections=False and
  Reflections=False for one launch each (backup `BmSystemSettings.ini.rf3-backup`, removed after restore).

## 2026-10-10 FLAREOFF (no file change) - note on LENSFLAREOFF
- The LENSFLAREOFF config edit above did NOT remove AK's flare rings (JJ 2026-10-10). It stays (harmless). The flares are
  now removed in memory by AKN: the game's shaders 0x2CCEAEA7 / 0xB42A7F40 are bound as Luma's copies with lens flare and
  lens dirt off. No game file is changed; `flareoff 0` in akn\command.txt brings the game's own back.

## 2026-10-11 SCR30 (TEST, game config) - why the title / loading screens run at ~30 frames/s
- BmGame\Config\PC\PCEngine.ini [Engine.GameEngine] `MaxSmoothedFrameRate=30` -> `120` (the generated BmEngine.ini takes
  it from here; it is rewritten every launch, so the source file is edited).
- BmGame\Config\DefaultEngine.ini [Engine.Engine] `FrameRateLimitingSetting=7` -> `0` (comment "@ROCK - Dustin: Limit
  MaxFPS to 30"; meaning of the values unknown). One byte changed, file encoding and line endings kept (the Edit tool had
  re-saved it as UTF-8 - redone byte-exact from the backup).
- Backups: Binaries\Win64\akvr_fix_backup\DefaultEngine.ini.before-scr30, PCEngine.ini.before-scr30. Restore both if the
  test shows no change (TEST PLAN SCR30 in NOTES.md).
- 2026-10-11 ~14:12 SCR30 RESULT: title still 28 frames/s with both changes -> neither setting is the cap. BOTH FILES
  RESTORED byte-exact from the backups (cmp identical); backups deleted. No lasting change.
