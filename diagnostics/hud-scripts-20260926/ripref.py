from ak import *
import numpy as np
tx=[s for s in secs if s[0]=='.text'][0]; _,TVA,TVS,TRO,TRS=tx
buf=np.frombuffer(data,dtype=np.uint8,count=TRS,offset=TRO)
b=buf.astype(np.int64)
D=(b[:-3] | (b[1:-2]<<8) | (b[2:-1]<<16) | (b[3:]<<24))
D=np.where(D>=2**31,D-2**32,D)
T=TVA+np.arange(len(D))+4+D
def refs(lo,hi):
    ii=np.nonzero((T>=lo)&(T<hi))[0]
    return [(TVA+i, int(T[i])) for i in ii]
