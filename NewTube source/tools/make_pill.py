r"""make_pill.py - NewTube's rounded "Play all" button picture (the owner's design, 2026-11-22) + the X / Y buttons.

  py make_pill.py <out ui folder>

pill_dark.sbi = 280 x 80: a white rim and a dark gradient inside, round ends (drawn 4x larger and shrunk for smooth
edges). x_button.sbi / y_button.sbi = the owner's button pictures in ..\design\btn_x.png / btn_y.png.
"""
import os, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_ui_art as m

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "samples", "09_newtube", "out", "ui")
os.makedirs(out, exist_ok=True)
W, H, S = 280, 80, 4
big = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
ImageDraw.Draw(big).rounded_rectangle([0, 0, W * S - 1, H * S - 1], radius=H * S // 2, fill=(255, 255, 255, 255))
grad = Image.new("RGBA", (W * S, H * S))
gd = ImageDraw.Draw(grad)
for y in range(H * S):
    c = int(62 - 46 * y / (H * S))                  # dark grey at the top -> nearly black at the bottom
    gd.line([(0, y), (W * S, y)], fill=(c, c, c, 255))
mask = Image.new("L", (W * S, H * S), 0)
r = 5 * S                                           # the white rim's width
ImageDraw.Draw(mask).rounded_rectangle([r, r, W * S - 1 - r, H * S - 1 - r], radius=(H * S - 2 * r) // 2, fill=255)
big.paste(grad, (0, 0), mask)
m.save_sbi(big.resize((W, H), Image.LANCZOS), os.path.join(out, "pill_dark.sbi"))
for name, png in (("x_button", "btn_x.png"), ("y_button", "btn_y.png")):
    src = os.path.join(HERE, "..", "design", png)
    if os.path.exists(src):
        m.png_to_sbi(src, os.path.join(out, name + ".sbi"))
print("pill_dark / x_button / y_button ->", out)
