"""The app icon's master, docs/branding/app-icon.png (1024 x 1024, no alpha), drawn from rectangles: a dark field,
a big pixel "35" and a small pixel coin. Original art: no game artwork, logos or fonts. Then tools/make_icons.sh."""
import os
import struct
import zlib

N = 1024
img = bytearray(N * N * 3)


def rect(x0, y0, x1, y1, col):
    x0, y0, x1, y1 = max(0, x0), max(0, y0), min(N, x1), min(N, y1)
    row = bytes(col) * (x1 - x0)
    for y in range(y0, y1):
        o = (y * N + x0) * 3
        img[o:o + len(row)] = row


# background: a vertical gradient, deep blue to near black
for y in range(N):
    t = y / (N - 1)
    c = (int(20 + 8 * (1 - t)), int(26 + 22 * (1 - t)), int(58 + 40 * (1 - t)))
    rect(0, y, N, y + 1, c)

# a lighter inset panel with stepped (pixel) corners
P, STEP = 72, 24
panel = (34, 52, 104)
rect(P + 2 * STEP, P, N - P - 2 * STEP, N - P, panel)
rect(P + STEP, P + STEP, N - P - STEP, N - P - STEP, panel)
rect(P, P + 2 * STEP, N - P, N - P - 2 * STEP, panel)

# "35" in a 5 x 7 pixel font of our own
GLYPHS = {
    '3': ["#####", "....#", "...#.", "..##.", "....#", "#...#", ".###."],
    '5': ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
}
cell = 64
gw, gap = 5 * cell, cell
x = (N - (2 * gw + gap)) // 2
y = 190
shadow, face, hi = (12, 14, 30), (250, 250, 245), (255, 214, 64)
for ch in "35":
    for r, line in enumerate(GLYPHS[ch]):
        for c, on in enumerate(line):
            if on == '#':
                px, py = x + c * cell, y + r * cell
                rect(px + 16, py + 16, px + cell + 16, py + cell + 16, shadow)
    x += gw + gap
x = (N - (2 * gw + gap)) // 2
for ch in "35":
    for r, line in enumerate(GLYPHS[ch]):
        for c, on in enumerate(line):
            if on == '#':
                px, py = x + c * cell, y + r * cell
                rect(px, py, px + cell, py + cell, face)
    x += gw + gap

# a small pixel coin, bottom right of the digits
COIN = [
    "..####..",
    ".#oooo#.",
    "#oowwoo#",
    "#oowooo#",
    "#oowooo#",
    "#oowooo#",
    "#oooooo#",
    ".#oooo#.",
    "..####..",
]
pal = {'#': (120, 70, 10), 'o': hi, 'w': (255, 248, 200)}
cs = 22
cx, cy = (N - 8 * cs) // 2, 190 + 7 * cell + 70
for r, line in enumerate(COIN):
    for c, k in enumerate(line):
        if k in pal:
            rect(cx + c * cs, cy + r * cs, cx + (c + 1) * cs, cy + (r + 1) * cs, pal[k])


def png(path):
    raw = b''.join(b'\0' + bytes(img[y * N * 3:(y + 1) * N * 3]) for y in range(N))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', N, N, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(data)


png(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'app-icon.png'))
