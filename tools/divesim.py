# Replay of AKVR's camera pipeline (camera.cpp, pitchunlink=2 "tipped") on a synthetic dive.
# Current: field = game_n + delta(game_{n-1}) + tiltcorr_n ; delta = Euler(compose(base)) - Euler(base)
# Fixed:   field = Euler_nearest(compose(game_n + tiltcorr_n))
import math, sys
D2R = math.pi / 180; R2D = 180 / math.pi
def norm(v): l = math.sqrt(sum(x*x for x in v)); return [x/l for x in v]
def add(a,b): return [a[i]+b[i] for i in range(3)]
def sc(a,s): return [x*s for x in a]
def dot(a,b): return sum(a[i]*b[i] for i in range(3))
def cross(a,b): return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
def build(y,p,r):
    cy,sy,cp,sp=math.cos(y),math.sin(y),math.cos(p),math.sin(p)
    f=[cy*cp,sy*cp,sp]; r0=[-sy,cy,0]; u0=cross(f,r0)
    cr,sr=math.cos(r),math.sin(r)
    return f, add(sc(r0,cr),sc(u0,sr)), add(sc(u0,cr),sc(r0,-sr))
def decomp(f,r):
    f=norm(f); h=math.hypot(f[0],f[1])
    y=math.atan2(f[1],f[0]) if h>0.02 else math.atan2(-r[0],r[1])
    p=math.asin(max(-1,min(1,f[2])))
    r0=[-math.sin(y),math.cos(y),0]; u0=cross(f,r0)
    return y,p,math.atan2(dot(r,u0),dot(r,r0))
def wrap(a):
    while a>math.pi: a-=2*math.pi
    while a<-math.pi: a+=2*math.pi
    return a
def compose(by,bp,gy,gp,gr):
    bf,br,bu=build(by,bp,0); hf,hr,hu=build(gy,-gp,-gr)
    tc=lambda v: add(add(sc(bf,v[0]),sc(br,v[1])),sc(bu,v[2]))
    f,r,u=norm(tc(hf)),norm(tc(hr)),norm(tc(hu))
    y,p,ro=decomp(f,r); return y,p,-ro, f, u      # UE roll sense
def view(y,p,rue): f,r,u=build(y,p,-rue); return f,u
def ang(a,b): return math.acos(max(-1,min(1,dot(norm(a),norm(b)))))*R2D

def tilt_smooth(st, pitch, stick=False):
    if 'shown' not in st: st.update(shown=pitch, prev=pitch, vel=0.0, last=0.0, act=False); return 0.0
    dp=pitch-st['prev']; st['prev']=pitch; old=st['shown']
    if abs(dp)>25: st.update(shown=pitch,act=False,vel=0.0)
    elif not st['act'] and abs(dp)>1.0 and not stick: st['act']=True; st['vel']=st['last']
    elif not st['act']: st['shown']=pitch
    if st['act'] and not stick:
        d=pitch-st['shown']; vt=max(-1.3,min(1.3,0.25*d)); st['vel']+=max(-0.15,min(0.15,vt-st['vel'])); st['shown']+=st['vel']
        if abs(pitch-st['shown'])<0.05 and abs(dp)<=1.0 and abs(st['vel'])<0.2: st.update(shown=pitch,act=False,vel=0.0)
    st['last']=0.0 if abs(dp)>25 else st['shown']-old
    return st['shown']-pitch

def dive(fps=60):
    # game pitch: level -> dive to the -71.4 limit in 0.35 s, hold 1.2 s, pull out to -5 in 0.45 s; heading drifts
    out=[]; t=0; n=int(2.6*fps)
    for i in range(n):
        t=i/fps
        if t<0.3: p=-8
        elif t<0.65: p=-8+(-71.4+8)*((t-0.3)/0.35)
        elif t<1.85: p=-71.4
        elif t<2.3: p=-71.4+(66.4)*((t-1.85)/0.45)
        else: p=-5
        out.append((40*t, p))   # degrees: slow heading drift 40 deg/s
    return out

def run(head, ease=True, fixed=False):
    gy,gp,gr=[x*D2R for x in head]
    st={}; prevBase=None; delta=(0,0,0); prevF=None; prevU=None; worst=(0,0); worstErr=0
    for i,(yd,pd) in enumerate(dive()):
        corr = tilt_smooth(st,pd) if ease else 0.0
        shownY,shownP=yd*D2R,(pd+corr)*D2R
        want=compose(shownY,shownP,gy,gp,gr)
        if fixed:
            f,u=want[3],want[4]
        else:
            fy,fp,fr=yd*D2R+delta[0], pd*D2R+delta[1]+corr*D2R, 0+delta[2]
            f,u=view(fy,fp,fr)
        err=ang(f,want[3])+0*ang(u,want[4])
        uerr=ang(u,want[4])
        if i>=6: worstErr=max(worstErr,err,uerr)
        if prevF is not None and i>=6:
            j=max(ang(f,prevF),ang(u,prevU))
            if j>worst[0]: worst=(j,i/60)
        prevF,prevU=f,u
        # akvr_head_update after this finalize: deltas from the game's own base (g_b*)
        by,bp=yd*D2R,pd*D2R
        cy_,cp_,cr_,_,_=compose(by,bp,gy,gp,gr)
        b2y,b2p,b2r=decomp(*build(by,bp,0)[:2])
        delta=(wrap(cy_-b2y),wrap(cp_-b2p),wrap(cr_-0))
    return worst, worstErr

for head in [(0,10,0),(0,25,0),(15,25,0),(30,30,0),(15,15,5),(-20,35,0),(0,-10,0),(40,5,0)]:   # pitch + = looking DOWN (g_htPitch)
    for ease in (True,False):
        (j,t),e=run(head,ease,False); (jf,tf),ef=run(head,ease,True)
        print("head yaw/pitch/roll %-14s ease=%d  NOW: worst frame step %6.1f deg @%.2fs, off-target %6.1f | FIXED: step %5.1f, off %4.1f"%(head,ease,j,t,e,jf,ef))
