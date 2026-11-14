#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""HOME's boxing glove for FM-KICK, drawn pixel by pixel (an original): writes kick/glove.h."""
import numpy as np, math
W,H=56,76
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
# the mitten: big rounded top, slightly narrower to the wrist
hand = rrm(7,2,51,40,17) | rrm(9,24,49,60,8)
thumb = rellm(16,41,9,15,-14) & (yy>=26)
cuff = rrm(11,59,47,74,3)
g[hand]=BS
sh=(xx-29)/22.0+(yy-30)/30.0*0.5
g[hand&(sh>0.45)]=MD
g[hand&(sh>0.82)]=DK
g[hand&(yy>=54)&(sh>0.2)]=MD
# shine: a soft blob and a bright spot, top left; a thin rim light on top
g[hand&ellm(21,12,9,6)]=LT
g[hand&ellm(19,10,4,2)]=HL
g[hand&(yy==4)&(xx>=18)&(xx<=36)]=LT
# the fold of the fingers: a line across, over the thumb's tip
for x in range(17,45):
    y=int(round(26+ 2.2*math.sin((x-17)/28*math.pi)))
    if hand[y,x]: g[y,x]=OUT
for x in range(33,45):
    y=int(round(26+2.2*math.sin((x-17)/28*math.pi)))-1
    if hand[y,x] and x%3: g[y,x]=DK
# the thumb lying on the hand, its edge on the inside
g[thumb]=LT
g[thumb&(((xx-16)/9)+((yy-41)/15)*0.7>0.5)]=BS
g[thumb&rellm(13,34,2,4,-14)]=HL
edge=thumb & ~rellm(16,41,8,14,-14)
g[edge & (xx>=15)]=OUT
g[thumb & (yy==26)]=OUT
# the lacing window on the palm
win=ellm(34,42,4,11)
g[win]=LT
g[win & ~ellm(34,42,3,10)]=OUT
for y in range(33,52):
    g[y,34]=LACE
    if y%3==1: g[y,33]=LACE; g[y,35]=LACE
# the cuff: a darker band, a light stripe, a hatched patch
g[cuff]=MD
g[cuff&(yy<=61)]=DK
g[cuff&(yy>=64)&(yy<=66)]=BS
g[cuff&(yy>=64)&(yy<=65)&(xx>=13)&(xx<=24)]=LT
for y in range(67,73):
    for x in range(37,46):
        if (x+y)%3==0 and cuff[y,x]: g[y,x]=OUT
g[cuff&(yy>=73)]=DK
# outline all round, and between hand and cuff
m=g!=T
nb=np.zeros_like(m)
for dy,dx in [(-1,0),(1,0),(0,-1),(0,1)]:
    nb|=np.roll(np.roll(m,dy,0),dx,1)
g[(~m)&nb]=OUT
g[(yy==59)&cuff]=OUT

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
print(len(rows),'runs')
