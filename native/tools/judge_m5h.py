"""AKN stage 5h judge: rain while paused (same camera both eyes) + hiding the 103530-vertex draw."""
import sys, os
import numpy as np
from PIL import Image
AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
def ld(t):
    p = os.path.join(AKN, f"pair_{t}.bmp")
    if not os.path.exists(p): return None
    a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32); w = a.shape[1] // 2; return a[:, :w], a[:, w:]
def dmap(a, b, name):
    d = np.abs(a - b).mean(axis=2); Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8)).save(os.path.join(AKN, name)); return float(d.mean())
def streak(im):
    g = im.mean(axis=2); hp = g - (np.roll(g, 2, 1) + np.roll(g, -2, 1)) / 2; return float((hp > 12).mean() * 100)
out = []
p1, q1, q2, s0, s1 = (ld(t) for t in ("hp1", "hq1", "hq2", "hs0", "hs1"))
if p1: out.append(f"playing, same camera: left-right difference {dmap(p1[0], p1[1], 'H_play_lr.png'):.3f}")
if q1: out.append(f"PAUSED, same camera: left-right difference {dmap(q1[0], q1[1], 'H_pause_lr.png'):.3f}")
if q1 and q2: out.append(f"PAUSED, left eye 4 s apart: change {dmap(q1[0], q2[0], 'H_pause_time.png'):.3f} (rain moving while paused if clearly above 0)")
if s0 and s1:
    out.append(f"hide vs 62c3cb6af9d6796c: thin-streak pixels left eye {streak(s0[0]):.2f}% -> {streak(s1[0]):.2f}%, picture change {dmap(s0[0], s1[0], 'H_hide_diff.png'):.3f}")
print("\n".join(out)); open(os.path.join(AKN, "M5h_result.txt"), "w").write("\n".join(out) + "\n")
