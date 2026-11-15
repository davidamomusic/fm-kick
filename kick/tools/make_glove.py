#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""HOME's boxing glove for FM-KICK, drawn pixel by pixel (an original): writes kick/glove.h."""
import numpy as np, math
W,H=64,62
T,OUT,DK,MD,BS,LT,HL,LACE=0,1,2,3,4,5,6,7
PAL={OUT:(5,12,44),DK:(16,50,140),MD:(28,86,205),BS:(42,122,248),LT:(104,170,255),HL:(180,218,255),LACE:(6,16,54)}
yy,xx=np.mgrid[0:H,0:W]
def ellm(cx,cy,rx,ry): return ((xx-cx)/rx)**2+((yy-cy)/ry)**2<=1.0
def rellm(cx,cy,rx,ry,deg):
    a=math.radians(deg); x=xx-cx; y=yy-cy
    u=x*math.cos(a)+y*math.sin(a); v=-x*math.sin(a)+y*math.cos(a)
    return (u/rx)**2+(v/ry)**2<=1.0
def rrm(x0,y0,x1,y1,r):
    inside=(xx>=x0)&(xx<=x1)&(yy>=y0)&(yy<=y1)
    cxs=np.clip(xx,x0+r,x1-r); cys=np.clip(yy,y0+r,y1-r)
    return inside & (((xx-cxs)**2+(yy-cys)**2)<=r*r)
g=np.zeros((H,W),int)
# a squatter mitten: wider, shorter
hand = rrm(5,2,58,34,16) | rrm(8,18,55,47,9)
thumb = rellm(16,33,10,12,-16) & (yy>=21)
cuff = rrm(11,48,52,60,3)
g[hand]=BS
sh=(xx-32)/26.0+(yy-25)/24.0*0.5
g[hand&(sh>0.45)]=MD
g[hand&(sh>0.82)]=DK
g[hand&(yy>=43)&(sh>0.2)]=MD
g[hand&ellm(21,10,10,5)]=LT
g[hand&ellm(19,8,4,2)]=HL
g[hand&(yy==4)&(xx>=18)&(xx<=42)]=LT
for x in range(18,52):                       # the fold of the fingers
    y=int(round(21+2.0*math.sin((x-18)/34*math.pi)))
    if hand[y,x]: g[y,x]=OUT
for x in range(36,51):
    y=int(round(21+2.0*math.sin((x-18)/34*math.pi)))-1
    if hand[y,x] and x%3: g[y,x]=DK
g[thumb]=LT                                  # the thumb against the hand
g[thumb&(((xx-16)/10)+((yy-33)/12)*0.7>0.5)]=BS
g[thumb&rellm(13,27,2,3,-16)]=HL
edge=thumb & ~rellm(16,33,9,11,-16)
g[edge & (xx>=15)]=OUT
g[thumb&(yy==21)]=OUT
win=ellm(38,35,4,8)                          # the lacing window
g[win]=LT
g[win & ~ellm(38,35,3,7)]=OUT
for y in range(28,43):
    g[y,38]=LACE
    if y%3==1: g[y,37]=LACE; g[y,39]=LACE
g[cuff]=MD                                   # the cuff
g[cuff&(yy<=50)]=DK
g[cuff&(yy>=53)&(yy<=55)]=BS
g[cuff&(yy>=53)&(yy<=54)&(xx>=13)&(xx<=25)]=LT
for y in range(56,60):
    for x in range(42,51):
        if (x+y)%3==0 and cuff[y,x]: g[y,x]=OUT
g[cuff&(yy>=60)]=DK
m=g!=T
nb=np.zeros_like(m)
for dy,dx in [(-1,0),(1,0),(0,-1),(0,1)]:
    nb|=np.roll(np.roll(m,dy,0),dx,1)
g[(~m)&nb]=OUT
g[(yy==48)&cuff]=OUT

# the punch: strokes fanning out from the knuckles, thick by the glove and thin at the end,
# and two speed lines a side under it; pixels relative to the glove's corner
mask=g!=T
hitpx={}
cx,cy=W/2,22.0
for k,deg in enumerate([-180,-158,-136,-114,-90,-66,-44,-22,0,18,162]):
    a=math.radians(deg); c,s=math.cos(a),math.sin(a); t=0
    while True:
        x=int(round(cx+t*c)); y=int(round(cy+t*s))
        if x<0 or y<0 or x>=W or y>=H or not mask[y,x]: break
        t+=0.5
    r0=t+3; ln=(15 if k%2==0 else 10)
    if deg in (-114,-90,-66): ln=min(ln,11)
    for i in range(int(ln*2)):
        tt=r0+i*0.5; f=i/(ln*2)
        x=int(round(cx+tt*c)); y=int(round(cy+tt*s))
        if y<-21: break
        w=1 if f<0.7 else 0
        col=HL if f<0.35 else LT
        for yy2 in range(y-w,y+w+1):
            for xx2 in range(x-w,x+w+1):
                if (xx2,yy2) not in hitpx or hitpx[(xx2,yy2)]==LT: hitpx[(xx2,yy2)]=col
for x0,ln in [(-6,12),(-11,7),(W+5,12),(W+10,7)]:
    y0=H-4
    for y in range(y0-ln,y0+1):
        for xx2 in (x0,x0+1): hitpx[(xx2,y)]=MD
hits=[]
for y in sorted(set(p[1] for p in hitpx)):
    xs=sorted(p[0] for p in hitpx if p[1]==y); i=0
    while i<len(xs):
        x0=xs[i]; c=hitpx[(x0,y)]; j=i
        while j+1<len(xs) and xs[j+1]==xs[j]+1 and hitpx[(xs[j+1],y)]==c: j+=1
        hits.append((y,x0,xs[j]-x0+1,c)); i=j+1

# out: kick/glove.h, the glove as runs per row (x, length, colour index), the palette as RGB565
import os
rows=[]
for y in range(H):
    x=0
    while x<W:
        c=g[y,x]
        if c==T:
            x+=1; continue
        x0=x
        while x<W and g[y,x]==c: x+=1
        rows.append((y,x0,x-x0,c))
def rgb565(c): r,gg,b=c; return ((r>>3)<<11)|((gg>>2)<<5)|(b>>3)
out=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','glove.h')
with open(out,'w') as f:
    f.write('/* SPDX-License-Identifier: MPL-2.0 */\n/* HOME\'s boxing glove, pixel art, %dx%d: made by kick/tools/make_glove.py (do not edit) */\n' % (W,H))
    f.write('#define GLOVE_W %d\n#define GLOVE_H %d\n#define GLOVE_NRUN %d\n' % (W,H,len(rows)))
    f.write('static const uint16_t GLOVE_PAL[8] = {0x0000u, %s};\n' % ', '.join('0x%04Xu'%rgb565(PAL[i]) for i in range(1,8)))
    f.write('/* y, x, length, colour */\nstatic const uint8_t GLOVE_RUN[GLOVE_NRUN][4] = {\n')
    for i in range(0,len(rows),6):
        f.write('    '+' '.join('{%d, %d, %d, %d},'%r for r in rows[i:i+6])+'\n')
    f.write('};\n')
    f.write('/* the punch: y, x (from the glove corner), length, colour */\n#define GLOVE_NHIT %d\nstatic const int8_t GLOVE_HIT[GLOVE_NHIT][4] = {\n' % len(hits))
    for i in range(0,len(hits),6):
        f.write('    '+' '.join('{%d, %d, %d, %d},'%r for r in hits[i:i+6])+'\n')
    f.write('};\n')
print(len(rows),'runs',len(hits),'punch runs')
