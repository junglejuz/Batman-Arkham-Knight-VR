"""HF6 driver (2026-10-09): RAINPAIR (rain step once per stereo pair, by pair not by eye label) and VSFRESH (new
right-eye view state, forced by command 3x; detective mode itself is pad-only). Usage: python hf6_driver.py <akn> <tools>"""
import os, sys, re, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hf4_driver as h
def rain(tag):
    time.sleep(3.2)
    ls = [l for l in h.log_since(max(0, h.log_pos() - 60000)).splitlines()]
    r = [l for l in ls if "rain steps/s" in l][-2:]; f = [l for l in ls if "frames/s" in l][-1:]
    for l in r: h.say(f"{tag}: " + re.sub(r" \| VR panel.*", "", l[13:]))
    for l in f: h.say(f"{tag}: " + l[13:60])
h.cmd("mark HF6 start", 0.5)
rain("gameplay")
if h.pause(True):
    time.sleep(2); rain("paused"); h.pause(False)
rain("after pause")
for i in range(3):
    p = h.log_pos(); h.cmd("vsfresh", 1.5)
    h.say(" | ".join(l[13:160] for l in h.log_since(p).splitlines() if "VSFRESH" in l) or "no VSFRESH line")
    h.xrshot(f"xr_hf6fresh{i}")
rain("after resets")
h.tool("mouse", "800", "0"); time.sleep(2); h.xrshot("xr_hf6turn")
rain("end")
h.cmd("mark HF6 end", 0.5)
open(os.path.join(h.AKN, "hf6_result.txt"), "w").write("\n".join(h.out) + "\n")
