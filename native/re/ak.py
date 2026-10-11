"""AKN static RE helpers for BatmanAK.exe (Steam, 54,632,960 bytes, image base 0x140000000).
Adapted from skvr/re/sk.py (pdata bounds) and batman_city/re/ac.py (call census).
Usage: import ak; ak.dis(0x140aed9d9)"""
import re, struct, bisect, pefile, capstone
EXE = r"E:\Games\Steam\steamapps\common\Batman Arkham Knight\Binaries\Win64\BatmanAK.exe"
pe = pefile.PE(EXE, fast_load=True)
img = pe.get_memory_mapped_image()
BASE = pe.OPTIONAL_HEADER.ImageBase
SECS = {s.Name.rstrip(b'\0').decode(): (s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections}
T0, T1 = SECS['.text']
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = False
def rd(va, n): return bytes(img[va-BASE:va-BASE+n])
def u32(va): return struct.unpack('<I', rd(va,4))[0]
def u64(va): return struct.unpack('<Q', rd(va,8))[0]
def i32(va): return struct.unpack('<i', rd(va,4))[0]
def f32(va): return struct.unpack('<f', rd(va,4))[0]
def in_text(va): return T0 <= va-BASE < T1
def find_bytes(pat, lo=0, hi=None):
    hi = len(img) if hi is None else hi
    out=[]; i=img.find(pat, lo, hi)
    while i>=0: out.append(BASE+i); i=img.find(pat,i+1,hi)
    return out
def find_str(s, wide=False):
    enc = s.encode('utf-16le') if wide else s.encode('latin1')
    return find_bytes(enc)
def wstr(va, n=400):
    b=rd(va,n); i=0
    while i+1<len(b) and b[i:i+2]!=b'\0\0': i+=2
    return b[:i].decode('utf-16le','replace')
def cstr(va,n=200): return rd(va,n).split(b'\0')[0].decode('latin1')
_pd = SECS['.pdata']; _raw = img[_pd[0]:_pd[1]]
FUNCS = sorted(struct.unpack_from('<II', _raw, i) for i in range(0, len(_raw)-11, 12) if struct.unpack_from('<I', _raw, i)[0])
_begins = [b for b,_ in FUNCS]
def func_of(va):
    r = va-BASE; i = bisect.bisect_right(_begins, r)-1
    if i<0: return None
    b,e = FUNCS[i]
    return (BASE+b, BASE+e) if b<=r<e else None
def dis(va, n=40, show=True):
    out=[]
    for x in md.disasm(rd(va, n*15), va):
        out.append(f"{x.address:X}  {x.mnemonic} {x.op_str}")
        if len(out)>=n: break
    s="\n".join(out)
    if show: print(s)
    return s
def disrange(a,b): return list(md.disasm(rd(a,b-a), a))
_rip = None
def rip_refs(target):
    """Instruction sites whose RIP-relative disp32 points at target (lea/mov/call/jmp [rip+x])."""
    tlo, thi = T0, T1
    out=[]
    t = target-BASE
    # disp32 at offset o, instruction end = o+4+k (k=0..1 imm bytes); search all disp values
    data = img
    for k in (0,1,4):
        # end = o + 4 + k; target = end + disp -> disp = t - (o+4+k)
        pass
    # brute force: scan text 4-byte windows
    import numpy as np
    arr = np.frombuffer(bytes(data[tlo:thi]), dtype=np.uint8)
    n = len(arr)-4
    d = (arr[:n].astype(np.int64) | (arr[1:n+1].astype(np.int64)<<8) | (arr[2:n+2].astype(np.int64)<<16) | (arr[3:n+3].astype(np.int64)<<24))
    d = np.where(d>=2**31, d-2**32, d)
    pos = np.arange(n, dtype=np.int64) + tlo
    for k in (0,1,4):
        hits = np.nonzero(pos + 4 + k + d == t)[0]
        for h in hits: out.append((BASE+tlo+int(h), k))
    return out
_callers=None
def call_census():
    global _callers
    if _callers is not None: return
    import numpy as np
    arr = np.frombuffer(bytes(img[T0:T1]), dtype=np.uint8)
    idx = np.nonzero(arr[:-5]==0xE8)[0]
    d = arr[idx+1].astype(np.int64)|(arr[idx+2].astype(np.int64)<<8)|(arr[idx+3].astype(np.int64)<<16)|(arr[idx+4].astype(np.int64)<<24)
    d = np.where(d>=2**31, d-2**32, d)
    tgt = idx + T0 + 5 + d
    ok = (tgt>=T0)&(tgt<T1)
    _callers={}
    for s,t in zip(idx[ok], tgt[ok]):
        _callers.setdefault(BASE+int(t), []).append(BASE+T0+int(s))
def callers(va):
    call_census(); return _callers.get(va, [])
def calls_in(va):
    f = func_of(va); 
    if not f: return []
    return [(x.address, x.op_str) for x in disrange(*f) if x.mnemonic=='call']
