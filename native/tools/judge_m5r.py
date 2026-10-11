"""AKN stage 5r judge (RAINDEPTH): rain-only paused pairs; right eye warped onto the left at infinity (turned eyes),
then per-tile horizontal disparity of the rain. Flat rain (each eye relative to itself) = disparity ~0 everywhere."""
import sys, os, re, math
import numpy as np
from PIL import Image
AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = [l[13:].strip()[:420] for l in log if "RAINDEPTH eye" in l][:2]
tp = {}
for l in log:
    m = re.search(r"eye (\d) turned lens: yaw ([-\d.]+) pitch ([-\d.]+) deg, half-angles ([-\d.]+) x ([-\d.]+)", l)
    if m: tp[int(m.group(1))] = [math.radians(float(m.group(i))) for i in (2, 3, 4, 5)]
def eye_frame(yc, pc):
    cy, sy, cp, sp = math.cos(yc), math.sin(yc), math.cos(pc), math.sin(pc)
    return np.array([cy, 0.0, -sy]), np.array([-sy * sp, cp, -cy * sp]), np.array([sy * cp, sp, cy * cp])
def disparity(tag):
    p = os.path.join(AKN, f"pair_{tag}.bmp")
    if not os.path.exists(p) or 0 not in tp or 1 not in tp: return None
    a = np.asarray(Image.open(p).convert("L"), dtype=np.float32); w = a.shape[1] // 2
    E0, E1 = a[::2, :w:2], a[::2, w::2]; H, W = E0.shape
    (y0, p0, hh, hv), (y1, p1, _, _) = tp[0], tp[1]
    r0, u0, f0 = eye_frame(y0, p0); r1, u1, f1 = eye_frame(y1, p1)
    jj, ii = np.mgrid[0:H, 0:W]
    x = (2 * (ii + 0.5) / W - 1) * math.tan(hh); y = (1 - 2 * (jj + 0.5) / H) * math.tan(hv)
    ray = x[..., None] * r0 + y[..., None] * u0 + f0
    a1, b1, c1 = ray @ r1, ray @ u1, ray @ f1
    px = ((a1 / c1) / math.tan(hh) + 1) * W / 2; py = (1 - (b1 / c1) / math.tan(hv)) * H / 2
    ok = (c1 > 0) & (px >= 0) & (px < W - 1) & (py >= 0) & (py < H - 1)
    R = E1[np.clip(py.astype(int), 0, H - 1), np.clip(px.astype(int), 0, W - 1)] * ok
    hp = lambda im: im - (np.roll(im, 3, 0) + np.roll(im, -3, 0) + np.roll(im, 3, 1) + np.roll(im, -3, 1)) / 4
    L_, R_ = np.clip(hp(E0), 0, None), np.clip(hp(R), 0, None)
    an = np.stack([np.clip(E0 * 2, 0, 255), np.clip(R * 2, 0, 255), np.clip(R * 2, 0, 255)], -1).astype(np.uint8)
    Image.fromarray(an).save(os.path.join(AKN, f"R_{tag}_anaglyph.png"))
    ds = []; T = 48; S = 40
    for ty in range(S, H - T - S, T):
        for tx in range(S, W - T - S, T):
            blk = L_[ty:ty + T, tx:tx + T]
            if blk.sum() < 2000 or not ok[ty:ty + T, tx:tx + T].all(): continue
            best = None
            for dx in range(-S, S + 1):
                c = float((blk * R_[ty:ty + T, tx + dx:tx + dx + T]).sum())
                if best is None or c > best[0]: best = (c, dx)
            ds.append(best[1])
    if not ds: return "no rain tiles"
    d = np.array(ds)
    return f"{len(d)} rain tiles: disparity (half-size px) median {np.median(d):.1f}, spread (10-90 %) {np.percentile(d, 10):.1f} .. {np.percentile(d, 90):.1f}, share at ~0 (|d|<=1) {np.mean(np.abs(d) <= 1) * 100:.0f} %"
for t in ("rd0", "rd1", "rd1run"):
    out.append(f"{t}: {disparity(t)}")
dl = [l for l in log if "rain depth draws" in l]; out.append(dl[-1][13:].strip()[-30:] if dl else "no depth telemetry")
crashes = [l for l in log if "CRASH RECORDER" in l and "XINPUT9" in l.upper().split("| RSP")[0]]
out.append(f"AKN crash records: {len(crashes)}")
print("\n".join(out)); open(os.path.join(AKN, "M5r_result.txt"), "w").write("\n".join(out) + "\n")
