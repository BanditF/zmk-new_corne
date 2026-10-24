"""Draws the left-panel assets for the first-party plugins (spec: docs/plugin-screens.md).

  python -I scripts/plugin_panels.py                writes assets/plugins/<id>/panel.json + previews
  python -I scripts/plugin_panels.py --sheet x.png  also a review sheet of every state

The host panel is 64x52, 1 bit. Sprites are stored the way screens.panel_override takes its
bitmap: rows MSB first, each row padded to whole bytes, 1 = ink, base64. A plugin builds a frame
by ORing sprites at the "layout" positions, or sends a ready-made "frames" entry as it is.
"""

import argparse
import base64
import json
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 64, 52

# 3x5 font, 4 px per character (the Bandit HUD font)
F35 = {
    "A": "010101111101101", "B": "110101110101110", "C": "011100100100011", "D": "110101101101110",
    "E": "111100110100111", "F": "111100110100100", "G": "011100101101011", "H": "101101111101101",
    "I": "111010010010111", "J": "001001001101010", "K": "101101110101101", "L": "100100100100111",
    "M": "101111111101101", "N": "110101101101101", "O": "010101101101010", "P": "110101110100100",
    "Q": "010101101110011", "R": "110101110101101", "S": "011100010001110", "T": "111010010010010",
    "U": "101101101101111", "V": "101101101101010", "W": "101101111111101", "X": "101101010101101",
    "Y": "101101010010010", "Z": "111001010100111", "0": "111101101101111", "1": "010110010010111",
    "2": "110001010100111", "3": "110001010001110", "4": "101101111001001", "5": "111100110001110",
    "6": "011100111101111", "7": "111001010010010", "8": "111101111101111", "9": "111101111001110",
    "'": "010010000000000", "-": "000000111000000", ".": "000000000000010", ":": "000010000010000",
    "/": "001001010100100", " ": "000000000000000", "?": "110001010000010", "%": "101001010100101",
}
# 5x7, drawn at 2x for the mic-mute words
F57 = {
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    "V": ["10001", "10001", "10001", "10001", "01010", "01010", "00100"],
    "?": ["01110", "10001", "00001", "00110", "00100", "00000", "00100"],
}


def blank(w=W, h=H, v=0):
    return Image.new("1", (w, h), v)


def text35(img, x, y, s, v=1):
    px = img.load()
    for ch in s.upper():
        for i, b in enumerate(F35[ch]):
            if b == "1" and 0 <= x + i % 3 < img.width and 0 <= y + i // 3 < img.height:
                px[x + i % 3, y + i // 3] = v
        x += 4


def text35_w(s):
    return len(s) * 4 - 1


def text57x2(img, x, y, s, v=1, gap=2):
    d = ImageDraw.Draw(img)
    for ch in s:
        for r, row in enumerate(F57[ch]):
            for c, b in enumerate(row):
                if b == "1":
                    d.rectangle([x + 2 * c, y + 2 * r, x + 2 * c + 1, y + 2 * r + 1], fill=v)
        x += 10 + gap


def text57x2_w(s, gap=2):
    return len(s) * (10 + gap) - gap


def brackets(img, v=1, n=4):
    d = ImageDraw.Draw(img)
    for x, y, dx, dy in ((0, 0, 1, 1), (W - 1, 0, -1, 1), (0, H - 1, 1, -1), (W - 1, H - 1, -1, -1)):
        d.line([(x, y), (x + dx * (n - 1), y)], fill=v)
        d.line([(x, y), (x, y + dy * (n - 1))], fill=v)


def sprite(rows):
    img = blank(len(rows[0]), len(rows))
    px = img.load()
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            px[x, y] = 1 if c == "#" else 0
    return img


# ---------------------------------------------------------------- Pomodoro
DW, DH, T = 11, 21, 3  # seven-segment digit, stroke 3, chamfered, 1 px gaps
SEGS = {"0": "abcdef", "1": "bc", "2": "abged", "3": "abgcd", "4": "fgbc", "5": "afgcd",
        "6": "afgedc", "7": "abc", "8": "abcdefg", "9": "abcdfg"}


def digit(ch):
    img = blank(DW, DH)
    px = img.load()

    def hseg(y0):
        for dy in range(T):
            inset = 0 if dy == 1 else 1
            for x in range(2 + inset, DW - 2 - inset):
                px[x, y0 + dy] = 1

    def vseg(x0, y0, y1):
        for dx in range(T):
            inset = 0 if dx == 1 else 1
            for y in range(y0 + inset, y1 + 1 - inset):
                px[x0 + dx, y] = 1

    if ch == "1":  # centred, with a flag, so it doesn't read as a colon
        vseg(4, 1, 9)
        vseg(4, 11, 19)
        px[3, 2] = px[2, 3] = px[3, 3] = 1
        return img
    for s in SEGS[ch]:
        if s == "a":
            hseg(0)
        elif s == "g":
            hseg(9)
        elif s == "d":
            hseg(18)
        elif s == "f":
            vseg(0, 2, 8)
        elif s == "b":
            vseg(DW - T, 2, 8)
        elif s == "e":
            vseg(0, 12, 18)
        elif s == "c":
            vseg(DW - T, 12, 18)
    return img


def colon():
    img = blank(3, DH)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 5, 2, 7], fill=1)
    d.rectangle([0, 13, 2, 15], fill=1)
    return img


ICONS = {
    "focus": ["....#....", "..#####..", ".#..#..#.", ".#.....#.", "####.####",
              ".#.....#.", ".#..#..#.", "..#####..", "....#...."],
    "rest": ["..#.#.#..", "..#.#.#..", ".........", "#######..", "#.....###",
             "#.....#.#", "#.....###", ".#...#...", "..###...."],
    "paused": [".........", ".##...##.", ".##...##.", ".##...##.", ".##...##.",
               ".##...##.", ".##...##.", ".##...##.", "........."],
}

# where things go on the 64x52 panel
POMO_LAYOUT = {
    "icon": {"x": 2, "y": 2},
    "phase_label": {"x": 14, "y": 4},          # FOCUS / REST / PAUSED, 3x5 font
    "round_label": {"x": 61, "y": 4, "align": "right"},  # e.g. 2/4
    "digits": {"x": 5, "y": 13, "advance": 12, "colon_gap": 1},  # MM:SS: d d : d d
    "bar": {"x": 2, "y": 38, "w": 60, "h": 6},  # outline; fill the inside by progress
    "bar_ticks": {"y": 45, "every_fifth": True},
    "hint": {"x": 32, "y": 47, "align": "center"},  # optional line, 3x5 font, 15 chars max
}


def pomodoro_frame(mmss, phase, progress, round_text="", hint=""):
    """A reference frame: what the plugin should send for this state."""
    L = POMO_LAYOUT
    img = blank()
    img.paste(sprite(ICONS[phase]), (L["icon"]["x"], L["icon"]["y"]))
    text35(img, L["phase_label"]["x"], L["phase_label"]["y"], phase.upper())
    if round_text:
        text35(img, L["round_label"]["x"] - text35_w(round_text) + 1, L["round_label"]["y"], round_text)
    x, y = L["digits"]["x"], L["digits"]["y"]
    for ch in mmss:
        if ch == ":":
            g = L["digits"]["colon_gap"]
            img.paste(colon(), (x + g, y))
            x += 3 + 2 * g + 1
        else:
            img.paste(digit(ch), (x, y))
            x += L["digits"]["advance"]
    d = ImageDraw.Draw(img)
    b = L["bar"]
    d.rectangle([b["x"], b["y"], b["x"] + b["w"] - 1, b["y"] + b["h"] - 1], outline=1)
    inner = b["w"] - 4
    fill = round(inner * progress)
    if fill:
        d.rectangle([b["x"] + 2, b["y"] + 2, b["x"] + 1 + fill, b["y"] + b["h"] - 3], fill=1)
    for k in range(6):
        tx = b["x"] + 2 + round(k * (inner - 1) / 5)
        d.point((tx, L["bar_ticks"]["y"]), fill=1)
    if hint:
        text35(img, L["hint"]["x"] - text35_w(hint) // 2, L["hint"]["y"], hint)
    return img


# ---------------------------------------------------------------- mic mute
def mic(img, v, live=False, dotted=False):
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([26, 2, 37, 18], radius=5, outline=v, width=2)
    if live:  # the capsule's grille
        for y in (7, 10, 13):
            d.line([(29, y), (34, y)], fill=v)
    d.arc([21, 6, 42, 24], 0, 180, fill=v, width=2)
    d.rectangle([31, 24, 32, 26], fill=v)
    d.rectangle([25, 27, 38, 28], fill=v)
    if dotted:  # unknown: break every other row of the outline
        bg = 1 - v
        px = img.load()
        for y in range(2, 29):
            if y % 3 == 0:
                for x in range(20, 44):
                    if px[x, y] == v:
                        px[x, y] = bg


def mute_frame(state, label=""):
    if state == "muted":  # inverted, so it reads from across the desk
        img = blank(v=1)
        ink = 0
    else:
        img = blank()
        ink = 1
    d = ImageDraw.Draw(img)
    mic(img, ink, live=state == "live", dotted=state == "unknown")
    if state == "muted":  # the slash, cut out of the mic with a 1 px gap
        d.line([(19, 1), (44, 29)], fill=1 - ink, width=6)
        d.line([(19, 1), (44, 29)], fill=ink, width=3)
    if state == "live":  # sound waves either side
        for r in (0, 4):
            d.arc([14 - r, 6 - r, 24 + r, 20 + r], 120, 240, fill=ink, width=1)
            d.arc([39 - r, 6 - r, 49 + r, 20 + r], -60, 60, fill=ink, width=1)
    word = {"muted": "MUTED", "live": "LIVE", "unknown": "?"}[state]
    text57x2(img, (W - text57x2_w(word)) // 2, 31, word, v=ink)
    if label:
        text35(img, (W - text35_w(label)) // 2, 47, label, v=ink)
    brackets(img, ink)
    return img


# ---------------------------------------------------------------- encoding
def pack(img):
    w, h = img.size
    stride = (w + 7) // 8
    out = bytearray(stride * h)
    px = img.load()
    for y in range(h):
        for x in range(w):
            if px[x, y]:
                out[y * stride + x // 8] |= 0x80 >> (x % 8)
    return {"w": w, "h": h, "bitmap": base64.b64encode(bytes(out)).decode()}


def preview(img, scale=4):
    """As the keyboard shows it: ink lit (white) on dark."""
    return img.convert("L").resize((img.width * scale, img.height * scale), Image.NEAREST)


def font35():
    return {"w": 3, "h": 5, "advance": 4,
            "glyphs": {ch: pack(sprite(["".join("#" if g[r * 3 + c] == "1" else "." for c in range(3))
                                        for r in range(5)])) for ch, g in F35.items()}}


def write(plugin, data, previews):
    d = os.path.join(ROOT, "assets", "plugins", plugin)
    os.makedirs(os.path.join(d, "preview"), exist_ok=True)
    with open(os.path.join(d, "panel.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1)
        f.write("\n")
    for name, img in previews.items():
        preview(img).save(os.path.join(d, "preview", name + ".png"))
    return previews


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sheet")
    a = ap.parse_args()
    head = {"format": "foxglove-panel-assets", "version": 1, "width": W, "height": H,
            "encoding": "1 bpp, rows MSB first, each row padded to whole bytes, 1 = ink, base64"}
    pomo = dict(head, plugin="org.foxglove.focus-timer", layout=POMO_LAYOUT,
                digits={ch: pack(digit(ch)) for ch in "0123456789"}, colon=pack(colon()),
                icons={k: pack(sprite(v)) for k, v in ICONS.items()}, font=font35())
    pomo_prev = {
        "focus": pomodoro_frame("24:59", "focus", 1 / 1500, "1/4", "NEXT REST 5M"),
        "focus-late": pomodoro_frame("03:12", "focus", 0.87, "2/4"),
        "rest": pomodoro_frame("04:30", "rest", 0.1, "2/4", "STAND UP"),
        "paused": pomodoro_frame("12:05", "paused", 0.52, "3/4", "PAUSED"),
    }
    write("org.foxglove.focus-timer", pomo, pomo_prev)
    mute_states = {s: mute_frame(s) for s in ("muted", "live", "unknown")}
    mute = dict(head, plugin="org.foxglove.mic-mute",
                frames={s: pack(img) for s, img in mute_states.items()},
                layout={"label": {"x": 32, "y": 47, "align": "center", "ink": {"muted": 0, "live": 1, "unknown": 1}}},
                font=font35())
    mute_prev = dict(mute_states)
    mute_prev["muted-label"] = mute_frame("muted", "HEADSET")
    mute_prev["live-label"] = mute_frame("live", "USB MIC")
    write("org.foxglove.mic-mute", mute, mute_prev)
    print("wrote panel assets to assets/plugins/")
    if a.sheet:
        cells = list(pomo_prev.items()) + list(mute_prev.items())
        s = 4
        sheet = Image.new("L", (5 * (W * s + 12) + 12, 2 * (H * s + 28) + 12), 90)
        dr = ImageDraw.Draw(sheet)
        for i, (name, img) in enumerate(cells):
            x, y = 12 + (i % 5) * (W * s + 12), 12 + (i // 5) * (H * s + 28)
            dr.text((x, y), name, fill=255)
            sheet.paste(preview(img, s), (x, y + 14))
        sheet.save(a.sheet)
        print("sheet", a.sheet)


if __name__ == "__main__":
    main()
