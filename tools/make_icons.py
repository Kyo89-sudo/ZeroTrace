# Genera le icone di ZeroTrace:
#  - icona applicazione in stile macOS Sequoia (squircle, gradiente, ombra morbida, glifo lucido)
#  - icone della barra strumenti (glifi colorati)
# uso: python3 make_icons.py <cartella src> <cartella test/StubbornApp> [preview.png]
import math, sys, os
from PIL import Image, ImageDraw, ImageFilter, ImageChops

SS = 1024  # tela di lavoro (come i template di Apple)

def squircle_mask(size, inset, n=5.0):
    """Superellisse |x|^n + |y|^n = 1 (forma delle icone macOS)."""
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    r = (size - 2 * inset) / 2
    c = size / 2
    pts = []
    for i in range(720):
        t = 2 * math.pi * i / 720
        ct, st = math.cos(t), math.sin(t)
        x = c + r * math.copysign(abs(ct) ** (2 / n), ct)
        y = c + r * math.copysign(abs(st) ** (2 / n), st)
        pts.append((x, y))
    d.polygon(pts, fill=255)
    return m

def vgradient(size, top, bottom):
    g = Image.new("RGBA", (1, 256))
    for y in range(256):
        t = y / 255
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4)))
    return g.resize((size, size), Image.BICUBIC)

def rgradient(size, center, inner, outer, radius):
    im = Image.new("RGBA", (size, size))
    px = im.load()
    for y in range(0, size, 4):
        for x in range(0, size, 4):
            dd = min(1, math.hypot(x - center[0], y - center[1]) / radius)
            col = tuple(int(inner[i] + (outer[i] - inner[i]) * dd) for i in range(4))
            for yy in range(y, min(y + 4, size)):
                for xx in range(x, min(x + 4, size)):
                    px[xx, yy] = col
    return im

def composite_squircle(top, bottom, glyph_fn, inset=100, shadow=True):
    S = SS
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    mask = squircle_mask(S, inset)
    if shadow:
        sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        sm = mask.filter(ImageFilter.GaussianBlur(22))
        sh.putalpha(sm.point(lambda v: int(v * 0.38)))
        canvas.alpha_composite(sh, (0, 14))
    body = vgradient(S, top, bottom)
    # luce morbida dall'alto
    glow = rgradient(S, (S * 0.5, S * 0.12), (255, 255, 255, 70), (255, 255, 255, 0), S * 0.75)
    body.alpha_composite(glow)
    body.putalpha(mask)
    canvas.alpha_composite(body)
    # bordo interno sottile (come le icone di sistema)
    edge = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    inner = squircle_mask(S, inset + 4)
    ring = ImageChops.subtract(mask, inner)
    edge.putalpha(ring.point(lambda v: int(v * 0.35)))
    canvas.alpha_composite(edge)
    glyph_fn(canvas, mask)
    return canvas

def glyph_layer(draw_fn, shade=True):
    """Disegna il glifo bianco su layer separato, con ombra e leggera sfumatura."""
    S = SS
    g = Image.new("L", (S, S), 0)
    draw_fn(ImageDraw.Draw(g))
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    if shade:
        sh = Image.new("RGBA", (S, S), (10, 20, 60, 0))
        sh.putalpha(g.filter(ImageFilter.GaussianBlur(16)).point(lambda v: int(v * 0.45)))
        layer.alpha_composite(sh, (0, 18))
    white = vgradient(S, (255, 255, 255, 255), (222, 232, 248, 255))
    white.putalpha(g)
    layer.alpha_composite(white)
    return layer

# ---------------------------------------------------------------- ZeroTrace
def zerotrace_glyph(canvas, mask):
    def draw(d):
        # coperchio inclinato
        cx = 512
        lid = [(318, 318), (706, 268), (716, 318), (328, 368)]
        d.polygon(lid, fill=255)
        d.rounded_rectangle([462, 236, 572, 286], 18, fill=255)
        # corpo del cestino (trapezio arrotondato)
        body = [(352, 410), (672, 410), (640, 772), (384, 772)]
        d.polygon(body, fill=255)
        d.rounded_rectangle([352, 400, 672, 440], 20, fill=255)
        d.rounded_rectangle([384, 730, 640, 790], 28, fill=255)
        # scanalature (bucate)
        for x in (442, 512, 582):
            d.rounded_rectangle([x - 16, 470, x + 16, 720], 16, fill=0)
    canvas.alpha_composite(glyph_layer(draw))
    # scintille "pulito"
    def sparkles(d):
        def star(cx, cy, r):
            pts = []
            for i in range(8):
                rr = r if i % 2 == 0 else r * 0.28
                a = math.pi / 4 * i - math.pi / 2
                pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
            d.polygon(pts, fill=255)
        star(752, 412, 74)
        star(820, 560, 40)
        star(250, 470, 36)
    sp = Image.new("L", (SS, SS), 0)
    sparkles(ImageDraw.Draw(sp))
    glow = Image.new("RGBA", (SS, SS), (255, 250, 200, 0))
    glow.putalpha(sp.filter(ImageFilter.GaussianBlur(18)).point(lambda v: int(v * 0.8)))
    canvas.alpha_composite(glow)
    col = Image.new("RGBA", (SS, SS), (255, 247, 205, 255))
    col.putalpha(sp)
    canvas.alpha_composite(col)

# ---------------------------------------------------------------- StubbornApp
def stubborn_glyph(canvas, mask):
    def draw(d):
        d.rounded_rectangle([322, 470, 702, 790], 56, fill=255)
        d.arc([382, 236, 642, 560], 180, 360, fill=255, width=70)
        d.rectangle([382, 396, 452, 480], fill=255)
        d.rectangle([572, 396, 642, 480], fill=255)
        d.ellipse([472, 568, 552, 648], fill=0)
        d.rounded_rectangle([496, 610, 528, 712], 14, fill=0)
    canvas.alpha_composite(glyph_layer(draw))

def save_ico(img, path, small_inset_boost=True):
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    frames = []
    for s in sizes:
        src = img
        if s <= 32 and small_inset_boost:
            # alle dimensioni piccole riduce il margine per leggibilità (come fa Windows)
            crop = 70
            src = img.crop((crop, crop, SS - crop, SS - crop))
        frames.append(src.resize((s, s), Image.LANCZOS))
    frames[-1].save(path, format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[:-1])

# ---------------------------------------------------------------- toolbar
def tb_icon(name, draw_fn):
    S = 512
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    draw_fn(im, ImageDraw.Draw(im))
    return im

def grad_fill(im, shape_mask, top, bottom):
    g = vgradient(im.width, top, bottom)
    g.putalpha(shape_mask)
    im.alpha_composite(g)

def mk_mask(S, fn):
    m = Image.new("L", (S, S), 0); fn(ImageDraw.Draw(m)); return m

def ic_uninstall(im, d):
    S = im.width
    m = mk_mask(S, lambda d: (d.polygon([(130, 190), (382, 190), (356, 450), (156, 450)], fill=255),
                              d.rounded_rectangle([96, 128, 416, 178], 20, fill=255),
                              d.rounded_rectangle([206, 78, 306, 130], 18, fill=255)))
    grad_fill(im, m, (88, 170, 255, 255), (20, 98, 224, 255))
    for x in (200, 256, 312):
        d.rounded_rectangle([x - 12, 230, x + 12, 410], 12, fill=(255, 255, 255, 200))

def ic_force(im, d):
    S = im.width
    m = mk_mask(S, lambda d: (d.polygon([(130, 190), (382, 190), (356, 450), (156, 450)], fill=255),
                              d.rounded_rectangle([96, 128, 416, 178], 20, fill=255),
                              d.rounded_rectangle([206, 78, 306, 130], 18, fill=255)))
    grad_fill(im, m, (255, 170, 70, 255), (232, 96, 20, 255))
    bolt = [(282, 214), (200, 340), (254, 340), (226, 430), (318, 292), (262, 292), (290, 214)]
    d.polygon(bolt, fill=(255, 255, 255, 245))

def ic_traces(im, d):
    S = im.width
    m = mk_mask(S, lambda d: (d.rounded_rectangle([56, 150, 400, 420], 30, fill=255),
                              d.rounded_rectangle([56, 110, 220, 190], 26, fill=255)))
    grad_fill(im, m, (255, 214, 96, 255), (238, 158, 24, 255))
    d.ellipse([230, 220, 410, 400], outline=(30, 120, 200, 255), width=34, fill=(225, 240, 255, 230))
    d.line([(392, 382), (466, 456)], fill=(30, 120, 200, 255), width=48)

def ic_refresh(im, d):
    col = (36, 178, 96, 255)
    d.arc([96, 96, 416, 416], 200, 340, fill=col, width=56)
    d.arc([96, 96, 416, 416], 20, 160, fill=col, width=56)
    d.polygon([(416, 160), (462, 266), (350, 244)], fill=col)
    d.polygon([(96, 352), (50, 246), (162, 268)], fill=col)

def ic_view(im, d):
    col = (90, 110, 150, 255)
    for r in range(2):
        for c in range(2):
            x, y = 90 + c * 180, 90 + r * 180
            d.rounded_rectangle([x, y, x + 150, y + 150], 30, fill=col)

def ic_folder(im, d):
    S = im.width
    m = mk_mask(S, lambda d: (d.rounded_rectangle([56, 150, 456, 420], 30, fill=255),
                              d.rounded_rectangle([56, 110, 230, 190], 26, fill=255)))
    grad_fill(im, m, (255, 214, 96, 255), (238, 158, 24, 255))
    d.rounded_rectangle([56, 200, 456, 420], 30, fill=(255, 226, 130, 255))

def ic_registry(im, d):
    cols = [(150, 90, 230, 255), (90, 140, 240, 255), (60, 190, 210, 255)]
    boxes = [(70, 250), (250, 250), (160, 80)]
    for (x, y), c in zip(boxes, cols):
        d.rounded_rectangle([x, y, x + 170, y + 170], 30, fill=c)
        d.rounded_rectangle([x + 30, y + 30, x + 140, y + 70], 14, fill=(255, 255, 255, 90))

def save_tb(img, path):
    sizes = [16, 20, 24, 32, 40, 48, 64]
    frames = [img.resize((s, s), Image.LANCZOS) for s in sizes]
    frames[-1].save(path, format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[:-1])

if __name__ == "__main__":
    src, stub = sys.argv[1], sys.argv[2]
    app = composite_squircle((96, 196, 255, 255), (60, 76, 232, 255), zerotrace_glyph)
    save_ico(app, os.path.join(src, "ZeroTrace.ico"))
    app.resize((256, 256), Image.LANCZOS).save(os.path.join(src, "ZeroTrace_256.png"))
    st = composite_squircle((255, 178, 84, 255), (228, 84, 60, 255), stubborn_glyph)
    save_ico(st, os.path.join(stub, "StubbornApp.ico"))
    icons = {"tb_uninstall": ic_uninstall, "tb_force": ic_force, "tb_traces": ic_traces,
             "tb_refresh": ic_refresh, "tb_view": ic_view, "tb_folder": ic_folder, "tb_registry": ic_registry}
    os.makedirs(os.path.join(src, "icons"), exist_ok=True)
    tbs = []
    for n, fn in icons.items():
        im = tb_icon(n, fn)
        save_tb(im, os.path.join(src, "icons", n + ".ico"))
        tbs.append(im)
    if len(sys.argv) > 3:
        prev = Image.new("RGBA", (1100, 420), (246, 247, 250, 255))
        prev.alpha_composite(app.resize((340, 340), Image.LANCZOS), (20, 30))
        prev.alpha_composite(st.resize((200, 200), Image.LANCZOS), (380, 30))
        for i, s in enumerate((128, 64, 32, 16)):
            prev.alpha_composite(app.resize((s, s), Image.LANCZOS), (600 + sum((128, 64, 32, 16)[:i]) + i * 20, 40))
        for i, t in enumerate(tbs):
            prev.alpha_composite(t.resize((64, 64), Image.LANCZOS), (380 + i * 90, 300))
        prev.save(sys.argv[3])
