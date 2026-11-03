"""Draws the README's two animations from scratch: the race and the timeline.

    python tools/Showcase.py docs/media          # race.webp and timeline.webp, as the README shows them
    python tools/Showcase.py out --format mp4    # 60 fps mp4 previews

Needs Python 3 with Pillow and NumPy, ffmpeg (with libwebp) on PATH, and Windows' Segoe UI and Bahnschrift
fonts. Everything is drawn at twice the size and box-filtered down, which antialiases the shapes.
"""
import argparse
import functools
import math
import os
import random
import subprocess

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

# The README's measurement, in ms: (menu visible, fully drawn) with this many windows open.
WINDOWS = 125
STOCK = (279, 334)
INSTANT = (44, 58)

W, H, S = 1920, 1080, 2  # frame size in 1080p units, drawn S times larger
DURATION = 8.0
FONTS = os.path.join(os.environ.get('WINDIR', 'C:\\Windows'), 'Fonts') + os.sep
BOLD, SEMI, REG = 'segoeuib.ttf', 'seguisb.ttf', 'segoeui.ttf'
NUM = 'bahnschrift.ttf'

ACC = (45, 212, 238)         # Instant Alt+Tab, and a menu on screen
AMB = (251, 183, 36)         # waiting for the menu
DONE_GREY = (128, 138, 166)  # a menu that finished, but slowly: done, not a winner
TXT = (241, 245, 249)
MUT = (140, 151, 178)
APP = [(59, 130, 246), (34, 160, 107), (139, 92, 246), (249, 115, 22), (236, 72, 153),
       (20, 184, 166), (239, 68, 68), (234, 179, 8), (99, 102, 241)]


# ---------------------------------------------------------------- easing and drawing helpers

def clamp(x, a=0.0, b=1.0):
    return a if x < a else b if x > b else x


def prog(t, a, b):
    """0 before a, 1 after b, linear in between."""
    return clamp((t - a) / (b - a)) if b > a else (1.0 if t >= a else 0.0)


def e_out(x):
    return 1 - (1 - x) ** 3


def e_inout(x):
    return 4 * x ** 3 if x < 0.5 else 1 - (-2 * x + 2) ** 3 / 2


def e_back(x, s=1.70158):  # overshoots a little: for things that pop in
    return 1 + (s + 1) * (x - 1) ** 3 + s * (x - 1) ** 2


def bump(x):  # 0 -> 1 -> 0
    return math.sin(math.pi * clamp(x))


def mix(c1, c2, t):
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(c1, c2))


@functools.lru_cache(None)
def font(name, px, var=None):
    f = ImageFont.truetype(FONTS + name, int(round(px * S)))
    if var:
        f.set_variation_by_name(var)
    return f


def rrect(d, x, y, w, h, r, fill=None, outline=None, width=0):
    d.rounded_rectangle([x * S, y * S, (x + w) * S - 1, (y + h) * S - 1], radius=r * S, fill=fill,
                        outline=outline, width=int(round(width * S)))


def paste(im, spr, X, Y):
    """alpha_composite, clipped to the image (it refuses negative positions)."""
    sx, sy = max(0, -X), max(0, -Y)
    ex, ey = min(spr.width, im.width - X), min(spr.height, im.height - Y)
    if ex > sx and ey > sy:
        im.alpha_composite(spr, dest=(X + sx, Y + sy), source=(sx, sy, ex, ey))


def fade(spr, opacity):
    if opacity >= 0.998:
        return spr
    out = spr.copy()
    out.putalpha(spr.getchannel('A').point(lambda v, o=opacity: int(v * o + 0.5)))
    return out


def blit(im, spr, x, y, anchor='mm', scale=1.0, opacity=1.0):
    """Composites a sprite with its anchor point at (x, y) in 1080p units. anchor: l/m/r then t/m/b."""
    if opacity <= 0.003 or scale <= 0.01:
        return
    if abs(scale - 1) > 1e-3:
        spr = spr.resize((max(1, round(spr.width * scale)), max(1, round(spr.height * scale))), Image.BICUBIC)
    spr = fade(spr, opacity)
    X = x * S - {'l': 0, 'm': spr.width / 2, 'r': spr.width}[anchor[0]]
    Y = y * S - {'t': 0, 'm': spr.height / 2, 'b': spr.height}[anchor[1]]
    paste(im, spr, int(round(X)), int(round(Y)))


@functools.lru_cache(4096)
def text_sprite(s, fname, px, color, var=None):
    """The sprite's height is the font's line box, not the text's, so lines of text align."""
    f = font(fname, px, var)
    l, _, r, _ = f.getbbox(s)
    _, t, _, b = f.getbbox('Hgjy0')
    pad = 6 * S
    im = Image.new('RGBA', (r - l + 2 * pad, b - t + 2 * pad), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((pad - l, pad - t), s, font=f, fill=color)
    return im


def text(im, x, y, s, fname, px, color, anchor='mm', scale=1.0, opacity=1.0, var=None):
    blit(im, text_sprite(s, fname, px, color, var), x, y, anchor, scale, opacity)


@functools.lru_cache(64)
def shadow_sprite(w, h, r, blur, alpha):
    b = int(blur * S * 2)
    m = Image.new('L', (int(w * S) + 2 * b, int(h * S) + 2 * b), 0)
    ImageDraw.Draw(m).rounded_rectangle([b, b, b + w * S, b + h * S], radius=r * S, fill=int(255 * alpha))
    out = Image.new('RGBA', m.size, (0, 0, 0, 0))
    out.putalpha(m.filter(ImageFilter.GaussianBlur(blur * S)))
    return out


@functools.lru_cache(64)
def glow_sprite(w, h, r, blur, color, alpha):
    out = Image.new('RGBA', shadow_sprite(w, h, r, blur, alpha).size, color + (0,))
    out.putalpha(shadow_sprite(w, h, r, blur, alpha).getchannel('A'))
    return out


@functools.lru_cache(1)
def background():
    y = np.linspace(0, 1, H, dtype=np.float32)[:, None, None]
    x = np.linspace(0, 1, W, dtype=np.float32)[None, :, None]
    top, bot = np.array([9, 13, 26], np.float32), np.array([15, 20, 40], np.float32)
    img = top + (bot - top) * y
    img = img + np.exp(-(((x - 0.5) / 0.42) ** 2 + ((y - 0.08) / 0.38) ** 2)) * np.array([22, 44, 84], np.float32) * .55
    img = img * (1 - 0.38 * (((x - 0.5) * 1.1) ** 2 + ((y - 0.5) * 1.3) ** 2))
    img = img + np.random.default_rng(1).uniform(-1.2, 1.2, (H, W, 1)).astype(np.float32)  # against banding
    small = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')
    return small.resize((W * S, H * S), Image.NEAREST).convert('RGBA')


# ---------------------------------------------------------------- windows, the Alt+Tab menu, desktops

@functools.lru_cache(1024)
def thumb_sprite(w, h, seed):
    """A made-up window: browser, editor, spreadsheet, chat, photo, document, terminal or files."""
    rnd = random.Random(seed * 7919 + 13)
    W2, H2 = int(w * S), int(h * S)
    c = Image.new('RGB', (W2, H2))
    d = ImageDraw.Draw(c)
    kind = seed % 8
    acc = APP[(seed * 5) % len(APP)]
    rad = max(1.0, min(4.0, h * 0.05))

    def r(x0, y0, x1, y1, fill, rr=0.0):  # in fractions of the window
        d.rounded_rectangle([x0 * W2, y0 * H2, x1 * W2, y1 * H2], radius=rr * S, fill=fill)

    if kind == 0:  # browser
        r(0, 0, 1, 1, (244, 246, 250)); r(0, 0, 1, .13, (225, 230, 240))
        r(.04, .03, .62, .10, (255, 255, 255), rad)
        r(.06, .21, .94, .55, acc, rad)
        for i in range(3):
            r(.06, .63 + i * .11, .06 + rnd.uniform(.4, .85), .68 + i * .11, (198, 205, 218), rad)
    elif kind == 1:  # code editor
        r(0, 0, 1, 1, (27, 31, 42)); r(0, 0, .16, 1, (22, 25, 34))
        cols = [(197, 134, 255), (86, 204, 242), (255, 167, 87), (125, 220, 140), (220, 224, 235)]
        for i in range(9):
            x0 = .2 + rnd.choice([0, 0, .05, .1])
            r(x0, .07 + i * .1, x0 + rnd.uniform(.15, .6), .11 + i * .1, rnd.choice(cols), rad)
    elif kind == 2:  # spreadsheet
        r(0, 0, 1, 1, (255, 255, 255)); r(0, 0, 1, .16, (33, 163, 102))
        for i in range(1, 8):
            d.line([0, (.16 + i * .12) * H2, W2, (.16 + i * .12) * H2], fill=(220, 226, 232), width=S)
        for j in range(1, 6):
            d.line([j * W2 / 6, .16 * H2, j * W2 / 6, H2], fill=(220, 226, 232), width=S)
        r(.17, .3, .5, .4, (190, 235, 210))
    elif kind == 3:  # chat
        r(0, 0, 1, 1, (246, 246, 251)); r(0, 0, .28, 1, (233, 230, 247))
        for i in range(4):
            r(.04, .08 + i * .2, .24, .18 + i * .2, (210, 205, 235), rad)
        for i in range(4):
            wdt = rnd.uniform(.25, .45)
            x0 = .94 - wdt if i % 2 else .34
            r(x0, .12 + i * .2, x0 + wdt, .24 + i * .2, acc if i % 2 else (222, 224, 232), rad * 1.5)
    elif kind == 4:  # photo
        a, b = APP[(seed + 2) % len(APP)], APP[(seed + 5) % len(APP)]
        for i in range(H2):
            d.line([0, i, W2, i], fill=mix(mix(a, (255, 255, 255), .25), b, i / H2))
        d.ellipse([.62 * W2, .15 * H2, .8 * W2, .15 * H2 + .18 * W2], fill=(255, 236, 170))
        d.polygon([(0, H2), (.3 * W2, .45 * H2), (.6 * W2, H2)], fill=mix(b, (0, 0, 0), .35))
        d.polygon([(.35 * W2, H2), (.7 * W2, .55 * H2), (W2, .9 * H2), (W2, H2)], fill=mix(b, (0, 0, 0), .5))
    elif kind == 5:  # document
        r(0, 0, 1, 1, (250, 251, 253)); r(0, 0, 1, .15, (43, 108, 240))
        r(.2, .22, .8, 1, (255, 255, 255))
        for i in range(7):
            r(.26, .3 + i * .09, .26 + rnd.uniform(.3, .48), .34 + i * .09, (190, 198, 214), rad)
    elif kind == 6:  # terminal
        r(0, 0, 1, 1, (14, 16, 22))
        for i in range(8):
            r(.05, .08 + i * .11, .05 + rnd.uniform(.2, .75), .12 + i * .11,
              (120, 230, 150) if i % 3 == 0 else (200, 205, 215), rad)
    else:  # files
        r(0, 0, 1, 1, (243, 245, 249)); r(0, 0, .22, 1, (229, 233, 241))
        for i in range(2):
            for j in range(4):
                r(.28 + j * .18, .14 + i * .42, .4 + j * .18, .38 + i * .42, (250, 199, 72), rad)
    m = Image.new('L', (W2, H2), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, W2 - 1, H2 - 1], radius=max(2, min(7, h * 0.07)) * S, fill=255)
    out = c.convert('RGBA')
    out.putalpha(m)
    return out


@functools.lru_cache(1024)
def card_sprite(w, th, hh, seed, placeholder=False):
    """One card of the menu: icon and title above the window's thumbnail (a grey block until it is drawn)."""
    rnd = random.Random(seed)
    im = Image.new('RGBA', (int(w * S), int((hh + th) * S)), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    ic = hh * 0.56
    rrect(d, 1, (hh - ic) / 2 - hh * 0.08, ic, ic, ic * 0.24, fill=APP[(seed * 5) % len(APP)])
    rrect(d, ic + hh * 0.4, hh / 2 - hh * 0.2, w * rnd.uniform(.32, .6), hh * 0.22, hh * 0.11, fill=(206, 213, 228))
    if placeholder:
        th_im = Image.new('RGBA', (int(w * S), int(th * S)), (0, 0, 0, 0))
        rrect(ImageDraw.Draw(th_im), 0, 0, w, th, max(2, min(7, th * 0.07)), fill=(42, 48, 68))
    else:
        th_im = thumb_sprite(w, th, seed)
    im.alpha_composite(th_im, (0, int(round(hh * S))))
    return im


class Menu:
    """The Alt+Tab menu: a panel of cards, the second one selected, as Alt+Tab opens it."""

    def __init__(self, cols, rows, cw, th, hh, gap, pad, seed0=0):
        self.cw, self.th, self.hh, self.gap = cw, th, hh, gap
        self.ch = hh + th
        self.w = pad * 2 + cols * cw + (cols - 1) * gap
        self.h = pad * 2 + rows * self.ch + (rows - 1) * gap
        self.cards = [(pad + c * (cw + gap), pad + r * (self.ch + gap)) for r in range(rows) for c in range(cols)]
        self.seeds = [seed0 + i * 3 + 1 for i in range(len(self.cards))]
        self.r = max(6, pad * 0.5)

    def render(self, drawn=None, sel=1):
        """drawn[i]: 0..1, how far card i's thumbnail has been drawn (all of them if None)."""
        im = panel_sprite(self.w, self.h, self.r).copy()
        for i, (x, y) in enumerate(self.cards):
            a = 1.0
            if drawn is not None and drawn[i] < 1:
                paste(im, card_sprite(self.cw, self.th, self.hh, self.seeds[i], True), int(x * S), int(y * S))
                a = drawn[i]
            if a > 0.003:
                paste(im, fade(card_sprite(self.cw, self.th, self.hh, self.seeds[i]), a), int(x * S), int(y * S))
        x, y = self.cards[sel]
        m = max(4, self.gap * 0.3)
        lay = Image.new('RGBA', im.size, (0, 0, 0, 0))
        rrect(ImageDraw.Draw(lay), x - m, y - m, self.cw + 2 * m, self.ch + 2 * m, max(5, self.r * 0.7),
              outline=(255, 255, 255), width=max(2, self.cw * 0.016))
        im.alpha_composite(lay)
        return im

    @functools.lru_cache(4)
    def full(self):
        return self.render()


@functools.lru_cache(4)
def panel_sprite(w, h, r):
    im = Image.new('RGBA', (int(w * S), int(h * S)), (0, 0, 0, 0))
    rrect(ImageDraw.Draw(im), 0, 0, w, h, r, fill=(30, 35, 52, 246), outline=(58, 67, 94), width=1.2)
    return im


MENU = Menu(4, 2, 184, 104, 32, 18, 24, seed0=100)
SCREEN_W, SCREEN_H = 880, 495


def taskbar(h):
    return round(h * 0.072)


@functools.lru_cache(2)
def desktop():
    """A desktop with three windows and a taskbar, without the menu."""
    w, h = SCREEN_W, SCREEN_H
    W2, H2 = int(w * S), int(h * S)
    y = np.linspace(0, 1, H2 // 4, dtype=np.float32)[:, None, None]
    x = np.linspace(0, 1, W2 // 4, dtype=np.float32)[None, :, None]
    a, b = np.array([22, 40, 92], np.float32), np.array([62, 38, 110], np.float32)
    img = a + (b - a) * ((x + y) / 2)
    img = img + np.exp(-(((x - .25) / .3) ** 2 + ((y - .3) / .35) ** 2)) * np.array([30, 90, 160], np.float32) * .6
    img = img + np.exp(-(((x - .8) / .3) ** 2 + ((y - .8) / .3) ** 2)) * np.array([120, 50, 120], np.float32) * .45
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB').resize((W2, H2), Image.BICUBIC).convert('RGBA')
    for i, (x0, y0, ww, hh) in enumerate([(.06, .08, .42, .55), (.38, .16, .46, .58), (.6, .05, .34, .42)]):
        blit(im, shadow_sprite(int(ww * w), int(hh * h), 6, 10, .5), (x0 + ww / 2) * w, (y0 + hh / 2) * h + 6, 'mm')
        paste(im, fade(thumb_sprite(int(ww * w), int(hh * h), 40 + i * 3), .92), int(x0 * w * S), int(y0 * h * S))
    tb = taskbar(h)
    lay = Image.new('RGBA', im.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(lay)
    rrect(ld, 0, h - tb, w, tb, 0, fill=(16, 20, 34, 225))
    ic, step = tb * 0.52, tb * 0.88
    for k in range(6):
        rrect(ld, w / 2 + (k - 3) * step + (step - ic) / 2, h - tb + (tb - ic) / 2, ic, ic, ic * 0.22,
              fill=APP[(k * 5) % len(APP)])
    im.alpha_composite(lay)
    m = Image.new('L', im.size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, im.width - 1, im.height - 1], radius=16 * S, fill=255)
    im.putalpha(m)
    return im


def screen(appear, fill):
    """The desktop with the Alt+Tab menu: appear 0..1 shows the menu, fill 0..1 draws its thumbnails."""
    im = desktop().copy()
    if appear > 0.003:
        n = len(MENU.cards)
        spr = MENU.full() if fill >= 1 else MENU.render(drawn=[clamp(fill * n - i) for i in range(n)])
        scale, op = 0.97 + 0.03 * e_out(appear), e_out(appear)
        cx, cy = SCREEN_W / 2, (SCREEN_H - taskbar(SCREEN_H)) / 2
        blit(im, shadow_sprite(int(MENU.w), int(MENU.h), MENU.r, 22, 0.55), cx, cy + 14 * scale, 'mm', scale, op)
        blit(im, spr, cx, cy, 'mm', scale, op)
    return im


# ---------------------------------------------------------------- the Alt + Tab keys

KEYS_W, KEYS_H = 150 + 64 + 170, 82  # Alt, the gap with the "+", Tab


def key_amount(t, intervals):
    """How far the keys are down (0..1): pressed during each (down, up) interval, springing back after."""
    v = 0.0
    for a, b in intervals:
        if a <= t < b:
            v = max(v, e_out(prog(t, a, a + 0.07)))
        elif b <= t < b + 0.2:
            v = max(v, 1 - e_out(prog(t, b, b + 0.16)))
    return v


def draw_key(im, cx, cy, w, h, label, press, opacity, k):
    """A pale keycap, which reads as a key at a glance on the dark background; cyan and glowing when pressed."""
    w, h = w * k, h * k
    lay = Image.new('RGBA', (int((w + 80 * k) * S), int((h + 80 * k) * S)), (0, 0, 0, 0))
    ox, oy = 40 * k, 40 * k
    d = ImageDraw.Draw(lay)
    depth = 12 * k
    off = depth * press * 0.85
    if press > 0.01:
        blit(lay, glow_sprite(int(w), int(h), 16 * k, 16 * k, ACC, 0.9), ox + w / 2, oy + h / 2 + off, 'mm', 1.08,
             press)
    rrect(d, ox, oy + depth, w, h, 16 * k, fill=mix((118, 128, 156), (20, 120, 150), press))
    rrect(d, ox, oy + off, w, h, 16 * k, fill=mix((234, 238, 247), (70, 216, 242), press),
          outline=mix((255, 255, 255), (190, 248, 255), press), width=1.5 * k)
    text(lay, ox + w / 2, oy + h / 2 + off - k, label, BOLD, round(34 * k), mix((28, 34, 52), (4, 30, 42), press))
    blit(im, lay, cx, cy, 'mm', 1, opacity)


def draw_alt_tab(im, cx, cy, press, opacity, k):
    left = cx - KEYS_W * k / 2  # the whole group is centred on cx, not the "+"
    draw_key(im, left + 75 * k, cy, 150, 82, 'Alt', press, opacity, k)
    text(im, left + (150 + 32) * k, cy + 4 * k, '+', SEMI, round(44 * k), TXT, opacity=opacity)
    draw_key(im, left + (150 + 64 + 85) * k, cy, 170, 82, 'Tab', press, opacity, k)


def ripple(im, cx, cy, w, h, age, opacity=1.0):
    """Two rings spreading out from the keys, `age` seconds after they go down."""
    for delay in (0.0, 0.14):
        a = age - delay
        if not 0 <= a < 0.65:
            continue
        q = e_out(a / 0.65)
        grow = 10 + 48 * q
        ww, hh = w + 2 * grow, h + 2 * grow
        lay = Image.new('RGBA', (int((ww + 12) * S), int((hh + 12) * S)), (0, 0, 0, 0))
        rrect(ImageDraw.Draw(lay), 6, 6, ww, hh, 22 + grow * 0.6, outline=ACC + (int(210 * (1 - q) * opacity),),
              width=4 - 2 * q)
        blit(im, lay, cx, cy, 'mm')


def outline(im, cx, cy, w, h, color, width, scale=1.0, opacity=1.0):
    lay = Image.new('RGBA', (int((w + 24) * S), int((h + 24) * S)), (0, 0, 0, 0))
    rrect(ImageDraw.Draw(lay), 12, 12, w, h, 16, outline=color, width=width)
    blit(im, lay, cx, cy, 'mm', scale, opacity)


# ---------------------------------------------------------------- the race: both desktops, one Alt+Tab

def scene_race(t):
    """One Alt+Tab on both desktops at once, 10x slower. The keys stay down, so both results stay up."""
    im = background().copy()
    vis = e_out(prog(t, 0, .5)) * (1 - e_inout(prog(t, 7.3, 7.95)))  # fades in and out, so it loops
    T0 = 0.8
    ms = max(0.0, (t - T0) * 100)
    sw, sh, gap, top = SCREEN_W, SCREEN_H, 40, 196
    kcy, k = 86, 1.15
    text(im, 960, 1032, f'{WINDOWS} windows open  \u00b7  Slowed down 10\u00d7', REG, 38, MUT, opacity=vis * .85)
    for stock, sx in ((True, 960 - gap / 2 - sw), (False, 960 + gap / 2)):
        cx, cy = sx + sw / 2, top + sh / 2
        show_ms, full_ms = STOCK if stock else INSTANT
        blit(im, shadow_sprite(sw, sh, 16, 18, .5), cx, cy + 10, 'mm', 1, vis)
        done = (ms - show_ms) / 100  # seconds since this side's menu appeared
        pop = 1 + .025 * bump(prog(done, 0, .3))
        if not stock and done >= 0:
            blit(im, glow_sprite(sw, sh, 16, 12, ACC, .55), cx, cy, 'mm', pop, vis * e_out(prog(done, 0, .12)))
        blit(im, screen(prog(ms, show_ms, show_ms + 5), prog(ms, show_ms, full_ms)), sx, top, 'lt', 1, vis)
        if 0 <= t - T0 < 0.3:  # both desktops get the press at the same moment
            outline(im, cx, cy, sw, sh, (220, 235, 255, int(200 * (1 - (t - T0) / 0.3) * vis)), 4)
        if done >= 0:  # the finish border
            outline(im, cx, cy, sw, sh, (DONE_GREY if stock else ACC) + (255,), 3 if stock else 6,
                    1 if stock else pop, vis * e_out(prog(done, 0, .12)))
        if stock:
            text(im, cx, 745, 'Windows 11', SEMI, 56, TXT, opacity=vis)
        else:
            text(im, cx, 745, 'Instant Alt+Tab', BOLD, 56, ACC, opacity=vis)
        frozen = ms >= show_ms
        live = t >= T0
        col = (AMB if stock else ACC) if frozen else TXT
        text(im, cx, 848, f'{show_ms if frozen else int(ms)} ms', NUM, 146, col,
             scale=1 + 0.1 * bump(prog(ms, show_ms, show_ms + 30)) if frozen else 1,
             opacity=vis * (1 if live else .35), var=b'Bold')
        if live:
            if not frozen:
                cap, cop = 'Building the menu\u2026', (.55 + .45 * math.sin(t * 7) ** 2) * stock
            else:
                cap, cop = ('Built from scratch' if stock else 'Kept ready'), e_out(prog(ms, show_ms, show_ms + 25))
            text(im, cx, 940, cap, REG, 44, MUT, opacity=vis * cop)
    # Once both are done, "6x faster" takes the keys' place.
    badge = e_back(prog(t, T0 + (STOCK[1] + 46) / 100, T0 + (STOCK[1] + 96) / 100), 2.2)
    keys_op = vis * (1 - e_out(prog(t, T0 + (STOCK[1] + 31) / 100, T0 + (STOCK[1] + 56) / 100)))
    ripple(im, 960, kcy, KEYS_W * k, KEYS_H * k, t - T0, keys_op * .7)
    draw_alt_tab(im, 960, kcy, key_amount(t, [(T0, 99)]), keys_op, k)
    if badge > 0:
        text(im, 960, kcy, f'{STOCK[0] // INSTANT[0]}\u00d7 faster', BOLD, 104, ACC, scale=max(0.01, badge),
             opacity=vis * clamp(badge))
    return im


# ---------------------------------------------------------------- the timeline: two quick Alt+Tabs, 4x slower

def legend(im, y, opacity):
    """Colour key: amber = waiting for the menu, cyan = menu on screen."""
    chip, space, gap = 36, 16, 72
    items = [(AMB + (80,), AMB, text_sprite('Waiting for the menu', REG, 42, TXT)),
             (ACC + (255,), ACC, text_sprite('Menu on screen', REG, 42, TXT))]
    widths = [chip + space + spr.width / S - 12 for _, _, spr in items]
    x = 960 - (sum(widths) + gap * (len(items) - 1)) / 2
    for (fill, line, spr), w in zip(items, widths):
        lay = Image.new('RGBA', (int((chip + 8) * S), int((chip + 8) * S)), (0, 0, 0, 0))
        rrect(ImageDraw.Draw(lay), 4, 4, chip, chip, 8, fill=fill, outline=line, width=2.5)
        blit(im, lay, x - 4, y, 'lm', 1, opacity)
        blit(im, spr, x + chip + space - 6, y, 'lm', 1, opacity)
        x += w + gap


def scene_timeline(t):
    """Each lane paints what its menu does as the playhead passes: amber while waiting, cyan while shown."""
    im = background().copy()
    vis = e_out(prog(t, 0, .5)) * (1 - e_inout(prog(t, 7.3, 7.95)))
    presses, HOLD, SLOW = [0.0, 2.3], 1.85, 4.0
    T0, LEN = 0.8, presses[-1] + HOLD  # the track runs from the first press to the last release
    tau = clamp(t - T0, 0, LEN)
    pw, ph, margin, gap = 540, 304, 80, 60
    x0, x1 = margin, 1920 - margin - pw - gap
    pcx = 1920 - margin - pw / 2
    inset = 16  # the blocks' inner margin, matching their top and bottom
    X = lambda tt: x0 + inset + (x1 - x0 - 2 * inset) * tt / LEN
    kcy, k, th = 926, 1.5, 180
    legend(im, 72, vis)
    lanes = ((196, True), (530, False))
    tops = [ly + 38 for ly, _ in lanes]
    ending = e_out(prog(t, T0 + LEN + .1, T0 + LEN + .55))
    for (ly, stock), ty in zip(lanes, tops):
        show_ms, full_ms = STOCK if stock else INSTANT
        wait, full = show_ms * SLOW / 1000, full_ms * SLOW / 1000
        text(im, x0 - 6, ly, 'Windows 11' if stock else 'Instant Alt+Tab', BOLD, 58, TXT if stock else ACC, 'lm',
             opacity=vis)
        lay = Image.new('RGBA', (int((x1 - x0 + 20) * S), int((th + 20) * S)), (0, 0, 0, 0))
        d = ImageDraw.Draw(lay)
        rrect(d, 10, 10, x1 - x0, th, 18, fill=(20, 26, 44), outline=(44, 52, 78), width=1.4)
        for p in presses:
            if tau > p:
                rrect(d, 10 + X(p) - x0, 26, max(3, X(min(tau, p + wait)) - X(p)), th - 32, 10, fill=AMB + (80,),
                      outline=AMB, width=3)
                if tau > p + wait:
                    rrect(d, 10 + X(p + wait) - x0 + 5, 26, max(3, X(min(tau, p + HOLD)) - X(p + wait) - 5), th - 32,
                          10, fill=ACC + (255,))
        blit(im, lay, x0 - 10, ty - 10, 'lt', 1, vis)
        for p in presses:  # how long each press waited, under its amber block
            if tau > p + wait:
                pop = e_back(prog(tau, p + wait, p + wait + .25))
                text(im, X(p) - 4, ty + th + 34, f'{show_ms} ms', SEMI, 44, AMB, 'lm', scale=max(.01, pop),
                     opacity=vis * clamp(pop))
        state, fill = 0.0, 0.0
        for p in presses:
            if p + wait <= tau < p + HOLD and t >= T0:
                state, fill = 1.0, prog(tau, p + wait, p + full)
        pcy = ty + th / 2
        prev = vis * (1 - ending)
        blit(im, shadow_sprite(pw, ph, 12, 12, .5), pcx, pcy + 6, 'mm', 1, prev)
        if state > 0:
            blit(im, glow_sprite(pw, ph, 14, 10, ACC, .9), pcx, pcy, 'mm', 1, prev)
        blit(im, screen(state, fill), pcx, pcy, 'mm', pw / SCREEN_W, prev)
        # In the end the previews give way to how long each lane waited in total.
        text(im, pcx, pcy - 22, f'{len(presses) * show_ms} ms', NUM, 124, AMB if stock else ACC,
             scale=.9 + .1 * ending, opacity=vis * ending, var=b'Bold')
        text(im, pcx, pcy + 70, 'Spent waiting', REG, 42, MUT, opacity=vis * ending)
    if t >= T0 and tau < LEN:  # the playhead flares where a press lands, so the eye stays on the tracks
        px = X(tau)
        flare = max(bump(prog(tau, p, p + .45)) for p in presses)
        fq = round(flare, 1)  # quantized, so the glow sprites stay cached
        for ty in tops:
            cy = ty + th / 2
            blit(im, glow_sprite(int(6 + 16 * fq), th + 24, 3, 12, ACC, round(.6 + .35 * fq, 2)), px, cy, 'mm', 1,
                 vis)
            lay = Image.new('RGBA', (int(24 * S), int((th + 34) * S)), (0, 0, 0, 0))
            wd = 4 + 4 * flare
            rrect(ImageDraw.Draw(lay), 12 - wd / 2, 5, wd, th + 24, wd / 2, fill=(235, 250, 255))
            blit(im, lay, px, cy, 'mm', 1, vis)
    text(im, margin, kcy, f'{WINDOWS} windows open', REG, 40, MUT, 'lm', opacity=vis * .85)
    text(im, 1920 - margin, kcy, 'Slowed down 4\u00d7', REG, 40, MUT, 'rm', opacity=vis * .85)
    for p in presses:
        ripple(im, 960, kcy, KEYS_W * k, KEYS_H * k, t - T0 - p, vis)
    draw_alt_tab(im, 960, kcy, key_amount(t, [(T0 + p, T0 + p + HOLD) for p in presses]), vis, k)
    return im


# ---------------------------------------------------------------- output

def render(scene, path, fmt, fps):
    if fmt == 'webp':  # what the README shows: looping, 1280 wide, sharp on high-DPI screens
        codec = ['-vf', 'scale=1280:-1:flags=lanczos', '-c:v', 'libwebp_anim', '-lossless', '0', '-quality', '80',
                 '-compression_level', '6', '-loop', '0']
    else:
        codec = ['-c:v', 'libx264', '-preset', 'slow', '-crf', '16', '-pix_fmt', 'yuv420p', '-movflags', '+faststart']
    ff = subprocess.Popen(['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s',
                           f'{W}x{H}', '-r', str(fps), '-i', '-'] + codec + [path], stdin=subprocess.PIPE)
    for f in range(int(round(DURATION * fps))):
        ff.stdin.write(scene(f / fps).convert('RGB').resize((W, H), Image.BOX).tobytes())
    ff.stdin.close()
    if ff.wait():
        raise SystemExit(f'ffmpeg failed on {path}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('out', help='folder to write race and timeline to')
    parser.add_argument('--format', choices=['webp', 'mp4'], default='webp')
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    fps = 30 if args.format == 'webp' else 60
    for name, scene in (('race', scene_race), ('timeline', scene_timeline)):
        render(scene, os.path.join(args.out, f'{name}.{args.format}'), args.format, fps)
        print('wrote', os.path.join(args.out, f'{name}.{args.format}'))


if __name__ == '__main__':
    main()
