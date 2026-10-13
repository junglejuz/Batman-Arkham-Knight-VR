# For each RVA (a return address), find the enclosing function via .pdata, then list the strings and
# imported functions that function references (RIP-relative operands).
import sys, pefile, capstone
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
imp = {}
for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    for i in d.imports:
        imp[i.address - base] = (d.dll.decode() + '!' + (i.name.decode() if i.name else str(i.ordinal)))
funcs = []
for e in pe.DIRECTORY_ENTRY_EXCEPTION:
    funcs.append((e.struct.BeginAddress, e.struct.EndAddress))
funcs.sort()
def func_of(rva):
    for b, e in funcs:
        if b <= rva < e: return b, e
    return None
def cstr(rva):
    try:
        data = pe.get_data(rva, 120)
    except Exception: return None
    s = data.split(b'\0')[0]
    if len(s) >= 4 and all(32 <= c < 127 for c in s): return s.decode()
    w = data.decode('utf-16le', 'ignore').split('\0')[0]
    if len(w) >= 4 and all(32 <= ord(c) < 127 for c in w): return 'L"' + w + '"'
    return None
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
for a in sys.argv[2:]:
    rva = int(a, 16); fb = func_of(rva)
    print(f"== rva {rva:x}: function {fb[0]:x}-{fb[1]:x}" if fb else f"== rva {rva:x}: no function")
    if not fb: continue
    code = pe.get_data(fb[0], fb[1] - fb[0]); seen = []
    for ins in md.disasm(code, fb[0]):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                t = ins.address + ins.size + op.mem.disp
                s = imp.get(t) or cstr(t)
                if s and s not in seen: seen.append(s)
        if ins.address < rva <= ins.address + 16 and ins.mnemonic == 'call':
            pass
    for s in seen[:40]: print("   ", s)
    # the call right before the return address
    for ins in md.disasm(pe.get_data(rva - 16, 16), rva - 16):
        if ins.address + ins.size == rva: print("   call before ret:", ins.mnemonic, ins.op_str)
