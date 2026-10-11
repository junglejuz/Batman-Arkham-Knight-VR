"""MB judge (MIPBIAS): per mode, texture detail of the left eye's lower half (ground + Batman) and the still-head
change between two pictures 1 s apart (shimmer). Also the log's swap / twin telemetry.
Usage: python judge_mb.py [akn folder]  -> prints, writes MB_result.txt and MB_crops.png"""
import re, sys, os
import numpy as np
from PIL import Image

AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = []
def load(n):
    p = os.path.join(AKN, n)
    return np.asarray(Image.open(p).convert("L"), dtype=np.float32) if os.path.exists(p) else None
def detail(g):   # mean absolute Laplacian = fine detail
    return float(np.abs(4 * g[1:-1, 1:-1] - g[:-2, 1:-1] - g[2:, 1:-1] - g[1:-1, :-2] - g[1:-1, 2:]).mean())
crops = []
res = {}
for m in (0, 1, 2):
    a, b = load(f"pair_mb{m}a.bmp"), load(f"pair_mb{m}b.bmp")
    if a is None or b is None: out.append(f"mode {m}: MISSING pictures"); continue
    w = a.shape[1] // 2; H = a.shape[0]
    A, B = a[H // 2:, :w], b[H // 2:, :w]
    d, ch = detail(A), float(np.abs(A - B).mean())
    res[m] = (d, ch)
    out.append(f"mode {m} ({['off', 'full', 'mild'][m]}): detail {d:.2f}, still-head change {ch:.2f}")
    crops.append(Image.open(os.path.join(AKN, f"pair_mb{m}a.bmp")).convert("RGB").crop((int(w * .35), int(H * .55), int(w * .35) + 600, int(H * .55) + 600)))
if 0 in res and 1 in res:
    out.append(f"VERDICT full vs off: detail x{res[1][0] / max(res[0][0], 1e-6):.2f}, shimmer x{res[1][1] / max(res[0][1], 1e-6):.2f}")
sw = [l for l in log if "mip bias" in l and "sampler swaps/s" in l]
tw = [l for l in log if "MIPBIAS: twin" in l]
out.append(f"telemetry: {len(tw)} twin lines (first: {tw[0][13:] if tw else 'none'}); last rate line: {sw[-1].split('|')[-1].strip() if sw else 'none'}")
if crops:
    o = Image.new("RGB", (600 * len(crops), 600))
    for i, c in enumerate(crops): o.paste(c, (600 * i, 0))
    o.save(os.path.join(AKN, "MB_crops.png"))
txt = "\n".join(out); print(txt)
open(os.path.join(AKN, "MB_result.txt"), "w", encoding="utf-8").write(txt + "\n")
