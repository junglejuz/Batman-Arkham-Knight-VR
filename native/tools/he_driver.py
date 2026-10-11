"""HE driver (2026-10-09): find the detective-mode key (blue tint), then view-state snapshots normal / detective / after,
the detective-only pixel shaders hidden one by one (vignette hunt), and the after-exit pictures (right eye stuck?).
Usage: python he_driver.py <akn folder> <tools folder>"""
import os, re, sys, time, shutil, subprocess
import numpy as np
from PIL import Image

AKN, TOOLS = sys.argv[1], sys.argv[2]
CMD = os.path.join(AKN, "command.txt")
out = []


def say(t):
    t = time.strftime("%H:%M:%S ") + t; print(t, flush=True); out.append(t)


def cmd(c, wait=1.0):
    with open(CMD, "w") as f: f.write(c + "\n")
    for _ in range(40):
        if not os.path.exists(CMD): break
        time.sleep(0.1)
    time.sleep(wait)


def key(vk):
    subprocess.run([sys.executable, os.path.join(TOOLS, "akmenu.py"), "key", hex(vk)], capture_output=True)


def pairshot(tag):
    p = os.path.join(AKN, f"pair_{tag}.bmp")
    if os.path.exists(p): os.remove(p)
    cmd(f"pairshot {tag}", 0.5)
    for _ in range(80):
        if os.path.exists(p) and os.path.getsize(p) > 1000:
            s0 = os.path.getsize(p); time.sleep(0.4)
            if os.path.getsize(p) == s0: break
        time.sleep(0.1)
    try:
        a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)
    except Exception:
        return None
    L = a[:, : a.shape[1] // 2]
    r, g, b = L[..., 0].mean(), L[..., 1].mean(), L[..., 2].mean()
    return (b / max(r + g, 1.0), float(L.mean()), float(np.abs(a[:, : a.shape[1] // 2] - a[:, a.shape[1] // 2:]).mean()))


def psinv(name):
    inv = os.path.join(AKN, "ps_inventory.txt")
    if os.path.exists(inv): os.remove(inv)
    cmd("psinv 4", 1.0)
    for _ in range(40):
        if os.path.exists(inv): break
        time.sleep(0.2)
    time.sleep(0.3)
    if not os.path.exists(inv): return set()
    dst = os.path.join(AKN, f"ps_{name}.txt"); shutil.copy(inv, dst)
    return set(l.strip() for l in open(dst) if l.strip())


cmd("mark HE detective", 0.5)
base = pairshot("he_base"); say(f"base: blue ratio / mean / L-R {base}")
found = None
for name, vk in (("V", 0x56), ("Q", 0x51), ("R", 0x52)):
    key(vk); time.sleep(2.5)
    m = pairshot(f"he_key{name}"); say(f"key {name}: {m}")
    if m and base and m[0] > base[0] * 1.25:
        found = (name, vk); break
    key(vk); time.sleep(1.5)   # a toggle that did something else: press it back
say(f"detective key: {found}")
if found:
    key(found[1]); time.sleep(3)   # back to normal
    cmd("vssnap normal", 1.0); ps_n = psinv("normal"); n0 = pairshot("he_normal")
    key(found[1]); time.sleep(3)
    cmd("vssnap det", 1.0); ps_d = psinv("det"); d0 = pairshot("he_det")
    only = sorted(ps_d - ps_n)
    say(f"normal {n0} / detective {d0}; {len(only)} shaders only in detective mode: {' '.join(only)}")
    for h in only[:20]:
        cmd(f"skipps {h}", 1.0); m = pairshot(f"he_skip_{h[:8]}"); say(f"  hide {h}: {m}")
    cmd("skipps 0", 1.0)
    key(found[1]); time.sleep(4)
    cmd("vssnap after", 1.0); a1 = pairshot("he_after"); time.sleep(3); a2 = pairshot("he_after2")
    say(f"after exit: {a1} / {a2} (base {base})")
cmd("mark HE end", 0.5)
open(os.path.join(AKN, "he_result.txt"), "w").write("\n".join(out) + "\n")
