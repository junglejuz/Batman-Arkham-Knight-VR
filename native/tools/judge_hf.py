"""HF judge (2026-10-09): per setup, how stable the HUD sheet is frame to frame (HUDSTAT lines after each "MARK HF <tag>"),
the pause pose stamp (status lines), and the VIEWSCAN lines.  Usage: python judge_hf.py [akn folder]"""
import re, sys, os
from collections import Counter

AKN = sys.argv[1] if len(sys.argv) > 1 else r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\akn"
log = open(os.path.join(AKN, "akn_log.txt"), encoding="utf-8", errors="replace").read().splitlines()
start = max(i for i, l in enumerate(log) if "MARK HF3 start" in l)
tag, rows = None, {}
for l in log[start:]:
    m = re.search(r"MARK HF[23]? (\S+)", l)
    if m: tag = m.group(1); continue
    m = re.search(r"HUDSTAT seq (\d+) draws (\d+) redirects (\d+) \| missed: immediate (\d+), recorded (\d+) \(redirected (\d+)\) \| sheet cells (\d+) hash (\w+) alpha (\d+)", l)
    if m and tag: rows.setdefault(tag, []).append([int(m.group(i)) for i in (1, 2, 3, 4, 5, 6, 7)] + [m.group(8), int(m.group(9))])
print("== HUD sheet per frame (stable HUD = one hash, steady cells/draws)")
for t, r in rows.items():
    seqs = [x[0] for x in r]; dr = [x[1] for x in r]; cells = [x[6] for x in r]; hs = Counter(x[7] for x in r)
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != a + 1)
    print(f"  {t:14s} frames {len(r):3d} | draws {min(dr)}-{max(dr)} | cells {min(cells)}-{max(cells)} | hashes {len(hs)} "
          f"(top {hs.most_common(1)[0][1] if hs else 0}) | missed immediate {sum(x[3] for x in r)}, recorded {sum(x[4] for x in r)} "
          f"(redirected {sum(x[5] for x in r)}) | seq gaps {gaps}")
    if len(hs) > 1:
        seq_cells = " ".join(str(x[6]) for x in r[:40]); print(f"      cells per frame: {seq_cells}")
print("== pose stamp (status lines during the pause)")
on = False
for l in log[start:]:
    if "MARK HF2 pause" in l: on = True
    if "MARK HF2 end" in l: on = False
    if on:
        m = re.search(r"pose: stamp-matched (\d+) of (\d+), delay (\d+)", l)
        if m: print("  ", l[:12], m.group(0))
print("== VIEWSCAN")
for l in log[start:]:
    if "VIEWSCAN eye" in l: print("  ", l[13:400])
