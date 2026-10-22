#!/usr/bin/env python3
"""Generate the documentation site's brand images.

Writes, into docs/images/:

  hero.png   the home page hero: a VGA-style pixel-art scene
  logo.svg   the header logo, also used as the favicon

The hero is drawn the way 256-colour adventure games were: at 320x180, in
flat colour areas, banded gradients, and hard pixel outlines, then scaled up
four times with no smoothing. It shows an evening desk with a beige PC whose
monitor runs a disk utility, a keyboard, floppy disks, a magnifying glass, a
mug, a desk lamp, and a sunset through the window. The logo is the same
monitor as 16x16 pixel art.

Only the standard library is used (the PNG is written with zlib), and the
output is deterministic, so regenerating without a change to this file
leaves the tracked images as they are. The tool screenshots in
docs/generated/screenshots come from tools/docgen/generate_screenshots.sh.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import struct
import zlib

W, H = 320, 180
SCALE = 4

Colour = tuple[int, int, int]


def rgb(hex_colour: str) -> Colour:
    h = hex_colour.lstrip("#")
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)


def mix(a: Colour, b: Colour, t: float) -> Colour:
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))  # type: ignore[return-value]


def ramp(stops: list[str], steps: int) -> list[Colour]:
    """A banded gradient: `steps` flat colours through the given stops."""
    cs = [rgb(s) for s in stops]
    out = []
    for i in range(steps):
        t = i / max(1, steps - 1) * (len(cs) - 1)
        k = min(int(t), len(cs) - 2)
        out.append(mix(cs[k], cs[k + 1], t - k))
    return out


def hash01(x: int, y: int, salt: int = 0) -> float:
    h = (x * 374761393 + y * 668265263 + salt * 2246822519) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65536.0


class Canvas:
    def __init__(self, w: int, h: int) -> None:
        self.w, self.h = w, h
        self.px: list[list[Colour]] = [[(0, 0, 0)] * w for _ in range(h)]

    def set(self, x: int, y: int, c: Colour) -> None:
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def rect(self, x0: int, y0: int, x1: int, y1: int, c: str | Colour) -> None:
        """Fill x0..x1, y0..y1 inclusive."""
        col = rgb(c) if isinstance(c, str) else c
        for y in range(max(0, y0), min(self.h, y1 + 1)):
            row = self.px[y]
            for x in range(max(0, x0), min(self.w, x1 + 1)):
                row[x] = col

    def vgrad(self, x0: int, y0: int, x1: int, y1: int, bands: list[Colour]) -> None:
        n = len(bands)
        for y in range(y0, y1 + 1):
            k = min(n - 1, (y - y0) * n // (y1 - y0 + 1))
            self.rect(x0, y, x1, y, bands[k])

    def rrect(self, x0: int, y0: int, x1: int, y1: int, r: int, c: str | Colour) -> None:
        col = rgb(c) if isinstance(c, str) else c
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                dx = max(x0 + r - x, 0, x - (x1 - r))
                dy = max(y0 + r - y, 0, y - (y1 - r))
                if dx * dx + dy * dy <= r * r + r:
                    self.set(x, y, col)

    def ellipse(self, cx: float, cy: float, rx: float, ry: float, c: str | Colour) -> None:
        col = rgb(c) if isinstance(c, str) else c
        for y in range(int(cy - ry) - 1, int(cy + ry) + 2):
            for x in range(int(cx - rx) - 1, int(cx + rx) + 2):
                if ((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2 <= 1:
                    self.set(x, y, col)

    def poly(self, pts: list[tuple[float, float]], c: str | Colour) -> None:
        col = rgb(c) if isinstance(c, str) else c
        ys = [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            yc = y + 0.5
            xs = []
            for (ax, ay), (bx, by) in zip(pts, pts[1:] + pts[:1]):
                if (ay <= yc < by) or (by <= yc < ay):
                    xs.append(ax + (yc - ay) * (bx - ax) / (by - ay))
            xs.sort()
            for a, b in zip(xs[::2], xs[1::2]):
                for x in range(int(round(a)), int(round(b))):
                    self.set(x, y, col)

    def light(self, pts: list[tuple[float, float]], tint: str, amount: float) -> None:
        """Brighten whatever is inside the polygon toward `tint`: flat in, flat out."""
        mask = Canvas(self.w, self.h)
        mask.poly(pts, (255, 255, 255))
        t = rgb(tint)
        for y in range(self.h):
            for x in range(self.w):
                if mask.px[y][x] == (255, 255, 255):
                    self.px[y][x] = mix(self.px[y][x], t, amount)

    def glow(self, cx: float, cy: float, rx: float, ry: float, tint: str,
             amounts: list[float]) -> None:
        """Concentric banded halo: the further out, the weaker."""
        t = rgb(tint)
        n = len(amounts)
        for y in range(self.h):
            for x in range(self.w):
                d = math.sqrt(((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2)
                if d < 1:
                    self.px[y][x] = mix(self.px[y][x], t, amounts[min(n - 1, int(d * n))])

    def line(self, x0: int, y0: int, x1: int, y1: int, c: str | Colour) -> None:
        col = rgb(c) if isinstance(c, str) else c
        steps = max(abs(x1 - x0), abs(y1 - y0), 1)
        for i in range(steps + 1):
            self.set(round(x0 + (x1 - x0) * i / steps), round(y0 + (y1 - y0) * i / steps), col)

    def png(self, scale: int) -> bytes:
        raw = bytearray()
        for row in self.px:
            line = bytearray()
            for c in row:
                line += bytes(c) * scale
            for _ in range(scale):
                raw += b"\x00" + line

        def chunk(tag: bytes, data: bytes) -> bytes:
            return (struct.pack(">I", len(data)) + tag + data
                    + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

        ihdr = struct.pack(">IIBBBBB", self.w * scale, self.h * scale, 8, 2, 0, 0, 0)
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
                + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


# 3x5 pixel letters for the few labels in the picture.
FONT3 = {
    "C": ["111", "100", "100", "100", "111"],
    "K": ["101", "101", "110", "101", "101"],
}


def label(cv: Canvas, x: int, y: int, text: str, c: str) -> None:
    for i, ch in enumerate(text):
        for gy, row in enumerate(FONT3[ch]):
            for gx, bit in enumerate(row):
                if bit == "1":
                    cv.set(x + i * 4 + gx, y + gy, rgb(c))


# The beige of every PC of the era, from lit to shadow.
BEIGE = ["#f1e6c8", "#ddd0ac", "#c7b88f", "#a89a74", "#7d7155"]
OUTLINE = "#2a1c2e"


def room(cv: Canvas) -> None:
    cv.vgrad(0, 0, W - 1, 125, ramp(["#1b1236", "#2c1f4d", "#3a2a5a"], 7))
    # the monitor's cool glow and the lamp's warm one, on the wall
    cv.glow(156, 58, 96, 70, "#4a6cd0", [0.42, 0.30, 0.20, 0.11])
    cv.glow(268, 96, 70, 60, "#ffb067", [0.40, 0.28, 0.17, 0.08])


def window(cv: Canvas) -> None:
    x0, y0, x1, y1 = 14, 12, 92, 98
    cv.rect(x0 - 3, y0 - 3, x1 + 3, y1 + 5, "#1a0f0d")
    cv.rect(x0 - 2, y0 - 2, x1 + 2, y1 + 3, "#5a3622")
    cv.rect(x0 - 2, y0 - 2, x1 + 2, y0 - 2, "#7d4e30")
    sky = ramp(["#0c1238", "#23246a", "#5b2f7c", "#b2496f", "#f2875a", "#ffc46b"], 11)
    cv.vgrad(x0, y0, x1, y1, sky)
    for i in range(26):                                   # stars, in the dark top
        sx = x0 + int(hash01(i, 1, 3) * (x1 - x0))
        sy = y0 + int(hash01(i, 2, 3) * 34)
        cv.set(sx, sy, rgb("#fff6d8" if i % 3 else "#9fb4ff"))
    cv.ellipse(70, 28, 7, 7, "#f6edcf")                   # crescent moon
    cv.ellipse(73, 26, 7, 7, sky[2])
    # far hills and a town on the horizon
    cv.poly([(x0, 86), (30, 78), (48, 83), (66, 75), (x1 + 1, 82),
             (x1 + 1, y1 + 1), (x0, y1 + 1)], "#3b1d4a")
    for bx, bw, bh in [(20, 6, 10), (28, 4, 14), (34, 8, 8), (52, 5, 12),
                       (59, 7, 9), (70, 4, 16), (76, 9, 7)]:
        cv.rect(bx, y1 - bh, bx + bw, y1, "#160c22")
        for wy in range(y1 - bh + 2, y1 - 1, 3):
            for wx in range(bx + 1, bx + bw, 2):
                if hash01(wx, wy, 9) < 0.4:
                    cv.set(wx, wy, rgb("#ffd36b"))
    # mullions and sill
    cv.rect(x0, (y0 + y1) // 2 - 1, x1, (y0 + y1) // 2, "#5a3622")
    cv.rect((x0 + x1) // 2 - 1, y0, (x0 + x1) // 2, y1, "#5a3622")
    cv.rect(x0 - 6, y1 + 4, x1 + 6, y1 + 7, "#7d4e30")
    cv.rect(x0 - 6, y1 + 7, x1 + 6, y1 + 8, "#3a2216")
    # a plant on the sill
    for lx, ly in [(26, -8), (29, -11), (32, -7), (23, -5), (35, -4), (28, -5)]:
        cv.ellipse(lx, y1 + ly, 2.5, 4, "#3f8f4a")
        cv.set(lx, y1 + ly - 2, rgb("#7fcf6a"))
    cv.rect(23, y1 - 1, 34, y1 + 3, OUTLINE)
    cv.rect(24, y1 - 1, 33, y1 + 3, "#b4532a")
    cv.rect(24, y1 - 1, 33, y1 - 1, "#d77040")


def desk(cv: Canvas) -> None:
    cv.vgrad(0, 126, W - 1, 167, ramp(["#5a3520", "#8a5631", "#a86b3c"], 6))
    for y in range(129, 166, 5):                           # grain
        for x in range(W):
            if hash01(x // 7, y, 5) < 0.35:
                cv.set(x, y, mix(cv.px[y][x], rgb("#4a2a18"), 0.35))
    cv.rect(0, 168, W - 1, 169, "#d08a50")                 # lit front edge
    cv.vgrad(0, 170, W - 1, H - 1, ramp(["#4a2a18", "#2a160e"], 3))


def monitor_screen(cv: Canvas, x0: int, y0: int, x1: int, y1: int) -> None:
    """A disk utility in text mode, at a few pixels per character."""
    cv.rect(x0, y0, x1, y1, "#0000aa")
    cv.rect(x0, y0, x1, y0 + 3, "#aaaaaa")                # menu bar
    for mx in range(x0 + 3, x1 - 14, 10):
        cv.rect(mx, y0 + 1, mx, y0 + 2, "#aa0000")
        cv.rect(mx + 1, y0 + 1, mx + 5, y0 + 2, "#222222")
    # disk map window
    wx0, wy0, wx1, wy1 = x0 + 4, y0 + 7, x0 + 42, y1 - 8
    cv.rect(wx0 + 2, wy0 + 2, wx1 + 2, wy1 + 2, "#000044")
    cv.rect(wx0, wy0, wx1, wy1, "#0000aa")
    for g in (0, 2):
        cv.rect(wx0 + g, wy0 + g, wx1 - g, wy0 + g, "#ffffff")
        cv.rect(wx0 + g, wy1 - g, wx1 - g, wy1 - g, "#ffffff")
        cv.rect(wx0 + g, wy0 + g, wx0 + g, wy1 - g, "#ffffff")
        cv.rect(wx1 - g, wy0 + g, wx1 - g, wy1 - g, "#ffffff")
    cv.rect(wx0 + 12, wy0, wx0 + 26, wy0, "#aaaaaa")
    n = 0
    for gy in range(wy0 + 4, wy1 - 6, 3):
        for gx in range(wx0 + 4, wx1 - 4, 3):
            if n < 40:
                colour = "#55ffff"
            elif n == 40:
                colour = "#ffff55"
            else:
                colour = "#ffffff" if hash01(gx, gy, 11) < 0.35 else "#3333bb"
            cv.rect(gx, gy, gx + 1, gy + 1, colour)
            n += 1
    cv.rect(wx0 + 4, wy1 - 4, wx1 - 4, wy1 - 4, "#3333bb")  # progress
    cv.rect(wx0 + 4, wy1 - 4, wx0 + 4 + (wx1 - wx0 - 8) * 6 // 10, wy1 - 4, "#55ffff")
    # info window with a speed chart
    ix0, iy0, ix1, iy1 = x0 + 47, y0 + 7, x1 - 4, y0 + 26
    cv.rect(ix0 + 2, iy0 + 2, ix1 + 2, iy1 + 2, "#000044")
    cv.rect(ix0, iy0, ix1, iy1, "#aaaaaa")
    cv.rect(ix0, iy0, ix1, iy0 + 1, "#0000aa")
    for i, (length, colour) in enumerate([(4, "#555555"), (8, "#555555"), (15, "#aa0000")]):
        cv.rect(ix0 + 3, iy0 + 5 + i * 4, ix0 + 3 + length, iy0 + 6 + i * 4, colour)
    # directory tree window
    tx0, ty0, tx1, ty1 = x0 + 47, y0 + 30, x1 - 4, y1 - 6
    cv.rect(tx0 + 2, ty0 + 2, tx1 + 2, ty1 + 2, "#000044")
    cv.rect(tx0, ty0, tx1, ty1, "#00aaaa")
    cv.rect(tx0, ty0, tx1, ty0, "#ffffff")
    for i, depth in enumerate([0, 1, 1, 2, 1]):
        ly = ty0 + 3 + i * 3
        if depth:
            cv.rect(tx0 + 2 + depth * 3, ly, tx0 + 3 + depth * 3, ly, "#000000")
        cv.rect(tx0 + 5 + depth * 3, ly, tx0 + 10 + depth * 3, ly,
                "#ffffff" if i == 3 else "#000000")
    # function-key bar
    cv.rect(x0, y1 - 2, x1, y1, "#000000")
    for fx in range(x0 + 1, x1 - 4, 7):
        cv.set(fx, y1 - 1, rgb("#ffffff"))
        cv.rect(fx + 1, y1 - 1, fx + 5, y1 - 1, "#00aaaa")
    # glass: a curved glare and darker edges
    for i in range(12):
        gx, gy = x0 + 2 + i, y0 + 5 + i // 2
        cv.set(gx, gy, mix(cv.px[gy][gx], (255, 255, 255), 0.35))
    for y in range(y0, y1 + 1):
        for x in (x0, x1):
            cv.set(x, y, mix(cv.px[y][x], (0, 0, 0), 0.4))


def computer(cv: Canvas) -> None:
    b = [rgb(c) for c in BEIGE]
    # system unit
    cv.rrect(97, 103, 215, 128, 2, OUTLINE)
    cv.rect(98, 104, 214, 127, b[1])
    cv.rect(98, 104, 214, 105, b[0])
    cv.rect(98, 124, 214, 127, b[3])
    cv.rect(212, 106, 214, 123, b[2])
    cv.rect(106, 110, 150, 116, b[2])                     # 5.25" drive
    cv.rect(108, 112, 148, 113, "#1c1418")
    cv.rect(126, 114, 130, 115, b[4])
    cv.rect(158, 110, 186, 115, b[2])                     # 3.5" drive
    cv.rect(160, 112, 178, 112, "#1c1418")
    cv.rect(181, 112, 183, 113, b[4])
    for vx in range(106, 150, 3):                         # vents
        cv.rect(vx, 119, vx + 1, 121, b[3])
    cv.rect(196, 118, 198, 119, "#3dff6a")                # power
    cv.rect(201, 118, 203, 119, "#ffb000")                # turbo
    cv.rect(195, 110, 205, 115, b[3])
    cv.rect(196, 111, 204, 114, b[0])
    label(cv, 160, 118, "CK", "#2b3a8a")
    # monitor stand
    cv.rect(140, 96, 172, 103, OUTLINE)
    cv.rect(141, 97, 171, 102, b[2])
    cv.rect(146, 92, 166, 97, b[3])
    # monitor
    cv.rrect(108, 16, 204, 96, 5, OUTLINE)
    cv.rrect(109, 17, 203, 95, 4, b[1])
    cv.rect(112, 17, 200, 18, b[0])
    cv.rect(199, 22, 203, 92, b[2])
    cv.rect(112, 92, 200, 95, b[3])
    cv.rrect(115, 22, 195, 85, 3, b[3])                   # bezel
    cv.rrect(117, 24, 193, 83, 3, "#151020")
    monitor_screen(cv, 120, 27, 190, 80)
    label(cv, 118, 88, "CK", "#6b5f45")
    cv.rect(188, 88, 190, 89, "#3dff6a")
    cv.rect(180, 88, 184, 89, b[3])


def keyboard(cv: Canvas) -> None:
    b = [rgb(c) for c in BEIGE]
    cv.poly([(98, 140), (212, 140), (220, 158), (90, 158)], OUTLINE)
    cv.poly([(99, 141), (211, 141), (218, 157), (92, 157)], b[2])
    cv.poly([(99, 141), (211, 141), (212, 143), (98, 143)], b[1])
    for row in range(4):
        y = 144 + row * 3
        x = 106 - row * 3
        while x < 203 + row * 3:
            wide = 12 if (row == 3 and 140 <= x < 146) else 0
            cv.rect(x, y, x + 3 + wide, y, b[0])
            cv.rect(x, y + 1, x + 3 + wide, y + 1, b[1])
            x += 5 + wide
    cv.rect(92, 157, 218, 159, b[4])


def floppies(cv: Canvas) -> None:
    for i, body in enumerate(["#1d1d24", "#a3242f", "#2a4fb0"]):
        x0, y0 = 232 - i * 2, 146 - i * 4
        cv.rect(x0 - 1, y0 - 1, x0 + 31, y0 + 17, OUTLINE)
        cv.rect(x0, y0, x0 + 30, y0 + 16, body)
        cv.rect(x0, y0 + 14, x0 + 30, y0 + 16, mix(rgb(body), (0, 0, 0), 0.35))
    x0, y0 = 228, 138                                      # the top disk's face
    cv.rect(x0 + 8, y0, x0 + 22, y0 + 5, "#c9ced8")
    cv.rect(x0 + 16, y0 + 1, x0 + 19, y0 + 4, "#6c7280")
    cv.rect(x0 + 4, y0 + 7, x0 + 26, y0 + 13, "#fff7d6")
    for ly in (y0 + 9, y0 + 11):
        cv.rect(x0 + 6, ly, x0 + 22, ly, "#5a6fd0")
    cv.set(x0 + 1, y0 + 1, rgb("#6c7280"))


def magnifier(cv: Canvas) -> None:
    cv.poly([(266, 163), (270, 159), (290, 170), (287, 174)], OUTLINE)
    cv.poly([(267, 163), (270, 160), (289, 170), (287, 173)], "#6b3a1e")
    cv.line(270, 161, 288, 171, "#9a5a2e")
    cv.ellipse(256, 156, 13, 9, OUTLINE)
    cv.ellipse(256, 156, 12, 8, "#d9a640")
    cv.ellipse(256, 156, 9.5, 6, mix(rgb("#8fd8e8"), rgb("#a86b3c"), 0.35))
    cv.rect(251, 152, 254, 152, "#ffffff")
    cv.rect(250, 153, 251, 153, "#ffffff")
    cv.rect(247, 151, 250, 151, "#f2c85a")


def mug(cv: Canvas) -> None:
    cv.rect(53, 136, 70, 158, OUTLINE)
    cv.rect(54, 137, 69, 157, "#b8323a")
    cv.rect(54, 137, 57, 157, "#d9545a")
    cv.rect(66, 137, 69, 157, "#8a2029")
    cv.ellipse(61.5, 137, 8, 2, "#3a1a10")
    cv.rect(71, 141, 75, 152, OUTLINE)
    cv.rect(72, 143, 73, 150, "#b8323a")
    label(cv, 58, 145, "CK", "#f4e2c0")
    for i, (sx, sy) in enumerate([(59, 131), (60, 128), (59, 125), (63, 130), (64, 127), (63, 124)]):
        cv.set(sx, sy, mix(rgb("#cfc6e6"), rgb("#3a2a5a"), 0.2 + 0.15 * (i % 3)))


def lamp(cv: Canvas) -> None:
    # light cone first, so the lamp sits on top of its own light
    cv.light([(262, 58), (282, 58), (318, 168), (226, 168)], "#ffd890", 0.16)
    cv.light([(266, 60), (278, 60), (304, 168), (238, 168)], "#ffe2a8", 0.12)
    cv.ellipse(296, 128, 13, 4, OUTLINE)
    cv.ellipse(296, 127, 12, 3, "#2f6a4e")
    cv.rect(286, 127, 306, 128, "#1d4a35")
    cv.line(296, 126, 302, 88, OUTLINE)
    cv.line(297, 126, 303, 88, "#9aa3ad")
    cv.line(302, 88, 280, 58, OUTLINE)
    cv.line(303, 88, 281, 58, "#9aa3ad")
    cv.ellipse(302, 88, 2, 2, "#5a6470")
    cv.poly([(262, 56), (278, 44), (290, 52), (282, 62)], OUTLINE)
    cv.poly([(264, 56), (278, 46), (288, 52), (281, 60)], "#2f6a4e")
    cv.line(266, 55, 279, 47, "#5aa77a")
    cv.ellipse(272, 59, 6, 2.5, "#fff2b8")


def hero() -> Canvas:
    cv = Canvas(W, H)
    room(cv)
    window(cv)
    desk(cv)
    lamp(cv)
    computer(cv)
    keyboard(cv)
    floppies(cv)
    magnifier(cv)
    mug(cv)
    return cv


def logo() -> str:
    """The hero's monitor as 16x16 pixel art, one SVG rect per run of a colour."""
    art = [
        "................",
        ".oooooooooooooo.",
        ".obbbbbbbbbbbbs.",
        ".obkkkkkkkkkkbs.",
        ".obkBBBBBBBBkbs.",
        ".obkBcBBBBBBkbs.",
        ".obkBBcBBBBBkbs.",
        ".obkBcBByyBBkbs.",
        ".obkBBBBBBBBkbs.",
        ".obkkkkkkkkkkbs.",
        ".obbbbbbbbbgbbs.",
        ".ossssssssssss..",
        "......oddo......",
        "....odddddddo...",
        "....oooooooooo..",
        "................",
    ]
    colours = {"o": OUTLINE, "b": BEIGE[1], "s": BEIGE[3], "d": BEIGE[2], "k": "#151020",
               "B": "#0000aa", "c": "#55ffff", "y": "#ffff55", "g": "#3dff6a"}
    rects = []
    for y, row in enumerate(art):
        x = 0
        while x < 16:
            ch = row[x]
            start = x
            while x < 16 and row[x] == ch:
                x += 1
            if ch != ".":
                rects.append(f'<rect x="{start * 4}" y="{y * 4}" width="{(x - start) * 4}" '
                             f'height="4" fill="{colours[ch]}"/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64" '
            'shape-rendering="crispEdges" role="img" aria-label="CK Utilities">\n'
            + "\n".join(rects) + "\n</svg>\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    default_out = pathlib.Path(__file__).resolve().parents[2] / "docs" / "images"
    parser.add_argument("--output", type=pathlib.Path, default=default_out)
    out = parser.parse_args().output
    out.mkdir(parents=True, exist_ok=True)
    (out / "hero.png").write_bytes(hero().png(SCALE))
    (out / "logo.svg").write_text(logo(), encoding="utf-8")
    print(f"wrote {out / 'hero.png'}")
    print(f"wrote {out / 'logo.svg'}")


if __name__ == "__main__":
    main()
