# Original platformer-style app icon: bright sky, grassy ground with a wavy
# soil band, floating coins, and an original mascot ("Pip", a round teal
# jelly hero with a yellow scarf) mid-jump. No third-party characters or logos.
import math
from PIL import Image, ImageDraw, ImageFilter

S = 2048
def sc(v): return int(round(v * S / 1024))

def vgrad(w, h, top, bot):
    img = Image.new('RGBA', (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[k] + (bot[k] - top[k]) * t) for k in range(3)) + (255,))
    return img

x0, y0, x1, y1, R = sc(100), sc(100), sc(924), sc(924), sc(185)
art = vgrad(S, S, (64, 156, 255), (170, 225, 255))
d = ImageDraw.Draw(art)

# clouds: puffy white with a soft blue underside
def cloud(cx, cy, s):
    lay = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lay)
    for dx, dy, r in ((-60, 10, 42), (-20, -18, 55), (30, -8, 48), (66, 14, 36), (0, 18, 50)):
        ld.ellipse((sc(cx + (dx - r) * s), sc(cy + (dy - r) * s), sc(cx + (dx + r) * s), sc(cy + (dy + r) * s)), fill=(214, 236, 255, 255))
    for dx, dy, r in ((-60, 4, 40), (-20, -24, 53), (30, -14, 46), (66, 8, 34), (0, 10, 48)):
        ld.ellipse((sc(cx + (dx - r) * s), sc(cy + (dy - r) * s), sc(cx + (dx + r) * s), sc(cy + (dy + r) * s)), fill=(255, 255, 255, 255))
    return lay
for c in (cloud(300, 260, 1.0), cloud(760, 330, 0.75), cloud(560, 180, 0.55)):
    art = Image.alpha_composite(art, c)
d = ImageDraw.Draw(art)

# ground: wavy soil band with dots, bright grass top with blade bumps
gy = sc(720)
soil = [(x, gy + sc(30) + sc(10) * math.sin(x / S * 2 * math.pi * 6)) for x in range(0, S + 8, 8)]
d.polygon(soil + [(S, S), (0, S)], fill=(200, 130, 70, 255))
for row in range(3):
    yy = gy + sc(110) + row * sc(70)
    band = [(x, yy + sc(12) * math.sin(x / S * 2 * math.pi * 5 + row)) for x in range(0, S + 8, 8)]
    d.line(band, fill=(170, 100, 50, 255), width=sc(14), joint='curve')
for i in range(26):
    xx = (i * 173) % 1024; yy = 790 + (i * 97) % 130
    d.ellipse((sc(xx - 6), sc(yy - 6), sc(xx + 6), sc(yy + 6)), fill=(232, 172, 110, 255))
grass = [(x, gy - sc(10) + sc(14) * abs(math.sin(x / S * math.pi * 22))) for x in range(0, S + 4, 4)]
d.polygon(grass + [(S, gy + sc(40)), (0, gy + sc(40))], fill=(76, 200, 70, 255))
d.line(grass, fill=(150, 240, 110, 255), width=sc(8))
d.rectangle((0, gy + sc(34), S, gy + sc(46)), fill=(40, 150, 50, 255))

# coins (generic gold discs with a slot highlight)
def coin(cx, cy, r, squash=1.0):
    d.ellipse((sc(cx - r * squash), sc(cy - r), sc(cx + r * squash), sc(cy + r)), fill=(214, 140, 20, 255))
    d.ellipse((sc(cx - (r - 7) * squash), sc(cy - r + 7), sc(cx + (r - 7) * squash), sc(cy + r - 7)), fill=(255, 205, 50, 255))
    d.rounded_rectangle((sc(cx - 6 * squash), sc(cy - r * 0.55), sc(cx + 6 * squash), sc(cy + r * 0.55)), radius=sc(5), fill=(255, 240, 160, 255))
coin(230, 470, 44); coin(330, 420, 44, 0.55); coin(420, 395, 44, 0.2)

# jump arc
for i in range(10):
    t = i / 9
    px = 300 + 320 * t; py = 690 - 300 * math.sin(math.pi * 0.5 * t)
    r = 6 + 6 * t
    d.ellipse((sc(px - r), sc(py - r), sc(px + r), sc(py + r)), fill=(255, 255, 255, int(70 + 140 * t)))

# mascot "Pip": shadow, body, belly, eyes, cheeks, smile, scarf, little feet
mx, my = 650, 450
sh = Image.new('RGBA', (S, S), (0, 0, 0, 0))
ImageDraw.Draw(sh).ellipse((sc(mx - 120), sc(705), sc(mx + 120), sc(735)), fill=(0, 60, 20, 110))
art = Image.alpha_composite(art, sh.filter(ImageFilter.GaussianBlur(sc(8))))
d = ImageDraw.Draw(art)
d.ellipse((sc(mx - 95), sc(my + 120), sc(mx - 30), sc(my + 170)), fill=(20, 110, 120, 255))   # feet
d.ellipse((sc(mx + 30), sc(my + 115), sc(mx + 95), sc(my + 165)), fill=(20, 110, 120, 255))
d.ellipse((sc(mx - 150), sc(my - 150), sc(mx + 150), sc(my + 150)), fill=(24, 140, 150, 255))   # body outline
d.ellipse((sc(mx - 140), sc(my - 145), sc(mx + 140), sc(my + 138)), fill=(52, 200, 196, 255))   # body
d.ellipse((sc(mx - 100), sc(my - 120), sc(mx - 20), sc(my - 60)), fill=(170, 245, 235, 255))    # gloss
d.ellipse((sc(mx - 80), sc(my + 10), sc(mx + 80), sc(my + 125)), fill=(150, 236, 220, 255))     # belly
for ex in (mx - 48, mx + 48):                                                                     # eyes
    d.ellipse((sc(ex - 30), sc(my - 70), sc(ex + 30), sc(my + 6)), fill=(255, 255, 255, 255))
    d.ellipse((sc(ex - 14), sc(my - 50), sc(ex + 18), sc(my - 4)), fill=(30, 40, 70, 255))
    d.ellipse((sc(ex - 6), sc(my - 44), sc(ex + 6), sc(my - 30)), fill=(255, 255, 255, 255))
for cx in (mx - 92, mx + 92):                                                                     # cheeks
    d.ellipse((sc(cx - 22), sc(my + 2), sc(cx + 22), sc(my + 26)), fill=(255, 140, 150, 200))
d.arc((sc(mx - 34), sc(my - 6), sc(mx + 34), sc(my + 44)), 20, 160, fill=(30, 40, 70, 255), width=sc(9))  # smile
# scarf flowing back (motion)
d.rounded_rectangle((sc(mx - 120), sc(my + 52), sc(mx + 120), sc(my + 92)), radius=sc(20), fill=(255, 196, 40, 255))
d.polygon([(sc(mx - 110), sc(my + 60)), (sc(mx - 230), sc(my + 30)), (sc(mx - 210), sc(my + 80)), (sc(mx - 250), sc(my + 110)), (sc(mx - 108), sc(my + 90))], fill=(255, 176, 30, 255))
d.line([(sc(mx - 120), sc(my + 72)), (sc(mx + 120), sc(my + 72))], fill=(230, 150, 20, 255), width=sc(5))

# sparkle near the mascot
for (x, y, r) in ((820, 250, 18), (500, 300, 12)):
    x, y, r = sc(x), sc(y), sc(r)
    d.polygon([(x, y - 2 * r), (x + r * 0.45, y - r * 0.45), (x + 2 * r, y), (x + r * 0.45, y + r * 0.45),
               (x, y + 2 * r), (x - r * 0.45, y + r * 0.45), (x - 2 * r, y), (x - r * 0.45, y - r * 0.45)], fill=(255, 255, 255, 240))

# clip to tile, rim, shadow
mask = Image.new('L', (S, S), 0); ImageDraw.Draw(mask).rounded_rectangle((x0, y0, x1, y1), radius=R, fill=255)
tile = Image.new('RGBA', (S, S), (0, 0, 0, 0)); tile.paste(art, (0, 0), mask)
rim = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ImageDraw.Draw(rim).rounded_rectangle((x0, y0, x1, y1), radius=R, outline=(255, 255, 255, 90), width=sc(4))
tile = Image.alpha_composite(tile, rim)
shadow = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ImageDraw.Draw(shadow).rounded_rectangle((x0, y0 + sc(12), x1, y1 + sc(12)), radius=R, fill=(0, 0, 0, 110))
out = Image.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(sc(20))), tile).resize((1024, 1024), Image.LANCZOS)
out.save('icon_1024.png'); out.resize((512, 512), Image.LANCZOS).save('preview.png'); print('ok')
