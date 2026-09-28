"""AKVR DIAG RB (2026-09-28): temporary geo-11 frame recording of the HUD draws, to find the RB icon's texture hash.
  python diag_rb.py on   -> hunting=1, analyse_frame = VK_F13, analyse_options = log, and
                            'analyse_options = dump_tex mono' in every [ShaderOverride_HUD*] section
  python diag_rb.py off  -> restore d3dx.ini from the backup taken by 'on'
"""
import sys, re, shutil, os
G = r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64"
ini = os.path.join(G, "d3dx.ini")
bak = os.path.join(G, "d3dx.ini.before-akvr-diag-rb")
TAG = "; AKVR DIAG RB"

if sys.argv[1] == "off":
    if os.path.exists(bak):
        shutil.copyfile(bak, ini); print("restored d3dx.ini")
    else:
        print("no backup - nothing to restore")
    sys.exit(0)

raw = open(ini, "rb").read()
nl = b"\r\n" if b"\r\n" in raw else b"\n"
s = raw.decode("latin1")
if TAG in s:
    print("already on"); sys.exit(0)
shutil.copyfile(ini, bak)
N = nl.decode()
# [Hunting]
s, n1 = re.subn(r"(?m)^hunting=0[ \t]*$", TAG + " (was hunting=0)" + N + "hunting=1", s, count=1)
assert n1 == 1, "hunting=0 not found"
s = s.replace("[Hunting]" + N, "[Hunting]" + N + TAG + ": geo-11 frame record on F13 (AKVR panel button), log only" + N +
              "analyse_frame = no_modifiers VK_F13" + N + "analyse_options = log" + N, 1)
# per HUD shader: dump the textures of HUD draws only
out, cur, added = [], None, 0
lines = s.split(N)
i = 0
while i < len(lines):
    ln = lines[i]
    out.append(ln)
    m = re.match(r"^\[(ShaderOverride_HUD[^\]]*)\]\s*$", ln)
    if m:
        # look ahead in this section for an existing analyse_options
        j = i + 1; has = False
        while j < len(lines) and not lines[j].startswith("["):
            if re.match(r"^\s*analyse_options\s*=", lines[j]): has = True
            j += 1
        if not has:
            out.append(TAG); out.append("analyse_options = dump_tex mono"); added += 1
    i += 1
s = N.join(out)
open(ini, "wb").write(s.encode("latin1"))
print(f"on: hunting=1, F13 record, dump_tex added to {added} HUD sections; backup {bak}")
