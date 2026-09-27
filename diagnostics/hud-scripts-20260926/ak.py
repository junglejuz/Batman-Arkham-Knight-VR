import struct, capstone, sys
EXE=r"E:/Games/Steam/steamapps/common/Batman Arkham Knight/Binaries/Win64/BatmanAK.exe"
data=open(EXE,'rb').read()
pe=struct.unpack_from('<I',data,0x3c)[0]
nsec=struct.unpack_from('<H',data,pe+6)[0]; opt=struct.unpack_from('<H',data,pe+20)[0]
BASE=struct.unpack_from('<Q',data,pe+24+24)[0]
secs=[]
for i in range(nsec):
    o=pe+24+opt+i*40
    name=data[o:o+8].rstrip(b'\0').decode(); vs,va,rs,ro=struct.unpack_from('<IIII',data,o+8)
    secs.append((name,BASE+va,vs,ro,rs))
def off(v):
    for n,va,vs,ro,rs in secs:
        if va<=v<va+max(vs,rs): return ro+(v-va)
    return None
def rd(v,n): o=off(v); return data[o:o+n]
def q(v): return struct.unpack('<Q',rd(v,8))[0]
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
def dis(v,n=40,stop=True):
    out=[]
    for ins in md.disasm(rd(v,n*8),v):
        out.append(f"{ins.address:x}: {ins.mnemonic} {ins.op_str}")
        n-=1
        if n<=0 or (stop and ins.mnemonic in('ret','int3')): break
    return "\n".join(out)
