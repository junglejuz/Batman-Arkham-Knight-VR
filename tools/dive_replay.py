import csv,math,sys
exec(open('divesim.py').read().split('def tilt_smooth')[0])
rows=list(csv.DictReader(open(sys.argv[1])))
def nearest(y,p,r,by,bp,br):
    ay,ap,ar=y+math.pi,(-math.pi if p<0 else math.pi)-p,r+math.pi
    c=lambda a,b,cc: abs(wrap(a-by))+abs(wrap(b-bp))+abs(wrap(cc-br))
    return (ay,ap,ar) if c(ay,ap,ar)<c(y,p,r) else (y,p,r)
def shown(r,new):
    by,bp,br=[float(r[k])*D2R for k in ('base_yaw_deg','base_pitch_deg','base_roll_deg')]
    gy,gp,gr=[float(r[k])*D2R for k in ('head_yaw_deg','head_pitch_deg','head_roll_deg')]
    f,ri,u=build(by,bp,br); b2=decomp(f,ri)
    if new: b2=nearest(*b2,by,bp,br)
    bf,brr,bu=build(b2[0],b2[1],0); hf,hr,hu=build(gy,-gp,-gr)
    tc=lambda v: add(add(sc(bf,v[0]),sc(brr,v[1])),sc(bu,v[2]))
    fw,rr=norm(tc(hf)),norm(tc(hr)); fy,fp,fr=decomp(fw,rr); fr=-fr
    if new: fy,fp,fr=nearest(fy,fp,fr,*b2)
    dy,dp,dr=wrap(fy-b2[0]),wrap(fp-b2[1]),wrap(fr-b2[2])
    return view(by+dy,bp+dp,br+dr)
for new in (False,True):
    prev=None; ev=[]
    for i,r in enumerate(rows):
        v=shown(r,new)
        if prev:
            j=max(ang(v[0],prev[0]),ang(v[1],prev[1]))
            if j>30: ev.append("t=%.2f %.0f"%(float(r['t_sec']),j))
        prev=v
    print("NEW" if new else "OLD", len(ev), ev[:12])
mx=0; n=0; steepmx=0
for r in rows:
    a=shown(r,False); b=shown(r,True); d=max(ang(a[0],b[0]),ang(a[1],b[1]))
    if abs(float(r['base_pitch_deg']))<89: mx=max(mx,d); n+=1
print("frames with game pitch inside +-89:",n,"max difference old vs new %.4f deg"%mx)
