"""AKN stage 5d judge (JJ's 6th Quest test: the VR panel will not open; the rain differs per eye).
Inputs: akn_log.txt (telemetry 'rain steps/s ... | VR panel state'), headset shots xr_panel / xr_pclosed / xr_pkey,
captures pair_rs0 / rs1 / rs3 (same camera both eyes) and pair_rn3 / rn0 (normal stereo).
Usage: python judge_m5d.py [akn folder]   -> prints and writes M5d_result.txt"""
import re, sys, os, json
import numpy as np
from PIL import Image

AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
out = []
def res(name, ok, detail, nxt_fail, nxt_pass=""):
    out.append(f"{name}: {'PASS' if ok else 'FAIL'} - {detail}" + (f"  -> NEXT: {nxt_fail}" if not ok else (f"  -> NEXT: {nxt_pass}" if nxt_pass else "")))
def mark(text):
    for i, l in enumerate(log):
        if "MARK" in l and text in l: return i
    return None
def between(a, b):
    i, j = mark(a), mark(b)
    return log[i:j] if i is not None and j is not None else []
def load(name):
    p = os.path.join(AKN, name)
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float32) if os.path.exists(p) else None
def side(name):
    p = os.path.join(AKN, name + ".json")   # xrsim-shot's own sidecar (layer list), as judge_m5c
    try: return json.load(open(p, encoding="utf-8-sig"))
    except Exception: return None
RX = re.compile(r"simulation left (\d+) right (\d+) outside (\d+), splashes left (\d+) right (\d+) outside (\d+) \(once (\d+), skipped (\d+)\) \| VR panel state (\w+) \(shown (\d), ImGui (\d), drew (\d), to headset (\d), pad (\d), chord down (\d), buttons (\w+), chord fires (\d+)\)")
def tele(lines): return [tuple(m.groups()) for l in lines for m in [RX.search(l)] if m]

# D0 telemetry present
allt = tele(log)
res("D0 telemetry", len(allt) > 5, f"{len(allt)} rain/panel lines; AKVR shader names {'reached' if any(int(t[0]) + int(t[1]) + int(t[2]) + int(t[3]) + int(t[4]) + int(t[5]) for t in allt) else 'NOT reached (no rain step named)'}",
    "no lines -> AKN build not deployed; zero steps everywhere -> AKVR export akvr_shader_hash missing or the rain shaders are not dispatched in this scene")

# D3 rain steps per eye (rainonce 0, before the panel tests)
d3 = tele(between("D0", "D1"))
if d3:
    t = d3[-1]; sl, sr, so, pl, pr, po = map(int, t[:6])
    res("D3 rain steps per eye", True, f"per second: simulation left {sl} right {sr} outside eye renders {so}; splashes left {pl} right {pr} outside {po}",
        "", "steps in BOTH eyes -> the rain is stepped twice per frame from two cameras (= per-eye rain): D4 decides the fix" if sl and sr else
        "steps outside the eye renders only -> rain stepped once; the per-eye difference comes from the draw itself (camera-relative placement)")
else: res("D3 rain steps per eye", False, "no telemetry between D0 and D1", "see D0")

# D1 panel by the test switch: AKVR state says shown + drew into the headset layer, and the headset got an extra quad
d1 = tele(between("D1", "D2"))
s_on, s_off = side("xr_panel"), side("xr_pclosed")
q_on = len([L for L in (s_on or {}).get("layers", []) if L.get("type") == "quad"]) if s_on else -1
q_off = len([L for L in (s_off or {}).get("layers", []) if L.get("type") == "quad"]) if s_off else -1
shown = any(t[9] == "1" for t in d1); drew = any(t[11] == "1" for t in d1); tohs = any(t[12] == "1" for t in d1)
st = d1[-1] if d1 else None
res("D1 VR panel opens (switch)", shown and drew and tohs and q_on > q_off >= 0,
    f"AKVR: shown {int(shown)} drew {int(drew)} into the headset layer {int(tohs)} (ImGui {st[10] if st else '?'}, pad {st[13] if st else '?'}); headset quads open {q_on} / closed {q_off}",
    "shown 0 -> the chord/toggle never reaches the panel flag; drew 0 -> ImGui not ready in engine mode; headset layer 0 -> overlay swapchain not ready; quads equal -> the panel layer is dropped from the engine-mode layer list")
# D2 panel by F8 (keyboard path of the same toggle)
d2 = tele(between("D2", "D2 end")); s_k = side("xr_pkey")
q_k = len([L for L in (s_k or {}).get("layers", []) if L.get("type") == "quad"]) if s_k else -1
res("D2 VR panel opens (F8)", any(t[9] == "1" for t in d2) and q_k > q_off, f"shown {int(any(t[9] == '1' for t in d2))}, headset quads {q_k} (closed {q_off})",
    "F8 not seen -> the key did not reach the game window (focus) - D1 decides")
# chord: the real pad as AKVR reads it
pads = set(t[13] for t in allt); fires = max((int(t[16]) for t in allt), default=0)
out.append(f"D6 controller as AKVR reads it (info): pad connected {sorted(pads)}, chord fires this launch {fires}")

# D4 rain, same camera + shared frame number in both eyes: difference L-R with the second eye's rain steps skipped
def lr(name):
    p = load(name)
    if p is None: return None, None
    w = p.shape[1] // 2; d = np.abs(p[:, :w] - p[:, w:]); return float(d.mean()), d
d0, m0 = lr("pair_rs0.bmp"); d1_, m1 = lr("pair_rs1.bmp"); d3_, m3 = lr("pair_rs3.bmp")
for nm, m in (("D4_rs0_diff.png", m0), ("D4_rs3_diff.png", m3)):
    if m is not None: Image.fromarray(np.clip(m.mean(axis=2) * 4, 0, 255).astype(np.uint8)).save(os.path.join(AKN, nm))
best = min([x for x in (d1_, d3_) if x is not None], default=None)
res("D4 rain the same in both eyes", d0 is not None and best is not None and best < 0.5 * d0 and best < 0.6,
    f"same camera, shared frame: rain stepped in both eyes {d0 if d0 is None else round(d0, 3)}, simulation once {d1_ if d1_ is None else round(d1_, 3)}, simulation + splashes once {d3_ if d3_ is None else round(d3_, 3)} (maps D4_rs0_diff / D4_rs3_diff)",
    "no drop -> the per-eye rain is not the second step: the draw places rain around each eye's camera itself (next: per-eye draw constants of the rain draw)",
    "skipping the second eye's rain step makes the rain identical -> ship rainonce on by default, JJ checks rain speed and splashes")

# D5 normal stereo: the left eye still has rain with its steps skipped (brightness of thin bright streaks per eye)
def streaky(name):
    p = load(name)
    if p is None: return None
    w = p.shape[1] // 2; out_ = []
    for im in (p[:, :w], p[:, w:]):
        g = im.mean(axis=2); hp = g - (np.roll(g, 2, 1) + np.roll(g, -2, 1)) / 2
        out_.append(float((hp > 12).mean() * 100))
    return out_
n3, n0 = streaky("pair_rn3.bmp"), streaky("pair_rn0.bmp")
res("D5 rain still in both eyes", n3 is not None and n0 is not None and min(n3) > 0.5 * min(n0),
    f"thin bright streak pixels % (left/right): steps skipped {n3}, normal {n0}",
    "left eye lost its rain -> the second eye needs the first eye's buffers kept (draw reads per-view data)")
crashes = [l for l in log if "CRASH RECORDER" in l and "XINPUT9" in l.upper().split("| RSP")[0]]   # ours only (AKVR guards its own probes)
res("D7 no crash", not crashes, f"{len(crashes)} crash records", "read akn_log crash lines")

ok = all(" PASS" in l for l in out if l.startswith("D") and "(info)" not in l and not l.startswith("D3"))
out.append(f"STAGE 5d: {'PASS' if ok else 'FAIL'}")
print("\n".join(out))
open(os.path.join(AKN, "M5d_result.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")
