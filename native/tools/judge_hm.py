"""HM judge (2026-10-08): MARKERBASE / MARKERMATCH / HUDSIZE / SAMEDIR / WHEELEYE / MAPFLAT from one Run-HM launch.
Usage: python judge_hm.py [akn folder] -> prints the measurements and writes hm_*.png crops beside them."""
import re, sys, os, json
import numpy as np
from PIL import Image

AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()


def section(tag):
    """PROJ lines logged after 'capmk <tag>'"""
    out, on = [], False
    for l in log:
        if f"MK3 capture {tag}:" in l: on = True; continue
        if on and "MK3 capture" in l: break
        if on and "PROJ ra" in l: out.append(l)
    return out


print("== 1. markers (PROJ: answer given to the game / the game's own left-eye answer)")
for t in ("hm0", "hm15", "hmn15", "hmn15b0"):
    ps = section(t)
    seen = {}
    for l in ps:
        m = re.search(r"world (\S+) (\S+) (\S+) -> (\S+) (\S+) depth \S+ w \S+ \(game's own (\S+) (\S+)\)", l)
        if m and m.group(1, 2, 3) not in seen: seen[m.group(1, 2, 3)] = (m.group(4, 5), m.group(6, 7))
    print(f"  {t}: {len(ps)} PROJ lines; " + "; ".join(f"world {' '.join(k)} -> {v[0][0]} {v[0][1]} (own {v[1][0]} {v[1][1]})" for k, v in list(seen.items())[:3]))


def alpha(tag):
    p = os.path.join(AKN, f"hud_alpha_{tag}.bmp")
    return np.asarray(Image.open(p).convert("L"), dtype=np.float32) / 255 if os.path.exists(p) else None


def blobs(a, thr=0.03):
    if a is None: return "no sheet"
    H, W = a.shape
    c = a[: H // 20 * 20, : W // 40 * 40].reshape(H // 20, 20, W // 40, 40).max(axis=(1, 3)) > thr
    from collections import deque
    seen = np.zeros_like(c); out = []
    for y in range(c.shape[0]):
        for x in range(c.shape[1]):
            if c[y, x] and not seen[y, x]:
                q = deque([(y, x)]); seen[y, x] = 1; ys = []; xs = []
                while q:
                    yy, xx = q.popleft(); ys.append(yy); xs.append(xx)
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            ny, nx = yy + dy, xx + dx
                            if 0 <= ny < c.shape[0] and 0 <= nx < c.shape[1] and c[ny, nx] and not seen[ny, nx]: seen[ny, nx] = 1; q.append((ny, nx))
                out.append(f"u {min(xs) * 40 / W:.3f}-{(max(xs) + 1) * 40 / W:.3f} v {min(ys) * 20 / H:.3f}-{(max(ys) + 1) * 20 / H:.3f}")
    return f"{W}x{H}: " + " | ".join(out[:8])


print("== 1b. HUD sheet pieces (must not move between head turns with markerbase 1)")
for t in ("mmain", "hm0", "hm15", "hmn15", "hmn15b0", "hmpause", "hmmap1", "hmmap0"):
    print(f"  {t}: {blobs(alpha(t))}")


def pair(tag):
    p = os.path.join(AKN, f"pair_{tag}.bmp")
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float32) if os.path.exists(p) else None


print("== 2. lens modes facing Batman: bright share per eye (whole / centre third)")
for t in ("rl2a", "rl2b", "rl3a", "rl3b"):
    p = pair(t)
    if p is None: print(f"  {t}: missing"); continue
    W = p.shape[1] // 2
    res = []
    for e in (0, 1):
        im = p[:, e * W:(e + 1) * W].mean(axis=2)
        h, w = im.shape
        c = im[h // 3: 2 * h // 3, w // 3: 2 * w // 3]
        res.append((float((im > 140).mean()), float((c > 140).mean()), float(im.mean())))
    print(f"  {t}: L whole {res[0][0]:.4f} centre {res[0][1]:.4f} mean {res[0][2]:.1f} | R whole {res[1][0]:.4f} centre {res[1][1]:.4f} mean {res[1][2]:.1f}")

print("== 3/4. headset shots")
for t in ("xr_mmain", "xr_hm0", "xr_hm15", "xr_rl2", "xr_rl3", "xr_hmpause", "xr_hmmap1", "xr_hmmap1t", "xr_hmmap0"):
    p = os.path.join(AKN, f"{t}.shot.json")
    if not os.path.exists(p): print(f"  {t}: no shot"); continue
    try: s = json.load(open(p, encoding="utf-8-sig"))
    except Exception as ex: print(f"  {t}: unreadable {ex}"); continue
    print(f"  {t}: layers {s.get('Layers')} quads {s.get('QuadLayers')} luma L {s.get('MeanLumaL')} R {s.get('MeanLumaR')}")
