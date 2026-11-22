r"""
make_font.py - turn a TrueType font into a ".sbf" letter sheet the console can draw text from (p360_draw.c).

  py make_font.py <font.ttf> <out.sbf> [--all] [--preview out.png]

The sheet is a "signed distance field": instead of the letter's pixels, every texel stores how far it is from the
letter's EDGE (0.5 = on the edge, more = inside, less = outside). The GPU turns that back into a sharp edge at any
size, so ONE sheet serves small captions and big titles. Letters are drawn here 4x larger than stored (SUPER) so
the distances are accurate.

Characters: ASCII, Latin-1 (accents: e.g. French / German / Spanish / Portuguese), Latin Extended-A (e.g. Polish,
Czech, Turkish), and common typography (' ' " " - - ... bullet euro tm). Other scripts (Chinese, Arabic, ...) are a
later step. Kerning pairs (e.g. "AV", "To") are measured for ASCII.

File layout (big-endian, as the 360 reads it):
  "SBF1", u16 base size, u16 spread, i16 ascent*16, i16 descent*16, i16 line height*16, u16 sheet w, u16 sheet h,
  u16 glyph count, u16 kerning count
  glyphs  (sorted by code): u32 code, u16 x, y, w, h (on the sheet), i16 x offset*16, y offset*16 (from the pen /
          baseline to the cell's top-left), i16 advance*16
  kerning: u16 first, u16 second, i16 adjust*16
  sheet:  w*h bytes
"""
import math, struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

BASE = 48          # stored letter size in pixels (per em)
SUPER = 4          # drawn this much bigger to measure distances
SPREAD = 6         # distance range in sheet texels (edge +- SPREAD -> 1.0 / 0.0)
SHEET_W = 1024

CHARS = (list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + list(range(0x100, 0x180)) +
         [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x20AC, 0x2122])

# --all: every character the font has in these ranges (other languages: 2026-11-22). Left out: combining accents
# (they need placing on the letter before) and right-to-left scripts (Hebrew / Arabic: drawn backwards otherwise)
RANGES = [(0x20, 0x7E), (0xA0, 0x24F),          # Latin incl. Extended-A / B (European languages, Romanian, ...)
          (0x250, 0x2FF),                        # IPA + spacing modifier letters
          (0x370, 0x3FF), (0x1F00, 0x1FFF),      # Greek + polytonic Greek
          (0x400, 0x52F),                        # Cyrillic (Russian, Ukrainian, Serbian, Bulgarian, Kazakh, ...)
          (0x1E00, 0x1EFF),                      # Latin Extended Additional (Vietnamese, Welsh, ...)
          (0x2000, 0x206F), (0x20A0, 0x20CF), (0x2100, 0x214F)]   # punctuation, currency signs, letter-like


def chars_for(src):
    from fontTools.ttLib import TTFont
    have = TTFont(src).getBestCmap()
    out = [cp for a, b in RANGES for cp in range(a, b + 1) if cp in have]
    out += [cp for cp in CHARS if cp in have and cp not in out]
    return sorted(set(out))


def sdf_glyph(font_hi, ch):
    """-> (field as uint8 array (h, w), x offset, y offset) in sheet texels, or None for an empty glyph"""
    left, top, right, bottom = font_hi.getbbox(ch, anchor="ls")
    if right <= left or bottom <= top:
        return None
    pad = SPREAD * SUPER
    ow = int(math.ceil((right - left + 2 * pad) / SUPER))
    oh = int(math.ceil((bottom - top + 2 * pad) / SUPER))
    img = Image.new("L", (ow * SUPER, oh * SUPER), 0)
    ImageDraw.Draw(img).text((pad - left, pad - top), ch, font=font_hi, fill=255, anchor="ls")
    hi = np.asarray(img) >= 128
    # edge pixels: inside pixels with an outside neighbour (4 directions)
    padded = np.pad(hi, 1)
    neighbour_out = (~padded[:-2, 1:-1]) | (~padded[2:, 1:-1]) | (~padded[1:-1, :-2]) | (~padded[1:-1, 2:])
    ey, ex = np.nonzero(hi & neighbour_out)
    if len(ey) == 0:
        return None
    edges = np.stack([ex + 0.5, ey + 0.5], axis=1).astype(np.float32)
    # sample points: the centres of the stored texels, in the big picture's pixels
    sy, sx = np.mgrid[0:oh, 0:ow]
    pts = np.stack([(sx.ravel() + 0.5) * SUPER, (sy.ravel() + 0.5) * SUPER], axis=1).astype(np.float32)
    best = np.full(len(pts), 1e9, dtype=np.float32)
    for i in range(0, len(edges), 512):
        e = edges[i:i + 512]
        d = (pts[:, None, 0] - e[None, :, 0]) ** 2 + (pts[:, None, 1] - e[None, :, 1]) ** 2
        best = np.minimum(best, d.min(axis=1))
    dist = np.sqrt(best) / SUPER                                    # in sheet texels
    inside = hi[np.minimum((pts[:, 1]).astype(int), hi.shape[0] - 1), np.minimum(pts[:, 0].astype(int), hi.shape[1] - 1)]
    signed = np.where(inside, dist, -dist)
    value = np.clip(0.5 + signed / (2 * SPREAD), 0, 1)
    field = (value * 255 + 0.5).astype(np.uint8).reshape(oh, ow)
    return field, (left - pad) / SUPER, (top - pad) / SUPER


def main():
    src, out = sys.argv[1], sys.argv[2]
    preview = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    font = ImageFont.truetype(src, BASE)
    font_hi = ImageFont.truetype(src, BASE * SUPER)
    ascent, descent = font.getmetrics()
    line = ascent + descent

    glyphs = []
    chars = chars_for(src) if "--all" in sys.argv else CHARS
    for cp in chars:
        ch = chr(cp)
        if cp != 0x20 and font.getmask(ch).getbbox() is None and font.getlength(ch) == font.getlength("￿"):
            continue                                              # not in this font
        adv = font_hi.getlength(ch) / SUPER
        g = sdf_glyph(font_hi, ch)
        glyphs.append({"cp": cp, "adv": adv, "field": g[0] if g else None,
                       "xoff": g[1] if g else 0, "yoff": g[2] if g else 0})

    # pack the cells onto the sheet in rows (tallest first)
    order = sorted([g for g in glyphs if g["field"] is not None], key=lambda g: -g["field"].shape[0])
    x = y = row_h = 0
    for g in order:
        h, w = g["field"].shape
        if x + w > SHEET_W:
            x, y, row_h = 0, y + row_h + 1, 0
        g["x"], g["y"] = x, y
        x += w + 1
        row_h = max(row_h, h)
    sheet_h = ((y + row_h + 31) // 32) * 32
    sheet = np.zeros((sheet_h, SHEET_W), dtype=np.uint8)
    for g in order:
        h, w = g["field"].shape
        sheet[g["y"]:g["y"] + h, g["x"]:g["x"] + w] = g["field"]

    # kerning (ASCII pairs): how much closer/further than the two advances added up
    kerns = []
    ascii_cps = [g["cp"] for g in glyphs if 0x21 <= g["cp"] < 0x7F]
    adv_base = {cp: font_hi.getlength(chr(cp)) for cp in ascii_cps}
    for a in ascii_cps:
        for b in ascii_cps:
            k = (font_hi.getlength(chr(a) + chr(b)) - adv_base[a] - adv_base[b]) / SUPER
            if abs(k) >= 0.4:
                kerns.append((a, b, k))

    f16 = lambda v: int(round(v * 16))
    data = bytearray(b"SBF1")
    data += struct.pack(">HHhhhHHHH", BASE, SPREAD, f16(ascent), f16(descent), f16(line), SHEET_W, sheet_h,
                        len(glyphs), len(kerns))
    for g in sorted(glyphs, key=lambda g: g["cp"]):
        if g["field"] is None:
            data += struct.pack(">IHHHHhhh", g["cp"], 0, 0, 0, 0, 0, 0, f16(g["adv"]))
        else:
            h, w = g["field"].shape
            data += struct.pack(">IHHHHhhh", g["cp"], g["x"], g["y"], w, h, f16(g["xoff"]), f16(g["yoff"]),
                                f16(g["adv"]))
    for a, b, k in kerns:
        data += struct.pack(">HHh", a, b, f16(k))
    data += sheet.tobytes()
    open(out, "wb").write(data)
    print("%s: %d letters, %d kerning pairs, sheet %dx%d, %.0f KB" % (out, len(glyphs), len(kerns), SHEET_W, sheet_h,
                                                                      len(data) / 1024))
    if preview:
        Image.fromarray(sheet).save(preview)


if __name__ == "__main__":
    main()
