"""HF9 driver (2026-10-09): MARKSHEET - marker pieces routed into their own sheet. Log the HUD draws' placements
(mkpieces), the routed counts, headset shots while turning (markers on screen). Usage: python hf9_driver.py <akn> <tools>"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hf4_driver as h
def last(key):
    ls = [l for l in h.log_since(max(0, h.log_pos() - 60000)).splitlines() if key in l]
    return ls[-1][13:260] if ls else "?"
h.cmd("mark HF9 start", 0.5)
for i in range(6):
    p = h.log_pos(); h.cmd("mkpieces 60", 1.5)
    ls = [l[13:200] for l in h.log_since(p).splitlines() if "MKPIECE" in l]
    routed = [l for l in ls if "MARKER SHEET" in l]
    h.say(f"turn {i}: {len(ls)} pieces logged, {len(routed)} routed | " + last("HUD in the eyes")[:200])
    for l in (routed[:6] or ls[:4]): h.say("   " + l)
    h.xrshot(f"xr_hf9_{i}")
    h.tool("mouse", "700", "0"); time.sleep(1.5)
h.cmd("mark HF9 end", 0.5)
open(os.path.join(h.AKN, "hf9_result.txt"), "w").write("\n".join(h.out) + "\n")
