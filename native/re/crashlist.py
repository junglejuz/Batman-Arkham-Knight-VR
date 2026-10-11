"""List a UE3AutoReportDump's stack addresses with their module; for XINPUT9_1_0.dll (AKN) map the low 16 bits onto
the built DLL's functions (module bases are 64 KB aligned, so the offset is low16 + n*0x10000).
Usage: python crashlist.py <UE3AutoReportDump.txt> [exe base hex from akn_log.txt]"""
import re, sys, struct, bisect
import pefile, capstone
t = open(sys.argv[1], 'rb').read().decode('utf-16le', 'replace')
i = t.find('Game')
print(t[max(0, i - 60):i + 120].replace('\r', ''))
dll = r'D:\Documents\Antigravity\VR_Modding\akvr_native\build\Release\XINPUT9_1_0.dll'
pe = pefile.PE(dll); img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
pd = [s for s in pe.sections if s.Name.startswith(b'.pdata')][0]
raw = img[pd.VirtualAddress:pd.VirtualAddress + pd.Misc_VirtualSize]
F = sorted(struct.unpack_from('<II', raw, k) for k in range(0, len(raw) - 11, 12))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
exe_base = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
for m in re.finditer(r"Address = 0x([0-9a-f]+) .*?\[in ([^\]]+)\]", t):
    a = int(m.group(1), 16); mod = m.group(2).split('\\')[-1]
    line = f"{a:08x} {mod}"
    if 'XINPUT' in mod.upper():
        for n in range(0, pe.OPTIONAL_HEADER.SizeOfImage >> 16):
            rva = (a & 0xFFFF) + (n << 16)
            k = bisect.bisect_right([b for b, _ in F], rva) - 1
            if k >= 0 and F[k][0] <= rva < F[k][1]:
                ins = list(md.disasm(bytes(img[rva:rva + 16]), base + rva))
                line += f"  -> AKN rva {rva:X} in func {F[k][0]:X}: {ins[0].mnemonic} {ins[0].op_str}" if ins else ''
    elif 'BatmanAK' in mod and exe_base:
        full = (exe_base & ~0xffffffff) | a
        if full < exe_base: full += 1 << 32
        line += f"  -> exe rva {full - exe_base:X}"
    print(line)
