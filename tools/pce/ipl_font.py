"""Small built-in error font; does not need a successful CD or SCD RAM load."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
font=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf',8)
rows=[]
for char in 'SUPER CD V3 REQUIRED':
    im=Image.new('1',(8,8));ImageDraw.Draw(im).text((0,-2),char,font=font,fill=1)
    rows.extend(sum((1<<(7-x)) for x in range(8) if im.getpixel((x,y))) for y in range(8))
Path(sys.argv[1]).write_text('static const unsigned char error_font[]={' + ','.join(str(x) for x in rows)+'};\n')
