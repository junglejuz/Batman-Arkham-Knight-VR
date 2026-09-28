# Shader driver test (before any fix-shader edit reaches the game)

2026-09-28: a HUD shader edit (HUDSPLIT) wrote an undeclared output register and the NVIDIA driver
(nvwgf2umx.dll, 0xc0000409) killed the game at launch. Test every edited shader first:

1. Apply the patch script to a COPY of ShaderFixesDM (copy the script next to it).
2. Assemble: `"E:\Games\# MODS\Geo-11\geo-11+v0.6.90\cmd_Decompiler\cmd_Decompiler.exe" -a *-vs.txt`
   (writes `*.shdr`).
3. Build this tool once (`cmake -S . -B build && cmake --build build --config Release`), then run
   `vstest.exe <file.shdr>` per shader, one process each. Exit 0 = the driver accepted it; a crash
   (exit 127 / nonzero without "FAILED") = do not ship.
Validated: tonight's crashing set fails here, the fixed set and the originals pass.
