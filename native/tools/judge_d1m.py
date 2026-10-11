"""DLSS D1m judge (CAMMV): per motion mode - still-head shimmer (two pictures 2 s apart, background change) and
turning-head sharpness (detail energy while turning / while still). Left eye, top 45 % of the picture (background)."""
import os, re
import numpy as np
from PIL import Image
AKN = r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
def eye(t):
    p = os.path.join(AKN, f"pair_{t}.bmp")
    if not os.path.exists(p): return None
    a = np.asarray(Image.open(p).convert("L"), dtype=np.float32); w = a.shape[1] // 2
    return a[: int(a.shape[0] * 0.45), :w]
def detail(im):
    lap = 4 * im[1:-1, 1:-1] - im[:-2, 1:-1] - im[2:, 1:-1] - im[1:-1, :-2] - im[1:-1, 2:]
    return float(np.abs(lap).mean())
out = []
log = open(os.path.join(AKN, "akn_log_D1m.txt"), encoding="utf-8", errors="replace").read().splitlines()
out += [l[13:].strip() for l in log if "camera motion" in l][:3]
for m, name in ((0, "game buffer only"), (1, "camera motion only"), (2, "camera + moving objects")):
    a, b, o = eye(f"m{m}sa"), eye(f"m{m}sb"), eye(f"m{m}o")
    if a is None or b is None or o is None: out.append(f"mode {m}: pictures missing"); continue
    shim = float(np.abs(a - b).mean()); da, do = detail(a), detail(o)
    out.append(f"mode {m} ({name}): still-head change {shim:.2f} | detail still {da:.2f}, turning {do:.2f} (kept {do / da * 100:.0f} %)")
print("\n".join(out)); open(os.path.join(AKN, "D1m_result.txt"), "w").write("\n".join(out) + "\n")
