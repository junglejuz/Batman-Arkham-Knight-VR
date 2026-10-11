"""AKN stage 5o judge (MARKERPROBE): world-to-screen callers per pose + HUD coverage per pose."""
import sys, os, re, collections
import numpy as np
from PIL import Image
AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = [l[13:].strip() for l in log if "FSceneView::Project" in l][:2]
cal = [l for l in log if "world-to-screen callers" in l]; out += [c[13:].strip() for c in cal[-3:]]
cur = None; per = collections.OrderedDict()
for l in log:
    m = re.search(r"MARK O pose (\d+) \((.*)\)", l)
    if m: cur = f"pose {m.group(1)} head {m.group(2)}"; per[cur] = []
    if "PROJ ra" in l and cur: per[cur].append(l[13:].strip())
for k, v in per.items():
    ras = collections.Counter(re.search(r"ra (\w+)", x).group(1) + " " + re.search(r"\((\w[\w ]*?)\)", x).group(1) for x in v)
    out.append(f"{k}: {len(v)} projections, by caller/view {dict(ras)}")
    seen = set()
    for x in v:
        ra = re.search(r"ra (\w+)", x).group(1)
        if ra not in seen: seen.add(ra); out.append("    e.g. " + x)
for i in range(7):
    p = os.path.join(AKN, f"hud_alpha_mk{i}.bmp")
    if os.path.exists(p):
        a = np.asarray(Image.open(p).convert("L")); out.append(f"HUD mk{i}: coverage {(a > 0).mean() * 100:.2f} %")
print("\n".join(out)); open(os.path.join(AKN, "M5o_result.txt"), "w").write("\n".join(out) + "\n")
