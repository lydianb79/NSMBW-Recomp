# Original app icon: floating grassy islands in a bright sky, a coil
# springboard on the front island launching a fountain of gold coins.
import math, random
from PIL import Image, ImageDraw, ImageFilter

S = 2048
def sc(v): return int(round(v * S / 1024))
random.seed(7)

def vgrad(w, h, stops):
    img = Image.new('RGBA', (w, h)); d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / (h - 1)
        for i in range(len(stops) - 1):
            t0, c0 = stops[i]; t1, c1 = stops[i + 1]
            if t0 <= t <= t1:
                u = (t - t0) / (t1 - t0); c = tuple(int(c0[k] + (c1[k] - c0[k]) * u) for k in range(3)); break
        d.line([(0, y), (w, y)], fill=c + (255,))
    return img

x0, y0, x1, y1, R = sc(100), sc(100), sc(924), sc(924), sc(185)
art = vgrad(S, S, [(0, (40, 132, 245)), (0.6, (120, 200, 255)), (1, (200, 238, 255))])

def cloud(cx, cy, s, alpha=255):
    lay = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ld = ImageDraw.Draw(lay)
    for dx, dy, r, col in ((-60, 12, 42, (205, 230, 255)), (-20, -14, 55, (205, 230, 255)), (32, -4, 48, (205, 230, 255)), (70, 16, 36, (205, 230, 255)),
                           (-60, 4, 40, (255, 255, 255)), (-20, -22, 53, (255, 255, 255)), (32, -12, 46, (255, 255, 255)), (70, 8, 34, (255, 255, 255))):
        ld.ellipse((sc(cx + (dx - r) * s), sc(cy + (dy - r) * s), sc(cx + (dx + r) * s), sc(cy + (dy + r) * s)), fill=col + (alpha,))
    return lay

# distant clouds
for c in (cloud(250, 250, 0.9), cloud(800, 210, 0.7), cloud(820, 560, 0.6, 200)):
    art = Image.alpha_composite(art, c)

def island(cx, top, w, depth, grass=(88, 200, 70), grass_hi=(170, 240, 120), rock=(150, 110, 80), rock_dk=(110, 78, 56)):
    lay = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ld = ImageDraw.Draw(lay)
    # rocky underside: an inverted, jagged cone
    pts = [(cx - w / 2, top + 20)]
    n = 9
    for i in range(1, n):
        t = i / n
        x = cx - w / 2 + w * t
        y = top + 20 + depth * math.sin(math.pi * t) ** 0.8 + random.uniform(-10, 10)
        pts.append((x, y))
    pts.append((cx + w / 2, top + 20))
    ld.polygon([(sc(x), sc(y)) for x, y in pts], fill=rock + (255,))
    # darker strata
    for k in (0.35, 0.65):
        ld.line([(sc(cx - w / 2 * (1 - k * 0.6)), sc(top + 20 + depth * k * 0.9)), (sc(cx + w / 2 * (1 - k * 0.6)), sc(top + 20 + depth * k * 0.9))], fill=rock_dk + (255,), width=sc(8))
    # dangling roots / tiny stones
    for i in range(3):
        rx = cx + random.uniform(-w / 4, w / 4); ry = top + 20 + depth * 0.95
        ld.ellipse((sc(rx - 7), sc(ry - 7), sc(rx + 7), sc(ry + 7)), fill=rock_dk + (255,))
    # grass cap with a scalloped edge
    ld.rounded_rectangle((sc(cx - w / 2 - 10), sc(top - 22), sc(cx + w / 2 + 10), sc(top + 30)), radius=sc(26), fill=grass + (255,))
    for i in range(int(w / 26) + 1):
        bx = cx - w / 2 + i * 26
        ld.ellipse((sc(bx - 16), sc(top + 14), sc(bx + 16), sc(top + 42)), fill=grass + (255,))
    ld.rounded_rectangle((sc(cx - w / 2), sc(top - 16), sc(cx + w / 2), sc(top - 4)), radius=sc(6), fill=grass_hi + (255,))
    return lay

# back islands (smaller, lighter), front island (big)
art = Image.alpha_composite(art, island(250, 470, 170, 120, (110, 205, 110), (185, 240, 160), (170, 140, 120), (135, 108, 90)))
art = Image.alpha_composite(art, island(810, 420, 140, 100, (110, 205, 110), (185, 240, 160), (170, 140, 120), (135, 108, 90)))
art = Image.alpha_composite(art, island(520, 730, 460, 200))
d = ImageDraw.Draw(art)

# springboard on the front island: base plate, coil, launch pad
bx, by = 520, 712
d.rounded_rectangle((sc(bx - 80), sc(by - 18), sc(bx + 80), sc(by + 4)), radius=sc(8), fill=(60, 70, 90, 255))
for i in range(5):                                      # zig-zag coil
    yA = by - 22 - i * 22; yB = yA - 11
    d.line([(sc(bx - 52), sc(yA)), (sc(bx + 52), sc(yB))], fill=(200, 210, 225, 255), width=sc(12))
    d.line([(sc(bx + 52), sc(yB)), (sc(bx - 52), sc(yB - 11))], fill=(150, 160, 180, 255), width=sc(12))
pad_y = by - 22 - 5 * 22
d.rounded_rectangle((sc(bx - 96), sc(pad_y - 26), sc(bx + 96), sc(pad_y + 4)), radius=sc(14), fill=(255, 120, 60, 255))
d.rounded_rectangle((sc(bx - 88), sc(pad_y - 24), sc(bx + 88), sc(pad_y - 12)), radius=sc(6), fill=(255, 180, 120, 255))
# boing lines
for ang in (-150, -120, -60, -30):
    a = math.radians(ang); r0, r1 = 120, 160
    d.line([(sc(bx + r0 * math.cos(a)), sc(pad_y + r0 * math.sin(a) * 0.6)), (sc(bx + r1 * math.cos(a)), sc(pad_y + r1 * math.sin(a) * 0.6))],
           fill=(255, 255, 255, 230), width=sc(9))

# coin fountain: coins along spreading arcs, spinning (varied widths)
def coin(cx, cy, r, squash):
    shadow = (196, 120, 16, 255)
    d.ellipse((sc(cx - r * squash), sc(cy - r), sc(cx + r * squash), sc(cy + r)), fill=shadow)
    d.ellipse((sc(cx - (r - 6) * squash), sc(cy - r + 6), sc(cx + (r - 6) * squash), sc(cy + r - 6)), fill=(255, 204, 48, 255))
    if squash > 0.35:
        d.ellipse((sc(cx - (r - 16) * squash), sc(cy - r + 16), sc(cx + (r - 16) * squash), sc(cy + r - 16)), outline=(232, 160, 24, 255), width=sc(4))
    d.ellipse((sc(cx - r * 0.55 * squash), sc(cy - r * 0.7), sc(cx - r * 0.15 * squash), sc(cy - r * 0.2)), fill=(255, 246, 190, 255))

coins = []
# fountain: each coin follows its own parabola out of the pad, staggered in time
paths = [(-300, 0.62), (-210, 0.38), (-120, 0.80), (-60, 0.30), (40, 0.55), (110, 0.86),
         (190, 0.34), (260, 0.70), (330, 0.95), (-250, 0.95), (0, 0.98)]
for k, (vx, t) in enumerate(paths):
    x = bx + vx * t
    y = pad_y - 50 - 760 * t + 470 * t * t
    r = 34 - 8 * t
    coins.append((x, y, r, abs(math.cos(k * 1.3)) * 0.85 + 0.15))
for x, y, r, sq in sorted(coins, key=lambda c: c[1]):
    coin(x, y, r, sq)

# sparkles
for (x, y, r) in ((700, 300, 16), (330, 330, 12), (600, 210, 11), (430, 560, 10)):
    x, y, r = sc(x), sc(y), sc(r)
    d.polygon([(x, y - 2 * r), (x + r * 0.45, y - r * 0.45), (x + 2 * r, y), (x + r * 0.45, y + r * 0.45),
               (x, y + 2 * r), (x - r * 0.45, y + r * 0.45), (x - 2 * r, y), (x - r * 0.45, y - r * 0.45)], fill=(255, 255, 240, 240))

# foreground cloud wisp along the bottom
art = Image.alpha_composite(art, cloud(200, 900, 1.3, 230))
art = Image.alpha_composite(art, cloud(840, 920, 1.2, 230))

mask = Image.new('L', (S, S), 0); ImageDraw.Draw(mask).rounded_rectangle((x0, y0, x1, y1), radius=R, fill=255)
tile = Image.new('RGBA', (S, S), (0, 0, 0, 0)); tile.paste(art, (0, 0), mask)
rim = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ImageDraw.Draw(rim).rounded_rectangle((x0, y0, x1, y1), radius=R, outline=(255, 255, 255, 90), width=sc(4))
tile = Image.alpha_composite(tile, rim)
sh = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ImageDraw.Draw(sh).rounded_rectangle((x0, y0 + sc(12), x1, y1 + sc(12)), radius=R, fill=(0, 0, 0, 110))
out = Image.alpha_composite(sh.filter(ImageFilter.GaussianBlur(sc(20))), tile).resize((1024, 1024), Image.LANCZOS)
out.save('icon_1024.png'); out.resize((512, 512), Image.LANCZOS).save('preview.png'); print('ok')
