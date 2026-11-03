# Generates the 3840x2160 home-screen background (pic.png) from the Retro Rewind ES-DE artwork. usage: mkbg.py ARTWORK_DIR OUT.png (ARTWORK_DIR holds wii/marquees and wii/physicalmedia). SPDX-License-Identifier: GPL-3.0-or-later
import sys, colorsys
import numpy as np
from PIL import Image, ImageFilter, ImageDraw, ImageFont
S=sys.argv[1]; out=sys.argv[2]
W,H=3840,2160
yy=np.linspace(0,1,H)[:,None]; xx=np.linspace(0,1,W)[None,:]
# dark purple vertical gradient
top=np.array([30,24,66]); bot=np.array([10,8,26])
img=(top[None,None,:]*(1-yy[...,None])+bot[None,None,:]*yy[...,None])*np.ones((H,W,1))
# rainbow checker, hue by y (as in the Retro Rewind artwork)
sq=180
cy=(np.arange(H)//sq)[:,None]; cx=(np.arange(W)//sq)[None,:]
mask=((cy+cx)&1)==1
hue=np.clip(np.arange(H)/H*0.78,0,1)  # red -> purple
rgb=np.array([colorsys.hsv_to_rgb(h,0.85,1.0) for h in hue])  # H x 3
col=rgb[:,None,:]*255
alpha=0.42*mask[...,None]
img=img*(1-alpha)+col*alpha
# vignette
r=np.sqrt(((xx-0.62)*1.0)**2+((yy-0.5)*1.4)**2)
v=np.clip(1.2-r*0.9,0.55,1.0)[...,None]
img=img*v
base=Image.fromarray(np.clip(img,0,255).astype(np.uint8)).convert("RGBA")
# logo
lg=Image.open(S+"/esde/wii/marquees/Retro Rewind.png").convert("RGBA")
a=lg.split()[3].point(lambda v:255 if v>40 else 0); lg=lg.crop(a.getbbox())
lw=1700; lh=round(lg.height*lw/lg.width); lg=lg.resize((lw,lh),Image.LANCZOS)
lx,ly=2300-lw//2,700
glow=Image.new("RGBA",base.size,(0,0,0,0))
sh=Image.new("RGBA",(lw,lh),(255,255,255,0)); sh.putalpha(lg.split()[3].point(lambda v:int(v*0.35)))
glow.paste(sh,(lx,ly),sh); glow=glow.filter(ImageFilter.GaussianBlur(60))
base=Image.alpha_composite(base,glow)
base.alpha_composite(lg,(lx,ly))
# disc
d=Image.open(S+"/esde/wii/physicalmedia/Retro Rewind.png").convert("RGBA")
a=d.split()[3].point(lambda v:255 if v>40 else 0); d=d.crop(a.getbbox()).resize((560,560),Image.LANCZOS)
base.alpha_composite(d,(2300-280,ly+lh+90))
# subtitle
f=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",84)
dr=ImageDraw.Draw(base)
t="PS5 PATCHER"
tw=dr.textlength(t,font=f)
dr.text((2300-tw/2+4,ly-150+4),t,font=f,fill=(0,0,0,200)); dr.text((2300-tw/2,ly-150),t,font=f,fill=(240,240,255,255))
base.convert("RGB").save(out)
