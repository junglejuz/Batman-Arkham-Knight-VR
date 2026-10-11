"""HB judge (HUDBLEND): is the HUD texture premultiplied with true coverage? A premultiplied pixel never has a colour
channel above its alpha; AKVR's layer divides colour by alpha, so such pixels blow up into flat bands (JJ's
"posterised" weapon wheel). Per capture hb0 (fix off) / hb1 (on): pixels with colour > alpha + 3 among HUD pixels,
and a preview of AKVR's conversion over the left eye (HB_preview.png: off | on).
Usage: python judge_hb.py [akn folder]"""
import sys, os
import numpy as np
from PIL import Image

AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
out, prev = [], []
for tag in ("hb0", "hb1"):
    pr, pa, pp = (os.path.join(AKN, f) for f in (f"hud_rgb_{tag}.bmp", f"hud_alpha_{tag}.bmp", f"pair_{tag}.bmp"))
    if not (os.path.exists(pr) and os.path.exists(pa)): out.append(f"{tag}: MISSING"); continue
    rgb = np.asarray(Image.open(pr).convert("RGB"), dtype=np.float32); a = np.asarray(Image.open(pa).convert("L"), dtype=np.float32)
    hud = (a > 0) | (rgb.max(axis=2) > 0)
    bad = hud & (rgb.max(axis=2) > a + 3)
    over = (rgb.max(axis=2) - a)[bad]
    out.append(f"{tag} ({'fix off' if tag == 'hb0' else 'fix on'}): HUD pixels {int(hud.sum())}, colour above alpha {int(bad.sum())} ({100.0 * bad.sum() / max(hud.sum(), 1):.1f} %), mean overshoot {over.mean() if over.size else 0:.1f}")
    if os.path.exists(pp):   # AKVR's way: straight = c / a (clamped), blended over the eye
        p = np.asarray(Image.open(pp).convert("RGB"), dtype=np.float32); L = p[:, : p.shape[1] // 2]
        if L.shape[:2] == rgb.shape[:2]:
            af = a[..., None] / 255.0
            st = np.where(af > 0.004, np.clip(rgb / np.maximum(af, 1e-3), 0, 255), rgb)
            comp = st * af + L * (1 - af) if True else None
            comp = np.where(af > 0.004, comp, np.clip(L + rgb, 0, 255))
            y0, y1, x0, x1 = 700, 1900, 900, 2320
            prev.append(Image.fromarray(comp[y0:y1, x0:x1].astype(np.uint8)))
if len(prev) == 2:
    o = Image.new("RGB", (prev[0].width * 2, prev[0].height)); o.paste(prev[0], (0, 0)); o.paste(prev[1], (prev[0].width, 0))
    o.resize((o.width // 2, o.height // 2)).save(os.path.join(AKN, "HB_preview.png"))
txt = "\n".join(out); print(txt)
open(os.path.join(AKN, "HB_result.txt"), "w", encoding="utf-8").write(txt + "\n")
