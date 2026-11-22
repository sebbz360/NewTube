r"""
make_ui_art.py - draw NewTube's interface pictures on the PC and save them as .sbi files for the console.

  py make_ui_art.py <out folder> [--preview preview.png]

.sbi = "SBI1", u32 width, u32 height (big-endian), then one u32 0xAARRGGBB per pixel (big-endian = what the 360
reads directly). Icons are white on see-through (the app tints them); everything is drawn 4x larger and shrunk
for smooth edges. These are PLACEHOLDERS: the owner's own logo / icons replace them (any PNG -> png_to_sbi below).
"""
import math, os, random, struct, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 4                     # drawn this much bigger, then shrunk (smooth edges)
GREEN_TOP, GREEN_BOT = (122, 212, 58), (63, 138, 18)
FONT_BOLD = r"C:\Windows\Fonts\arialbd.ttf"


def save_sbi(img, path):
    img = img.convert("RGBA")
    w, h = img.size
    data = bytearray(b"SBI1" + struct.pack(">II", w, h))
    for r, g, b, a in img.getdata():
        data += struct.pack(">I", (a << 24) | (r << 16) | (g << 8) | b)
    open(path, "wb").write(data)


def png_to_sbi(png, path):
    save_sbi(Image.open(png), path)


def canvas(size):
    return Image.new("RGBA", (size * S, size * S), (0, 0, 0, 0))


def done(img, size):
    return img.resize((size, size), Image.LANCZOS)


W = (255, 255, 255, 255)


def icon_search(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.ellipse([k * .14, k * .12, k * .62, k * .60], outline=W, width=int(k * .09))
    d.line([k * .56, k * .55, k * .86, k * .86], fill=W, width=int(k * .12))
    return done(im, n)


def icon_home(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.polygon([(k * .5, k * .12), (k * .9, k * .5), (k * .1, k * .5)], fill=W)
    d.rectangle([k * .22, k * .48, k * .78, k * .88], fill=W)
    d.rectangle([k * .42, k * .62, k * .58, k * .88], fill=(0, 0, 0, 0))
    return done(im, n)


def icon_gaming(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.rounded_rectangle([k * .08, k * .30, k * .92, k * .66], radius=k * .16, fill=W)
    d.ellipse([k * .06, k * .44, k * .36, k * .82], fill=W)
    d.ellipse([k * .64, k * .44, k * .94, k * .82], fill=W)
    c = (0, 0, 0, 0)
    d.rectangle([k * .20, k * .44, k * .34, k * .50], fill=c)
    d.rectangle([k * .24, k * .40, k * .30, k * .54], fill=c)
    d.ellipse([k * .64, k * .38, k * .72, k * .46], fill=c)
    d.ellipse([k * .72, k * .46, k * .80, k * .54], fill=c)
    return done(im, n)


def icon_music(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.ellipse([k * .14, k * .62, k * .42, k * .86], fill=W)
    d.ellipse([k * .56, k * .54, k * .84, k * .78], fill=W)
    d.rectangle([k * .35, k * .18, k * .42, k * .74], fill=W)
    d.rectangle([k * .77, k * .10, k * .84, k * .66], fill=W)
    d.polygon([(k * .35, k * .18), (k * .84, k * .10), (k * .84, k * .24), (k * .35, k * .32)], fill=W)
    return done(im, n)


def icon_movies(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.rounded_rectangle([k * .10, k * .38, k * .90, k * .86], radius=k * .05, fill=W)
    d.polygon([(k * .10, k * .32), (k * .86, k * .14), (k * .89, k * .26), (k * .13, k * .44)], fill=W)
    c = (0, 0, 0, 0)
    for i in range(4):
        x = k * (.22 + i * .17)
        d.polygon([(x, k * .27 - i * k * .04), (x + k * .07, k * .25 - i * k * .04),
                   (x + k * .02, k * .37 - i * k * .04), (x - k * .05, k * .39 - i * k * .04)], fill=c)
    return done(im, n)


def person(d, k, cx, cy, r):
    d.ellipse([cx - r, cy - r * 1.9, cx + r, cy + r * 0.1], fill=W)
    d.pieslice([cx - r * 1.8, cy + r * 0.4, cx + r * 1.8, cy + r * 3.8], 180, 360, fill=W)


def icon_subscriptions(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    person(d, k, k * .34, k * .42, k * .14)
    person(d, k, k * .68, k * .46, k * .13)
    return done(im, n)


def icon_account(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    person(d, k, k * .5, k * .46, k * .17)
    return done(im, n)


def icon_offline(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.rectangle([k * .44, k * .10, k * .56, k * .48], fill=W)
    d.polygon([(k * .26, k * .44), (k * .74, k * .44), (k * .5, k * .70)], fill=W)
    d.rectangle([k * .14, k * .78, k * .86, k * .88], fill=W)
    return done(im, n)


def icon_gear(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    cx = cy = k * .5
    pts = []
    for i in range(16):
        a = i * math.pi / 8 + math.pi / 16
        r = k * (.44 if (i // 2) % 2 == 0 else .32)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    teeth = []
    for i in range(8):                      # 8 square teeth around a disc
        a = i * math.pi / 4
        for da, r in ((-0.22, .30), (-0.16, .45), (0.16, .45), (0.22, .30)):
            teeth.append((cx + k * r * math.cos(a + da), cy + k * r * math.sin(a + da)))
    d.ellipse([cx - k * .33, cy - k * .33, cx + k * .33, cy + k * .33], fill=W)
    for i in range(8):
        d.polygon(teeth[i * 4:i * 4 + 4], fill=W)
    d.ellipse([cx - k * .13, cy - k * .13, cx + k * .13, cy + k * .13], fill=(0, 0, 0, 0))
    return done(im, n)


def star_points(cx, cy, r_out, r_in):
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = r_out if i % 2 == 0 else r_in
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def icon_star(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.polygon(star_points(k * .5, k * .54, k * .46, k * .19), fill=W)
    return done(im, n)


def badge_star(n=64):
    """the dark square with a grey star in front of each section title (mockup)"""
    im = Image.new("RGBA", (n * S, n * S), (0, 0, 0, 0)); d = ImageDraw.Draw(im); k = n * S
    for y in range(k):                      # dark grey, slightly lighter at the top
        c = int(78 - 26 * y / k)
        d.line([0, y, k, y], fill=(c, c, c, 255))
    d.rectangle([0, 0, k - 1, k - 1], outline=(30, 30, 30, 255), width=S)
    d.polygon(star_points(k * .5, k * .54, k * .38, k * .16), fill=(150, 150, 150, 255))
    return im.resize((n, n), Image.LANCZOS)


def avatar(n=64):
    im = Image.new("RGBA", (n * S, n * S), (200, 200, 200, 255)); d = ImageDraw.Draw(im); k = n * S
    d.rectangle([0, 0, k - 1, k - 1], outline=(255, 255, 255, 255), width=3 * S)
    g = (240, 240, 240, 255)
    d.ellipse([k * .33, k * .18, k * .67, k * .52], fill=g)
    d.pieslice([k * .14, k * .58, k * .86, k * 1.3], 180, 360, fill=g)
    return im.resize((n, n), Image.LANCZOS)


def y_button(n=48):
    own = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "design", "btn_y.png")
    if os.path.exists(own):                     # the owner's own Y button picture (2026-11-22)
        return Image.open(own).convert("RGBA")
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.ellipse([k * .04, k * .04, k * .96, k * .96], fill=(30, 30, 30, 255))
    d.ellipse([k * .10, k * .10, k * .90, k * .90], fill=(245, 200, 30, 255))
    f = ImageFont.truetype(FONT_BOLD, int(k * .62))
    d.text((k * .5, k * .53), "Y", font=f, fill=(20, 20, 20, 255), anchor="mm")
    return done(im, n)


def logo():
    """placeholder 2012-style logo: "New" + "Tube" in a green rounded box (2x size: drawn at half on screen)"""
    h = 120
    f = ImageFont.truetype(FONT_BOLD, int(h * .62 * S / S * S) // S * 1)
    f = ImageFont.truetype(FONT_BOLD, 74)
    w_new = f.getlength("New")
    w_tube = f.getlength("Tube")
    pad = 14
    W_ = int(w_new + 8 + w_tube + 2 * pad) + 4
    im = Image.new("RGBA", (W_, h), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.text((0, h * .52), "New", font=f, fill=(255, 255, 255, 255), anchor="lm")
    bx = int(w_new + 8)
    box = Image.new("RGBA", (W_ - bx, h), (0, 0, 0, 0))
    grad = Image.new("RGBA", box.size)
    gd = ImageDraw.Draw(grad)
    for y in range(h):
        t = y / (h - 1)
        gd.line([0, y, box.size[0], y], fill=tuple(int(GREEN_TOP[i] + (GREEN_BOT[i] - GREEN_TOP[i]) * t)
                                                   for i in range(3)) + (255,))
    mask = Image.new("L", box.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 8, box.size[0] - 1, h - 9], radius=26, fill=255)
    box.paste(grad, (0, 0), mask)
    ImageDraw.Draw(box).text((pad, h * .52), "Tube", font=f, fill=(255, 255, 255, 255), anchor="lm")
    im.alpha_composite(box, (bx, 0))
    return im


def paper(n=256):
    """the light grey page background with a faint grain (tiles seamlessly)"""
    rnd = random.Random(360)
    im = Image.new("L", (n, n))
    im.putdata([236 + rnd.randint(-5, 5) for _ in range(n * n)])
    im = im.filter(ImageFilter.GaussianBlur(0.6))
    return Image.merge("RGBA", (im, im, im, Image.new("L", (n, n), 255)))


def icon_prev(n=64):
    """2012 player: a bar + a left-pointing triangle"""
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.rectangle([k * .22, k * .18, k * .32, k * .82], fill=W)
    d.polygon([(k * .78, k * .18), (k * .78, k * .82), (k * .34, k * .5)], fill=W)
    return done(im, n)


def icon_next(n=64):
    return icon_prev(n).transpose(Image.FLIP_LEFT_RIGHT)


def icon_play(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.polygon([(k * .28, k * .16), (k * .28, k * .84), (k * .82, k * .5)], fill=W)
    return done(im, n)


def icon_pause(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.rectangle([k * .24, k * .18, k * .42, k * .82], fill=W)
    d.rectangle([k * .58, k * .18, k * .76, k * .82], fill=W)
    return done(im, n)


def icon_comment(n=64):
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.rounded_rectangle([k * .08, k * .14, k * .92, k * .70], radius=k * .14, fill=W)
    d.polygon([(k * .24, k * .66), (k * .44, k * .66), (k * .22, k * .88)], fill=W)
    return done(im, n)


def hd_badge():
    """the little red "HD" tag on the 2012 gear button (shown in colour, not tinted)"""
    w, h = 44, 30
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, w * S - 1, h * S - 1], radius=4 * S, fill=(170, 20, 30, 255))
    f = ImageFont.truetype(FONT_BOLD, 21 * S)
    d.text((w * S / 2, h * S * .53), "HD", font=f, fill=(255, 255, 255, 255), anchor="mm")
    return im.resize((w, h), Image.LANCZOS)


def knob(n=32):
    """the 2012 scrub knob: white ball with a grey rim and a soft grey centre"""
    im = canvas(n); d = ImageDraw.Draw(im); k = n * S
    d.ellipse([k * .04, k * .04, k * .96, k * .96], fill=(150, 150, 150, 255))
    d.ellipse([k * .10, k * .10, k * .90, k * .90], fill=(250, 250, 250, 255))
    d.ellipse([k * .36, k * .36, k * .64, k * .64], fill=(205, 205, 205, 255))
    return done(im, n)


ART = {
    "knob": knob, "icon_prev": icon_prev, "icon_next": icon_next, "icon_play": icon_play, "icon_pause": icon_pause,
    "icon_comment": icon_comment, "hd_badge": hd_badge,
    "icon_search": icon_search, "icon_home": icon_home, "icon_gaming": icon_gaming, "icon_music": icon_music,
    "icon_movies": icon_movies, "icon_subscriptions": icon_subscriptions, "icon_account": icon_account,
    "icon_offline": icon_offline, "icon_gear": icon_gear, "icon_star": icon_star, "badge_star": badge_star,
    "avatar": avatar, "y_button": y_button, "logo": logo, "paper": paper,
}


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    made = {}
    for name, fn in ART.items():
        img = fn()
        save_sbi(img, os.path.join(out, name + ".sbi"))
        made[name] = img
        print("%-20s %dx%d" % (name, img.size[0], img.size[1]))
    if "--preview" in sys.argv:
        sheet = Image.new("RGBA", (900, 260), (60, 60, 60, 255))
        x = y = 10
        for name, img in made.items():
            if name == "paper":
                continue
            if x + img.size[0] > 890:
                x, y = 10, y + 130
            sheet.alpha_composite(img, (x, y))
            x += img.size[0] + 12
        sheet.save(sys.argv[sys.argv.index("--preview") + 1])


if __name__ == "__main__":
    main()
