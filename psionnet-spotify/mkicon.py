"""Draw psionnet-spotify.png, the launcher icon, in the style of Psion's own:
48x48, shaded, on a transparent background with a soft shadow. A generic
round "play music" button -- not Spotify's mark, which is theirs."""
import math
from PIL import Image, ImageDraw, ImageFilter

S = 384                                   # draw at 8x, then reduce
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

# soft shadow under the disc
sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(sh).ellipse((60, 300, 324, 360), fill=(0, 0, 0, 110))
img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(14)))

# the disc: a radial gradient, light upper-left to deep green lower-right
cx, cy, r = 192, 182, 158
disc = Image.new("RGBA", (S, S), (0, 0, 0, 0))
px = disc.load()
lo, hi = (18, 104, 52), (110, 214, 128)
for y in range(cy - r, cy + r + 1):
    for x in range(cx - r, cx + r + 1):
        d = math.hypot(x - cx, y - cy)
        if d <= r:
            t = min(1.0, math.hypot(x - (cx - 60), y - (cy - 70)) / (r * 1.55))
            c = tuple(int(hi[i] + (lo[i] - hi[i]) * t) for i in range(3))
            px[x, y] = c + (255,)
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
disc.putalpha(mask)
img.alpha_composite(disc)
ImageDraw.Draw(img).ellipse((cx - r, cy - r, cx + r, cy + r), outline=(10, 70, 34, 255), width=8)

# two beamed quavers in white
note = Image.new("RGBA", (S, S), (0, 0, 0, 0))
nd = ImageDraw.Draw(note)
W = (255, 255, 255, 255)
def head(x, y):
    h = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(h).ellipse((x - 34, y - 24, x + 34, y + 24), fill=W)
    return h.rotate(22, center=(x, y), resample=Image.BICUBIC)
note.alpha_composite(head(150, 252))
note.alpha_composite(head(250, 228))
nd.rectangle((174, 108, 188, 248), fill=W)        # stems
nd.rectangle((274, 84, 288, 224), fill=W)
nd.polygon([(174, 108), (288, 84), (288, 122), (174, 146)], fill=W)   # beam
shadow = note.filter(ImageFilter.GaussianBlur(5))
shadow = Image.merge("RGBA", (*[Image.new("L", (S, S), 0)] * 3, shadow.split()[3].point(lambda a: a * 0.5)))
img.alpha_composite(shadow, (4, 6))
img.alpha_composite(note)

# gloss across the top half, clipped to the disc
gloss = Image.new("RGBA", (S, S), (0, 0, 0, 0))
gp = gloss.load()
for y in range(cy - r + 10, cy):
    a = int(110 * (1 - (y - (cy - r + 10)) / (r - 10)) ** 1.4)
    for x in range(S):
        gp[x, y] = (255, 255, 255, a)
gm = Image.new("L", (S, S), 0)
ImageDraw.Draw(gm).ellipse((cx - r + 26, cy - r + 14, cx + r - 26, cy + 20), fill=255)
gloss.putalpha(Image.composite(gloss.split()[3], Image.new("L", (S, S), 0), gm))
img.alpha_composite(gloss)

img.resize((48, 48), Image.LANCZOS).save("psionnet-spotify.png")
img.resize((128, 128), Image.LANCZOS).save("psionnet-spotify-128.png")
