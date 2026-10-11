"""HF5 driver (2026-10-09): sanity after DETVIG / VSSYNC / BLACKKEY2 / smaller MARKERCUT: HUD steady + frame rate in
gameplay, pause shot, the shared detective vignette forced on (both eyes). Usage: python hf5_driver.py <akn> <tools>"""
import os, sys, re, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hf4_driver as h   # its helpers (cmd, xrshot, pause, log_pos, log_since, say) - its own steps do not run on import? (guarded below)
def stat(n=10):
    p = h.log_pos(); h.cmd(f"hudstat {n}", 0.5 + n * 0.03)
    time.sleep(0.5)
    ls = [l for l in h.log_since(p).splitlines() if "HUDSTAT seq" in l]
    cells = [int(re.search(r"sheet cells (\d+)", l).group(1)) for l in ls]
    return cells
def fps():
    ls = [l for l in h.log_since(max(0, h.log_pos() - 200000)).splitlines() if "frames/s" in l]
    return ls[-1][:160] if ls else "?"
h.cmd("mark HF5 start", 0.5)
h.say(f"gameplay HUD cells: {stat()}"); h.say("rate: " + fps())
h.xrshot("xr_hf5game"); import subprocess; subprocess.run([sys.executable, os.path.join(h.TOOLS, "akmenu.py"), "shot", os.path.join(h.AKN, "hf5_desktop.png")], capture_output=True)
p0 = h.log_pos(); h.cmd("akvrdiag", 1.0); h.say(" | ".join(l[13:200] for l in h.log_since(p0).splitlines() if "AKVR:" in l))
h.cmd("detvigtest 1", 1.5); h.xrshot("xr_hf5detvig"); h.cmd("detvigtest 0", 1.0)
if h.pause(True):
    time.sleep(2); h.xrshot("xr_hf5pause"); h.say(f"pause HUD cells: {stat()}"); h.pause(False)
time.sleep(2); h.say(f"after HUD cells: {stat()}"); h.say("rate: " + fps())
h.cmd("mark HF5 end", 0.5)
open(os.path.join(h.AKN, "hf5_result.txt"), "w").write("\n".join(h.out) + "\n")
