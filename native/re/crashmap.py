"""Map a UE3AutoReportDump's 32-bit-truncated BatmanAK.exe addresses to RVAs / functions.
Usage: python crashmap.py <report.txt> <exe base hex from akn_log.txt>"""
import re, sys, os
import ak
rep = open(sys.argv[1], 'rb').read().decode('utf-16le', 'replace')
base = int(sys.argv[2], 16)
for m in re.finditer(r"Address = 0x([0-9a-f]+) .*?\[in ([^\]]+)\]", rep):
    a = int(m.group(1), 16); mod = os.path.basename(m.group(2))
    if 'BatmanAK' in mod:
        full = (base & ~0xffffffff) | a
        if full < base: full += 1 << 32
        rva = full - base; f = ak.func_of(ak.BASE + rva)
        print(hex(rva), 'in func', f and hex(f[0] - ak.BASE), f and (f[1] - f[0]))
    else:
        print(mod, hex(a))
i = rep.find('Game')
print(rep[i:i + 1500].replace('\r', ''))
