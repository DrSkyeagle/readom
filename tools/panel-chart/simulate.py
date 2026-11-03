#!/usr/bin/env python3
"""Panel view of a framebuffer dump, before and after the tone table.

  simulate.py CONTRAST OUT.png NAME.pgm [NAME.pgm ...]

The dumps hold the levels the even spread produced (level*17, i.e. luma to
within +-8). Left: those levels as the panel shows them, relative to its white.
Right: the levels the tone table gives for the same luma.
"""
import sys, math
from PIL import Image
def lum(v):
    v/=255.0
    return v/12.92 if v<=0.04045 else ((v+0.055)/1.055)**2.4
def light(y): return 116*y**(1/3)-16 if y>216/24389 else y*24389/27
def unlight(l): return ((l+16)/116)**3 if l>8 else l*27/24389
def srgb(y): return round(255*(12.92*y if y<=0.0031308 else 1.055*y**(1/2.4)-0.055))
def table(contrast):
    b=light(1.0/contrast)
    return [round(15*min(1,max(0,(light(lum(v))-b)/(100-b)))) for v in range(256)]
contrast=float(sys.argv[1]); out=sys.argv[2]
b=light(1.0/contrast)
shown=[srgb(unlight(b+k/15*(100-b))) for k in range(16)]   # level -> grey on a monitor whose white is the panel's white
even=[shown[(v+8)//17] for v in range(256)]
tone=[shown[k] for k in table(contrast)]
tiles=[]
for name in sys.argv[3:]:
    im=Image.open(name).convert('L')
    tiles.append((im.point(even), im.point(tone)))
w,h=tiles[0][0].size; gap=24
sheet=Image.new('L',(2*w+gap, len(tiles)*(h+gap)-gap), 128)
for i,(a,c) in enumerate(tiles):
    sheet.paste(a,(0,i*(h+gap))); sheet.paste(c,(w+gap,i*(h+gap)))
sheet.save(out)
print(out, sheet.size, 'levels of #212121 #424242 #616161 #757575 #9E9E9E #BDBDBD #E0E0E0 #EEEEEE #F5F5F5 #FAFAFA:',
      [table(contrast)[v] for v in (33,66,97,117,158,189,224,238,245,250)], 'even:', [(v+8)//17 for v in (33,66,97,117,158,189,224,238,245,250)])
