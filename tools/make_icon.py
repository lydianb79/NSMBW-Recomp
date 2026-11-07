import math
from PIL import Image, ImageDraw, ImageFilter, ImageChops

S = 2048                      # supersampled canvas (final 1024)
def sc(v): return int(v * S / 1024)

def rounded_mask(size, box, r):
    m = Image.new('L', (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=r, fill=255)
    return m

def vgrad(w, h, stops):
    img = Image.new('RGB', (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        for i in range(len(stops) - 1):
            t0, c0 = stops[i]; t1, c1 = stops[i + 1]
            if t0 <= t <= t1:
                u = (t - t0) / (t1 - t0)
                c = tuple(int(c0[k] + (c1[k] - c0[k]) * u) for k in range(3))
                break
        for x in range(w): px[x, y] = c
    return img

# --- tile geometry (Apple icon grid: 824px tile inside 1024 canvas) ---
x0, y0, x1, y1 = sc(100), sc(100), sc(924), sc(924)
R = sc(185)
tile = vgrad(S, S, [(0.0, (34, 58, 140)), (0.38, (78, 110, 210)), (0.62, (236, 140, 120)), (0.78, (255, 196, 120)), (1.0, (255, 214, 140))])
art = tile.convert('RGBA')
d = ImageDraw.Draw(art)

# sun glow on the horizon
glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
gd = ImageDraw.Draw(glow)
cx, cy = sc(640), sc(640)
for r, a in ((sc(300), 40), (sc(210), 70), (sc(140), 120), (sc(95), 255)):
    gd.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 236, 170, a))
glow = glow.filter(ImageFilter.GaussianBlur(sc(18)))
art = Image.alpha_composite(art, glow)

# a few soft clouds
cl = Image.new('RGBA', (S, S), (0, 0, 0, 0))
cd = ImageDraw.Draw(cl)
for (x, y, w) in ((250, 300, 150), (330, 285, 120), (700, 250, 110), (760, 262, 90)):
    cd.ellipse((sc(x - w / 2), sc(y - w / 4), sc(x + w / 2), sc(y + w / 4)), fill=(255, 255, 255, 150))
cl = cl.filter(ImageFilter.GaussianBlur(sc(6)))
art = Image.alpha_composite(art, cl)

def hill_layer(base_y, amp, freq, phase, color, highlight):
    lay = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ld = ImageDraw.Draw(lay)
    pts = []
    for x in range(0, S + 8, 8):
        u = x / S
        y = base_y - amp * (0.6 * math.sin(2 * math.pi * (freq * u + phase)) + 0.4 * math.sin(2 * math.pi * (2.3 * freq * u + phase * 1.7)))
        pts.append((x, y))
    ld.polygon(pts + [(S, S), (0, S)], fill=color)
    # rim light along the crest
    ld.line(pts, fill=highlight, width=sc(7))
    return lay

art = Image.alpha_composite(art, hill_layer(sc(690), sc(40), 1.1, 0.15, (46, 120, 120, 255), (120, 200, 170, 255)))
art = Image.alpha_composite(art, hill_layer(sc(760), sc(46), 0.9, 0.55, (52, 160, 86, 255), (150, 225, 120, 255)))
art = Image.alpha_composite(art, hill_layer(sc(840), sc(34), 0.7, 0.05, (88, 196, 72, 255), (196, 245, 140, 255)))

# --- leaping star with a sparkling arc trail ---
def star_poly(cx, cy, r_out, r_in, rot=-90):
    pts = []
    for i in range(10):
        r = r_out if i % 2 == 0 else r_in
        a = math.radians(rot + i * 36)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts

trail = Image.new('RGBA', (S, S), (0, 0, 0, 0))
td = ImageDraw.Draw(trail)
# arc from lower-left hill up to the star
ax, ay, arad = sc(560), sc(760), sc(330)
for i in range(14):
    t = i / 13
    ang = math.radians(200 + 70 * t)
    px_, py_ = ax + arad * math.cos(ang), ay + arad * math.sin(ang)
    rr = sc(5 + 12 * t)
    td.ellipse((px_ - rr, py_ - rr, px_ + rr, py_ + rr), fill=(255, 245, 200, int(60 + 150 * t)))
trail = trail.filter(ImageFilter.GaussianBlur(sc(3)))
art = Image.alpha_composite(art, trail)

scx, scy = sc(560), sc(420)
# a glowing comet orb at the top of its leap (original motif, no character)
og = Image.new('RGBA', (S, S), (0, 0, 0, 0))
ogd = ImageDraw.Draw(og)
for r, a in ((sc(230), 70), (sc(170), 120)):
    ogd.ellipse((scx - r, scy - r, scx + r, scy + r), fill=(255, 230, 150, a))
og = og.filter(ImageFilter.GaussianBlur(sc(36)))
art = Image.alpha_composite(art, og)
d = ImageDraw.Draw(art)
R0 = sc(128)
d.ellipse((scx - R0, scy - R0 + sc(10), scx + R0, scy + R0 + sc(10)), fill=(190, 96, 40, 255))      # shade
d.ellipse((scx - R0, scy - R0, scx + R0, scy + R0), fill=(255, 168, 64, 255))                      # body
d.ellipse((scx - sc(104), scy - sc(108), scx + sc(96), scy + sc(92)), fill=(255, 208, 96, 255))    # inner
d.ellipse((scx - sc(70), scy - sc(84), scx + sc(20), scy - sc(10)), fill=(255, 246, 200, 255))      # specular
# orbit ring around the orb
ring = Image.new('RGBA', (S, S), (0, 0, 0, 0))
rd = ImageDraw.Draw(ring)
rd.ellipse((scx - sc(210), scy - sc(62), scx + sc(210), scy + sc(62)), outline=(255, 255, 255, 220), width=sc(14))
ring = ring.rotate(-18, center=(scx, scy), resample=Image.BICUBIC)
# hide the ring segment behind the orb (upper half of the ellipse)
cut = Image.new('L', (S, S), 255)
ImageDraw.Draw(cut).pieslice((scx - R0, scy - R0, scx + R0, scy + R0), 180, 360, fill=0)
ring.putalpha(ImageChops.multiply(ring.getchannel('A'), cut))
art = Image.alpha_composite(art, ring)
d = ImageDraw.Draw(art)

# sparkles
for (x, y, r) in ((780, 330, 26), (300, 470, 18), (820, 520, 14), (420, 230, 12)):
    x, y, r = sc(x), sc(y), sc(r)
    d.polygon([(x, y - r * 2), (x + r * 0.45, y - r * 0.45), (x + r * 2, y), (x + r * 0.45, y + r * 0.45),
               (x, y + r * 2), (x - r * 0.45, y + r * 0.45), (x - r * 2, y), (x - r * 0.45, y - r * 0.45)], fill=(255, 255, 240, 235))

# --- "recompiled" badge: a small chip in the lower right ---
bx, by, bw = sc(700), sc(700), sc(170)
badge = Image.new('RGBA', (S, S), (0, 0, 0, 0))
bd = ImageDraw.Draw(badge)
bd.rounded_rectangle((bx + sc(6), by + sc(10), bx + bw + sc(6), by + bw + sc(10)), radius=sc(30), fill=(0, 0, 0, 90))
badge = badge.filter(ImageFilter.GaussianBlur(sc(8)))
art = Image.alpha_composite(art, badge)
d = ImageDraw.Draw(art)
pin = sc(14)
for i in range(4):                     # pins on all four sides
    o = bx + sc(32) + i * sc(36)
    d.rounded_rectangle((o, by - pin, o + sc(16), by + sc(4)), radius=sc(4), fill=(200, 205, 215, 255))
    d.rounded_rectangle((o, by + bw - sc(4), o + sc(16), by + bw + pin), radius=sc(4), fill=(200, 205, 215, 255))
    o = by + sc(32) + i * sc(36)
    d.rounded_rectangle((bx - pin, o, bx + sc(4), o + sc(16)), radius=sc(4), fill=(200, 205, 215, 255))
    d.rounded_rectangle((bx + bw - sc(4), o, bx + bw + pin, o + sc(16)), radius=sc(4), fill=(200, 205, 215, 255))
d.rounded_rectangle((bx, by, bx + bw, by + bw), radius=sc(28), fill=(38, 44, 60, 255))
d.rounded_rectangle((bx + sc(10), by + sc(10), bx + bw - sc(10), by + bw - sc(10)), radius=sc(20), fill=(58, 66, 90, 255))
# chevrons ">>" on the chip (forward / native)
cxp, cyp = bx + bw // 2, by + bw // 2
for off in (-sc(26), sc(18)):
    d.line([(cxp + off - sc(18), cyp - sc(34)), (cxp + off + sc(16), cyp), (cxp + off - sc(18), cyp + sc(34))],
           fill=(120, 230, 255, 255), width=sc(16), joint='curve')

# --- clip to the tile, add a subtle inner border and the drop shadow ---
mask = rounded_mask(S, (x0, y0, x1, y1), R)
tile_img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
tile_img.paste(art, (0, 0), mask)
edge = Image.new('RGBA', (S, S), (0, 0, 0, 0))
ImageDraw.Draw(edge).rounded_rectangle((x0, y0, x1, y1), radius=R, outline=(255, 255, 255, 70), width=sc(4))
tile_img = Image.alpha_composite(tile_img, edge)

shadow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
ImageDraw.Draw(shadow).rounded_rectangle((x0, y0 + sc(12), x1, y1 + sc(12)), radius=R, fill=(0, 0, 0, 110))
shadow = shadow.filter(ImageFilter.GaussianBlur(sc(20)))
final = Image.alpha_composite(shadow, tile_img)
final = final.resize((1024, 1024), Image.LANCZOS)
final.save('icon_1024.png')
print('ok')
