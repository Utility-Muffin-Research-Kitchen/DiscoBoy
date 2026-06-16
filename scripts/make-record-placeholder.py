#!/usr/bin/env python3
"""Build the no-album-art placeholder: a vinyl record with an off-white label,
the "Leaf" / "Disco Boy" wordmark snipped from res/header.png, and a black
spindle dot. Output: res/record.png (square, transparent outside the disc)."""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HEADER = os.path.join(ROOT, "discoboy_header.png")
OUT = os.path.join(ROOT, "pak", "res", "record.png")

SS = 3                 # supersample for smooth circles
SZ = 900               # final canvas (square)
S = SZ * SS
C = S // 2
R = C - 2 * SS         # disc radius (small AA margin)

VINYL   = (18, 18, 24, 255)     # near-black vinyl
GROOVE  = (32, 32, 40, 255)     # subtle concentric grooves
RIM     = (60, 60, 72, 255)     # faint outer-edge sheen (helps on dark themes)
LABEL   = (244, 239, 227, 255)  # warm off-white label
DOT     = (8, 8, 10, 255)       # black spindle dot

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

def circle(cx, cy, rad, fill):
    d.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], fill=fill)

def ring(rad, color, width):
    d.ellipse([C - rad, C - rad, C + rad, C + rad], outline=color, width=width)

# vinyl disc + faint edge sheen
circle(C, C, R, VINYL)
ring(R - SS, RIM, 2 * SS)
# concentric grooves between label and rim
label_r = int(R * 0.40)
g = label_r + int(R * 0.06)
while g < R - int(R * 0.05):
    ring(g, GROOVE, max(1, SS))
    g += int(R * 0.035)

# off-white label
circle(C, C, label_r, LABEL)

# --- snip the wordmark text from the header ---
hdr = Image.open(HEADER).convert("RGBA")

def snip(x0, x1):
    crop = hdr.crop((x0, 0, x1, hdr.height))
    bb = crop.getbbox()
    return crop.crop(bb)

leaf = snip(0, 590)          # "Leaf"
disco = snip(600, hdr.width)  # "Disco Boy"

def place(snip_img, target_w, cy_off):
    w = target_w
    h = int(snip_img.height * w / snip_img.width)
    rs = snip_img.resize((w, h), Image.LANCZOS)
    x = C - w // 2
    y = C + cy_off - h // 2
    img.alpha_composite(rs, (x, y))

place(leaf,  int(R * 0.34), -int(R * 0.17))
place(disco, int(R * 0.52),  int(R * 0.18))

# black spindle dot last (sits over nothing, dead center)
circle(C, C, int(R * 0.048), DOT)

out = img.resize((SZ, SZ), Image.LANCZOS)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
out.save(OUT)
print("wrote", OUT, out.size)
