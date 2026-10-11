"""AKN rain bisection (2026-10-08, JJ: rain differs per eye). With both eyes on the same camera + frame number, hide
half of the vertex shaders (both eyes) and keep the half whose hiding brings the left/right difference DOWN the most,
until one shader is left = the draw that differs between the eyes. Drives AKN through akn\\command.txt.
Usage: python bisect_vs.py <akn folder>   -> prints and writes bisect_result.txt"""
import os, sys, time
import numpy as np
from PIL import Image

AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
CMD = os.path.join(AKN, "command.txt")
out = []
def say(t):
    t = time.strftime("%H:%M:%S ") + t; print(t, flush=True); out.append(t)
def alive():
    return os.system('tasklist /FI "IMAGENAME eq BatmanAK.exe" | find /I "BatmanAK.exe" >nul') == 0
def cmd(c, wait=1.0):
    with open(CMD, "w") as f: f.write(c + "\n")
    for _ in range(40):
        if not os.path.exists(CMD): break
        time.sleep(0.1)
    time.sleep(wait)
n_shot = [0]
def diff():
    """pair picture now -> mean left/right difference (both eyes the same camera)"""
    n_shot[0] += 1; tag = f"bs{n_shot[0]}"; p = os.path.join(AKN, f"pair_{tag}.bmp")
    if os.path.exists(p): os.remove(p)
    cmd(f"pairshot {tag}", 0.5)
    for _ in range(80):
        if os.path.exists(p) and os.path.getsize(p) > 1000:
            s0 = os.path.getsize(p); time.sleep(0.4)
            if os.path.getsize(p) == s0: break
        time.sleep(0.1)
    try:
        a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)
    except Exception as e:
        say(f"no picture ({e})"); return None
    w = a.shape[1] // 2; d = float(np.abs(a[:, :w] - a[:, w:]).mean())
    os.remove(p)
    return d
def diff_avg(k=2):
    v = [d for d in (diff() for _ in range(k)) if d is not None]
    return float(np.mean(v)) if v else None

cmd("eyesame 1", 2); cmd("framesync 1", 2)
inv = os.path.join(AKN, "vs_inventory.txt")
if os.path.exists(inv): os.remove(inv)
cmd("vsinv 6", 4)
hs = sorted(int(l, 16) for l in open(inv) if l.strip()) if os.path.exists(inv) else []
say(f"{len(hs)} vertex shaders in the eye renders")
base = diff_avg(3); say(f"nothing hidden: difference {base}")
cand = hs
level = 0
while len(cand) > 1 and alive():
    level += 1
    A, B = cand[:len(cand) // 2], cand[len(cand) // 2:]
    cmd(f"skiprange {A[0]:016x} {A[-1]:016x}", 1.5); dA = diff_avg()
    cmd(f"skiprange {B[0]:016x} {B[-1]:016x}", 1.5); dB = diff_avg()
    say(f"level {level}: hide {len(A)} shaders -> {dA}, hide the other {len(B)} -> {dB}")
    if dA is None or dB is None: break
    cand = A if dA < dB else B
cmd("skiprange 0 0", 1)
if len(cand) == 1:
    h = cand[0]
    cmd(f"skipvs {h:016x}", 1.5); d1 = diff_avg(3); cmd("skipvs 0", 1.5); d0 = diff_avg(3)
    say(f"RESULT: vertex shader {h:016x}: hidden -> difference {d1}, shown -> {d0} (paused reference 0.020)")
cmd("eyesame 0", 1)
open(os.path.join(AKN, "bisect_result.txt"), "w").write("\n".join(out) + "\n")
