"""R1 judge (Batman reflections per eye): same spot + frame, turned eyes; right eye warped onto the left must match.
Pairs: pair_rctl (same camera control), pair_rs<m> still / pair_rm<m> head sweep for DLSS motion mode m = 1, 2, 0.
Writes R1_<tag>_diff.png maps and R1_result.txt.  Usage: python judge_r1.py [akn folder]"""
import re, sys, os, math
import numpy as np
from PIL import Image

AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = []
def load(name):
    p = os.path.join(AKN, name)
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float32) if os.path.exists(p) else None
def eye_frame(yc, pc):
    cy, sy, cp, sp = math.cos(yc), math.sin(yc), math.cos(pc), math.sin(pc)
    return np.array([cy, 0.0, -sy]), np.array([-sy * sp, cp, -cy * sp]), np.array([sy * cp, sp, cy * cp])
tp = {}
for l in log:
    m_ = re.search(r"eye (\d) turned lens: yaw ([-\d.]+) pitch ([-\d.]+) deg, half-angles ([-\d.]+) x ([-\d.]+)", l)
    if m_: tp[int(m_.group(1))] = [math.radians(float(m_.group(i))) for i in (2, 3, 4, 5)]
ctl = None
c = load("pair_rctl.bmp")
if c is not None:
    w = c.shape[1] // 2; ctl = float(np.abs(c[::2, :w:2] - c[::2, w::2]).mean())
    out.append(f"control (same camera and lens): difference {ctl:.2f}")
else: out.append("control: MISSING pair_rctl.bmp")
if not (0 in tp and 1 in tp): out.append("no 'turned lens' lines for both eyes - cannot warp")
else:
    (y0, p0_, hh, hv), (y1, p1_, _, _) = tp[0], tp[1]
    r0, u0, f0 = eye_frame(y0, p0_); r1, u1, f1 = eye_frame(y1, p1_)
    for tag, what in (("rs1", "still, camera motion (mode 1, default)"), ("rm1", "head sweep, mode 1"),
                      ("rs2", "still, camera + moving objects (mode 2)"), ("rm2", "head sweep, mode 2"),
                      ("rs0", "still, game buffer only (mode 0)"), ("rm0", "head sweep, mode 0")):
        pz = load(f"pair_{tag}.bmp")
        if pz is None: out.append(f"{tag} {what}: MISSING"); continue
        w5 = pz.shape[1] // 2; E0, E1 = pz[::2, :w5:2], pz[::2, w5::2]
        H5, W5 = E0.shape[:2]
        jj, ii = np.mgrid[0:H5, 0:W5]
        x = (2 * (ii + 0.5) / W5 - 1) * math.tan(hh); y = (1 - 2 * (jj + 0.5) / H5) * math.tan(hv)
        ray = x[..., None] * r0 + y[..., None] * u0 + f0
        a1, b1, c1 = ray @ r1, ray @ u1, ray @ f1
        px = ((a1 / c1) / math.tan(hh) + 1) * W5 / 2; py = (1 - (b1 / c1) / math.tan(hv)) * H5 / 2
        ok_ = (c1 > 0) & (px >= 0) & (px < W5 - 1) & (py >= 0) & (py < H5 - 1)
        warped = E1[np.clip(py.astype(int), 0, H5 - 1), np.clip(px.astype(int), 0, W5 - 1)]
        gm = np.zeros((H5, W5)); gm[ok_] = np.abs(E0[ok_] - warped[ok_]).mean(axis=1)
        dw = float(gm[ok_].mean())
        # worst 64-px blocks (where the eyes disagree most), as fractions of the left eye's picture
        B = 64; hb, wb = H5 // B, W5 // B
        blk = gm[:hb * B, :wb * B].reshape(hb, B, wb, B).mean(axis=(1, 3))
        okb = ok_[:hb * B, :wb * B].reshape(hb, B, wb, B).mean(axis=(1, 3)) > 0.9
        blk[~okb] = 0
        top = np.argsort(blk.ravel())[::-1][:4]
        spots = ", ".join(f"({(t % wb + .5) / wb:.2f},{(t // wb + .5) / hb:.2f})={blk.ravel()[t]:.1f}" for t in top)
        Image.fromarray(np.clip(gm * 4, 0, 255).astype(np.uint8)).save(os.path.join(AKN, f"R1_{tag}_diff.png"))
        out.append(f"{tag} {what}: difference {dw:.2f} (control {ctl if ctl is not None else float('nan'):.2f}; no-DLSS T17b 2026-10-07: 5.37 vs 2.26); worst spots {spots}")
txt = "\n".join(out); print(txt)
open(os.path.join(AKN, "R1_result.txt"), "w", encoding="utf-8").write(txt + "\n")
