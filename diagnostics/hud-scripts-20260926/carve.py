import sys,zlib,os,re,glob
os.makedirs('swf',exist_ok=True)
for f in glob.glob('out_*/**/*.SwfMovie',recursive=True):
    b=open(f,'rb').read()
    for m in re.finditer(rb'(CFX|GFX|CWS|FWS)[\x06-\x10]',b):
        o=m.start(); sig=b[o:o+3]; ver=b[o+3]; ln=int.from_bytes(b[o+4:o+8],'little')
        body=b[o+8:]
        if sig in (b'CFX',b'CWS'):
            try: body=zlib.decompressobj().decompress(body)
            except Exception as e: print('fail',f,e); continue
        data=(b'GFX' if sig[1:]==b'FX' else b'FWS')+bytes([ver])+ln.to_bytes(4,'little')+body[:ln-8]
        name=f.split(os.sep)[-3 if False else -1]
        pkg=f.replace(chr(92),'/').split('/')
        out='swf/%s__%s__%s.gfx'%(pkg[1],pkg[-2],pkg[-1].replace('.SwfMovie',''))
        open(out,'wb').write(data); print(out,len(data),ln); break
