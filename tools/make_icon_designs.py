# Five original app-icon designs for NSMBW Recomp (no third-party artwork).
import math, random, sys
from PIL import Image, ImageDraw, ImageFilter

S = 2048
def sc(v): return int(round(v * S / 1024))
X0, Y0, X1, Y1, R = sc(100), sc(100), sc(924), sc(924), sc(185)

def vgrad(stops):
    img = Image.new('RGBA', (S, S)); d = ImageDraw.Draw(img)
    for y in range(S):
        t = y / (S - 1)
        for i in range(len(stops) - 1):
            t0, c0 = stops[i]; t1, c1 = stops[i + 1]
            if t0 <= t <= t1:
                u = (t - t0) / max(t1 - t0, 1e-6); c = tuple(int(c0[k] + (c1[k] - c0[k]) * u) for k in range(3)); break
        d.line([(0, y), (S, y)], fill=c + (255,))
    return img

def finish(art, name):
    mask = Image.new('L', (S, S), 0); ImageDraw.Draw(mask).rounded_rectangle((X0, Y0, X1, Y1), radius=R, fill=255)
    tile = Image.new('RGBA', (S, S), (0, 0, 0, 0)); tile.paste(art, (0, 0), mask)
    rim = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ImageDraw.Draw(rim).rounded_rectangle((X0, Y0, X1, Y1), radius=R, outline=(255, 255, 255, 80), width=sc(4))
    tile = Image.alpha_composite(tile, rim)
    sh = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ImageDraw.Draw(sh).rounded_rectangle((X0, Y0 + sc(12), X1, Y1 + sc(12)), radius=R, fill=(0, 0, 0, 110))
    out = Image.alpha_composite(sh.filter(ImageFilter.GaussianBlur(sc(20))), tile).resize((1024, 1024), Image.LANCZOS)
    out.save(f'design_{name}.png')

def glow_layer(draw_fn, blur):
    lay = Image.new('RGBA', (S, S), (0, 0, 0, 0)); draw_fn(ImageDraw.Draw(lay))
    return lay.filter(ImageFilter.GaussianBlur(sc(blur)))

# ---------------------------------------------------------------- 1 arcade chip
def arcade_chip():
    art = vgrad([(0, (36, 24, 70)), (1, (12, 10, 30))])
    d = ImageDraw.Draw(art)
    # circuit traces radiating to the tile edges
    random.seed(3)
    for i in range(22):
        side = i % 4; t = random.uniform(0.15, 0.85)
        sx, sy = [(512 - 200 + 400 * t, 312), (712, 312 + 400 * t), (512 - 200 + 400 * t, 712), (312, 312 + 400 * t)][side]
        ex, ey = [(sx, 120), (904, sy), (sx, 904), (120, sy)][side]
        mx, my = ((sx + ex) / 2, sy) if side in (1, 3) else (sx, (sy + ey) / 2)
        jog = random.choice((-40, 40))
        pts = [(sx, sy), (mx, my), (mx + (jog if side in (0, 2) else 0), my + (jog if side in (1, 3) else 0)), (ex + (jog if side in (0, 2) else 0), ey + (jog if side in (1, 3) else 0))]
        col = random.choice(((90, 220, 255), (255, 90, 200), (140, 255, 160)))
        art = Image.alpha_composite(art, glow_layer(lambda g: g.line([(sc(x), sc(y)) for x, y in pts], fill=col + (200,), width=sc(10)), 10))
        d = ImageDraw.Draw(art)
        d.line([(sc(x), sc(y)) for x, y in pts], fill=col + (255,), width=sc(5))
        x, y = pts[-1]; d.ellipse((sc(x - 9), sc(y - 9), sc(x + 9), sc(y + 9)), fill=col + (255,))
    # chip body with pins
    for i in range(7):
        o = 330 + i * 55
        for (a, b, c, e) in ((o, 290, o + 24, 318), (o, 706, o + 24, 734), (290, o, 318, o + 24), (706, o, 734, o + 24)):
            d.rounded_rectangle((sc(a), sc(b), sc(c), sc(e)), radius=sc(5), fill=(200, 205, 220, 255))
    d.rounded_rectangle((sc(312), sc(312), sc(712), sc(712)), radius=sc(48), fill=(48, 52, 72, 255))
    d.rounded_rectangle((sc(330), sc(330), sc(694), sc(694)), radius=sc(38), fill=(70, 76, 104, 255))
    # gamepad on the chip
    gx, gy = 512, 520
    d.rounded_rectangle((sc(gx - 150), sc(gy - 80), sc(gx + 150), sc(gy + 70)), radius=sc(70), fill=(240, 242, 250, 255))
    d.ellipse((sc(gx - 190), sc(gy - 40), sc(gx - 60), sc(gy + 120)), fill=(240, 242, 250, 255))
    d.ellipse((sc(gx + 60), sc(gy - 40), sc(gx + 190), sc(gy + 120)), fill=(240, 242, 250, 255))
    d.rectangle((sc(gx - 110), sc(gy - 12), sc(gx - 50), sc(gy + 12)), fill=(60, 64, 90, 255))       # d-pad
    d.rectangle((sc(gx - 92), sc(gy - 30), sc(gx - 68), sc(gy + 30)), fill=(60, 64, 90, 255))
    for (bx, by, col) in ((gx + 80, gy - 18, (255, 90, 120)), (gx + 116, gy + 14, (90, 200, 255))):
        d.ellipse((sc(bx - 20), sc(by - 20), sc(bx + 20), sc(by + 20)), fill=col + (255,))
    # ">>" chevrons above the pad
    for off in (-34, 22):
        d.line([(sc(gx + off - 22), sc(400)), (sc(gx + off + 12), sc(430)), (sc(gx + off - 22), sc(460))], fill=(120, 230, 255, 255), width=sc(14), joint='curve')
    finish(art, 'arcade_chip')

# ---------------------------------------------------------------- 2 pixel leap
def pixel_leap():
    P = 32                                     # 32x32 pixel grid inside the tile area
    grid = Image.new('RGBA', (P, P), (0, 0, 0, 0)); g = ImageDraw.Draw(grid)
    for y in range(P):                          # sky bands
        c = [(96, 104, 230), (110, 130, 240), (130, 160, 250), (160, 196, 255)][min(3, y * 4 // 22)]
        g.line([(0, y), (P, y)], fill=c + (255,))
    for (x, y, w) in ((4, 5, 5), (21, 8, 6), (13, 3, 3)):              # pixel clouds
        g.rectangle((x, y, x + w, y + 1), fill=(255, 255, 255, 255)); g.rectangle((x + 1, y - 1, x + w - 1, y - 1), fill=(255, 255, 255, 255))
    # platforms
    for (x, y, w) in ((1, 25, 10), (19, 21, 10), (12, 28, 8)):
        g.rectangle((x, y, x + w, y), fill=(120, 220, 90, 255)); g.rectangle((x, y + 1, x + w, y + 3), fill=(170, 110, 70, 255))
        for k in range(x, x + w + 1, 2): g.point((k, y + 2), fill=(140, 86, 56, 255))
    g.rectangle((0, 30, P, P), fill=(170, 110, 70, 255)); g.rectangle((0, 29, P, 29), fill=(120, 220, 90, 255))
    # gems along the jump arc
    for (x, y) in ((9, 18), (13, 14), (17, 14)):
        g.polygon([(x, y - 1), (x + 1, y), (x, y + 1), (x - 1, y)], fill=(255, 220, 60, 255)); g.point((x, y), fill=(255, 250, 200, 255))
    # original slime hero mid-jump (rounded blob, big eyes, antenna)
    hx, hy = 21, 13
    body = [(hx - 2, hy - 3, hx + 2, hy - 3), (hx - 3, hy - 2, hx + 3, hy + 2), (hx - 2, hy + 3, hx + 2, hy + 3)]
    for b in body: g.rectangle(b, fill=(250, 110, 170, 255))
    g.rectangle((hx - 2, hy - 2, hx - 1, hy - 1), fill=(255, 190, 220, 255))
    g.point((hx - 1, hy), fill=(30, 20, 50, 255)); g.point((hx + 1, hy), fill=(30, 20, 50, 255))
    g.point((hx, hy - 4), fill=(250, 110, 170, 255)); g.point((hx, hy - 5), fill=(255, 230, 90, 255))
    for (x, y) in ((hx - 5, hy + 3), (hx - 7, hy + 5)): g.point((x, y), fill=(255, 255, 255, 200))   # motion dots
    art = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    big = grid.resize((X1 - X0, Y1 - Y0), Image.NEAREST)
    art.paste(big, (X0, Y0))
    finish(art, 'pixel_leap')

# ---------------------------------------------------------------- 3 neon synthwave
def neon():
    art = vgrad([(0, (20, 6, 50)), (0.55, (90, 20, 110)), (0.56, (30, 8, 60)), (1, (10, 4, 30))])
    # striped sun
    sun = Image.new('RGBA', (S, S), (0, 0, 0, 0)); sd = ImageDraw.Draw(sun)
    for y in range(sc(330), sc(560)):
        t = (y - sc(330)) / (sc(230))
        half = math.sqrt(max(0, sc(170) ** 2 - (y - sc(500)) ** 2)) if y > sc(330) else 0
        if int(t * 9) % 2 == 1 and t > 0.45: continue
        c = (255, int(220 - 140 * t), int(80 + 60 * t))
        sd.line([(sc(512) - half, y), (sc(512) + half, y)], fill=c + (255,))
    art = Image.alpha_composite(art, sun.filter(ImageFilter.GaussianBlur(sc(1))))
    art = Image.alpha_composite(art, glow_layer(lambda g: g.ellipse((sc(330), sc(320), sc(694), sc(690)), fill=(255, 100, 160, 70)), 40))
    d = ImageDraw.Draw(art)
    # perspective grid
    hy = 575
    for i in range(-10, 11):
        d.line([(sc(512 + i * 30), sc(hy)), (sc(512 + i * 140), sc(940))], fill=(255, 80, 220, 255), width=sc(4))
    for k in range(1, 9):
        y = hy + (k / 8) ** 1.8 * 370
        d.line([(0, sc(y)), (S, sc(y))], fill=(255, 80, 220, 255), width=sc(4))
    # neon bounce arc + ball
    def arc(g, w, a):
        pts = [(sc(250 + 520 * t), sc(800 - 520 * math.sin(math.pi * t))) for t in [i / 60 for i in range(61)]]
        g.line(pts, fill=(90, 240, 255, a), width=sc(w), joint='curve')
    art = Image.alpha_composite(art, glow_layer(lambda g: arc(g, 26, 180), 18))
    d = ImageDraw.Draw(art); arc(d, 8, 255)
    bx, by = 512, 280
    art = Image.alpha_composite(art, glow_layer(lambda g: g.ellipse((sc(bx - 70), sc(by - 70), sc(bx + 70), sc(by + 70)), fill=(255, 240, 120, 200)), 22))
    d = ImageDraw.Draw(art)
    d.ellipse((sc(bx - 46), sc(by - 46), sc(bx + 46), sc(by + 46)), fill=(255, 250, 200, 255))
    for (x, y) in ((180, 200), (850, 170), (760, 300), (300, 140)):
        d.ellipse((sc(x - 4), sc(y - 4), sc(x + 4), sc(y + 4)), fill=(255, 255, 255, 230))
    finish(art, 'neon')

# ---------------------------------------------------------------- 4 paper craft
def paper():
    art = vgrad([(0, (255, 236, 214)), (1, (255, 214, 190))])
    def layer(base, amp, ph, col, shadow=True):
        lay = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ld = ImageDraw.Draw(lay)
        pts = [(x, sc(base) - sc(amp) * math.sin(x / S * math.pi * 2.2 + ph)) for x in range(0, S + 8, 8)]
        ld.polygon(pts + [(S, S), (0, S)], fill=col + (255,))
        out = lay
        if shadow:
            sh = Image.new('RGBA', (S, S), (0, 0, 0, 0)); ImageDraw.Draw(sh).polygon([(x, y - sc(6)) for x, y in pts] + [(S, S), (0, S)], fill=(80, 40, 30, 90))
            out = Image.alpha_composite(sh.filter(ImageFilter.GaussianBlur(sc(10))), lay)
        return out
    # sun disc
    d = ImageDraw.Draw(art); d.ellipse((sc(620), sc(200), sc(800), sc(380)), fill=(255, 150, 90, 255))
    for (b, a, p, c) in ((560, 60, 0.4, (250, 180, 150)), (640, 70, 2.0, (240, 140, 120)), (720, 60, 3.4, (200, 100, 110)), (800, 55, 1.2, (150, 70, 100)), (880, 40, 4.5, (100, 50, 90))):
        art = Image.alpha_composite(art, layer(b, a, p, c))
    d = ImageDraw.Draw(art)
    # dashed loop path + paper plane
    pts = []
    for i in range(0, 200):
        t = i / 199
        x = 200 + 600 * t + 90 * math.sin(t * math.pi * 2)
        y = 520 - 200 * t + 110 * math.cos(t * math.pi * 2) - 110
        pts.append((x, y))
    for i in range(0, len(pts) - 6, 12):
        d.line([(sc(pts[i][0]), sc(pts[i][1])), (sc(pts[i + 6][0]), sc(pts[i + 6][1]))], fill=(255, 255, 255, 255), width=sc(8))
    px, py = 770, 200
    plane = [(px + 90, py - 30), (px - 80, py - 10), (px - 20, py + 10), (px - 40, py + 70)]
    d.polygon([(sc(x), sc(y)) for x, y in (plane[0], plane[1], plane[2])], fill=(255, 255, 255, 255))
    d.polygon([(sc(x), sc(y)) for x, y in (plane[0], plane[2], plane[3])], fill=(225, 225, 235, 255))
    d.line([(sc(plane[0][0]), sc(plane[0][1])), (sc(plane[2][0]), sc(plane[2][1]))], fill=(200, 200, 215, 255), width=sc(4))
    finish(art, 'paper')

# ---------------------------------------------------------------- 5 minimal bounce
def minimal():
    art = vgrad([(0, (255, 92, 92)), (1, (255, 150, 70))])
    d = ImageDraw.Draw(art)
    # bounce trail: three fading ghost balls on an arc
    ghosts = Image.new('RGBA', (S, S), (0, 0, 0, 0)); gd = ImageDraw.Draw(ghosts)
    for i, (x, y, a) in enumerate(((300, 640, 60), (380, 470, 110), (470, 360, 170))):
        gd.ellipse((sc(x - 70), sc(y - 70), sc(x + 70), sc(y + 70)), fill=(255, 255, 255, a))
    gd.ellipse((sc(230), sc(720), sc(370), sc(760)), fill=(255, 255, 255, 120))
    for (x, y, L) in ((700, 280, 120), (720, 340, 160), (700, 400, 110)):
        gd.rounded_rectangle((sc(x + 20), sc(y - 9), sc(x + 20 + L), sc(y + 9)), radius=sc(9), fill=(255, 255, 255, 220))
    art = Image.alpha_composite(art, ghosts); d = ImageDraw.Draw(art)
    bx, by = 590, 330
    d.ellipse((sc(bx - 110), sc(by - 110), sc(bx + 110), sc(by + 110)), fill=(255, 255, 255, 255))
    d.ellipse((sc(bx - 60), sc(by - 70), sc(bx - 10), sc(by - 20)), fill=(255, 210, 200, 255))
    # ground line with a squash mark
    d.rounded_rectangle((sc(200), sc(760), sc(824), sc(790)), radius=sc(15), fill=(255, 255, 255, 255))
    finish(art, 'minimal')

for f in (arcade_chip, pixel_leap, neon, paper, minimal): f()
names = ['arcade_chip', 'pixel_leap', 'neon', 'paper', 'minimal']
W = Image.new('RGBA', (5 * 360 + 40, 420), (245, 245, 247, 255)); dd = ImageDraw.Draw(W)
for i, n in enumerate(names):
    W.alpha_composite(Image.open(f'design_{n}.png').resize((340, 340), Image.LANCZOS), (20 + i * 360, 20))
    dd.text((20 + i * 360 + 140, 375), n, fill=(40, 40, 50, 255))
W.save('designs_sheet.png'); print('ok')
