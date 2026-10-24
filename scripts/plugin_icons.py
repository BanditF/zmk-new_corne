"""Draws the Plugins tab's icons and empty-state art into app/resources/plugins/ (spec:
docs/plugin-icons.md).

  python -I scripts/plugin_icons.py            write every icon set and empty.svg
  python -I scripts/plugin_icons.py --sheet out.png   also render a review sheet (needs resvg-py)

Each glyph is drawn once on a 24x24 grid as (role, svg element) parts. A role picks the colour
per variant: "ink" for line work, "accent" for the Bandit orange, "r"/"g"/"b" and "rgb" (a
gradient) for colour, "focus"/"rest" for the focus timer. The glyph and tray variants are
single-colour, so roles only matter for the tiles.
"""

import argparse
import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "app", "resources", "plugins")

STROKE = 'fill="none" stroke-width="2" stroke-linecap="square" stroke-linejoin="miter"'


def arc(cx, cy, r, a0, a1):
    """An SVG path for a clockwise arc from angle a0 to a1 (degrees, 0 = right, 90 = down)."""
    import math

    p = lambda a: (cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
    (x0, y0), (x1, y1) = p(a0), p(a1)
    large = 1 if (a1 - a0) % 360 > 180 else 0
    return '<path d="M%.2f %.2f A%g %g 0 %d 1 %.2f %.2f"/>' % (x0, y0, r, r, large, x1, y1)


FOX = "4,3 9,8 15,8 20,3 20,12 12,21 4,12"

GLYPHS = {
    # fallback for plugins with no icon: the Bandit fox head
    "_default": [
        ("ink", '<polygon points="%s"/>' % FOX),
        ("accent", '<path d="M7.5 12.5 L10.5 13.8 M16.5 12.5 L13.5 13.8"/>'),
        ("ink", '<path d="M11 17.5 H13"/>'),
    ],
    # 25 minutes of focus (red) and 5 of rest (green) round the dial, as the timer runs
    "org.foxglove.focus-timer": [
        ("focus", arc(12, 13.5, 8, -90, 210)),
        ("rest", arc(12, 13.5, 8, 210, 270)),
        ("ink", '<path d="M10 2.5 H14 M12 2.5 V5.5 M18.5 6 L19.8 4.7"/>'),
        ("accent", '<path d="M12 13.5 V9"/>'),
    ],
    # colour arriving over the network (E1.31): broadcast arcs over three channels
    "org.foxglove.openrgb": [
        ("ink", arc(12, 13, 4, -135, -45)),
        ("ink", arc(12, 13, 8, -140, -40)),
        ("accent", '<path d="M12 13 V13.01"/>'),
        ("r", '<path d="M5 19 H8"/>'),
        ("g", '<path d="M10.5 19 H13.5"/>'),
        ("b", '<path d="M16 19 H19"/>'),
    ],
    # another app's lighting signal, carried through
    "org.foxglove.signalrgb": [
        ("rgb", '<polyline points="2,12 6,12 8.5,6 11.5,18 14.5,5 17,16 19,12 22,12"/>'),
    ],
    # the system microphone, with the mute slash in red
    "org.foxglove.mic-mute": [
        ("ink", '<rect x="9" y="2.5" width="6" height="11" rx="3"/>'),
        ("ink", '<path d="M5.5 10.5 A6.5 6.5 0 0 0 18.5 10.5 M12 17 V20.5 M8.5 21 H15.5"/>'),
        ("r", '<path d="M4 3 L20 19"/>'),
    ],
    # lighting mirrored onto a key: rays over a keycap
    "org.foxglove.icue": [
        ("ink", '<rect x="5" y="10" width="14" height="11" rx="2"/>'),
        ("ink", '<rect x="8.5" y="12.5" width="7" height="5" rx="0.5"/>'),
        ("g", '<path d="M12 7 V3"/>'),
        ("r", '<path d="M8 7.5 L5.5 4.5"/>'),
        ("b", '<path d="M16 7.5 L18.5 4.5"/>'),
    ],
    # kept in sync: two arrows round a key
    "org.foxglove.lightsync": [
        ("ink", arc(12, 12, 8.5, 200, 330)),
        ("ink", '<path d="M18.2 5.6 L19.4 7.8 L17 8.4"/>'),
        ("ink", arc(12, 12, 8.5, 20, 150)),
        ("ink", '<path d="M5.8 18.4 L4.6 16.2 L7 15.6"/>'),
        ("accent", '<rect x="9" y="9" width="6" height="6" rx="1"/>'),
    ],
}

# tile palettes; "rgb" is a left-to-right gradient of r, g, b
DARK = dict(bg="#16171b", edge="#2c2e35", ink="#ece8e1", accent="#ff7a1a", bracket="#ff7a1a",
            r="#ff4d4d", g="#3ddc84", b="#4d8bff", focus="#ff4d4d", rest="#3ddc84")
LIGHT = dict(bg="#f6f3ee", edge="#d9d4cc", ink="#22242a", accent="#d45d00", bracket="#d45d00",
             r="#e02424", g="#12a150", b="#1f5fe0", focus="#e02424", rest="#12a150")


def colour(role, pal):
    return "url(#rgb)" if role == "rgb" else pal[role]


def glyph_body(parts, pal=None, mono="#ffffff"):
    out = []
    for role, el in parts:
        c = colour(role, pal) if pal else mono
        out.append('<g stroke="%s">%s</g>' % (c, el))
    return "\n  ".join(out)


def gradient(pal):
    return ('<defs><linearGradient id="rgb" x1="2" y1="0" x2="22" y2="0" gradientUnits="userSpaceOnUse">'
            '<stop offset="0" stop-color="%s"/><stop offset="0.5" stop-color="%s"/>'
            '<stop offset="1" stop-color="%s"/></linearGradient></defs>' % (pal["r"], pal["g"], pal["b"]))


def glyph_svg(parts, size=24, mono="#ffffff"):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 24 24">\n'
            '<g %s>\n  %s\n</g>\n</svg>\n' % (size, size, STROKE, glyph_body(parts, mono=mono)))


def tile_svg(parts, pal):
    # 48x48 rounded tile, HUD corner brackets, the glyph at 1.25x in the middle
    br = []
    for x, y, dx, dy in ((5, 5, 1, 1), (43, 5, -1, 1), (5, 43, 1, -1), (43, 43, -1, -1)):
        br.append("M%g %g V%g M%g %g H%g" % (x, y + 4 * dy, y, x, y, x + 4 * dx))
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 48 48">\n'
            '%s\n'
            '<rect x="0.5" y="0.5" width="47" height="47" rx="10" fill="%s" stroke="%s"/>\n'
            '<path d="%s" fill="none" stroke="%s" stroke-width="1.25" stroke-opacity="0.7"/>\n'
            '<g transform="translate(9 9) scale(1.25)" %s>\n  %s\n</g>\n</svg>\n'
            % (gradient(pal), pal["bg"], pal["edge"], " ".join(br), pal["bracket"], STROKE,
               glyph_body(parts, pal)))


def empty_svg():
    """The empty Plugins tab: Bandit asleep in an empty den. Mono white; the app tints it."""
    s = 1.8  # the fox head from the glyph, scaled and resting its chin on the ground
    head = " ".join("%g,%g" % (float(x) * s + 56, float(y) * s + 92)
                    for x, y in (p.split(",") for p in FOX.split()))
    ticks = " ".join("M%d 137 V%d" % (x, 141 if (x - 24) % 48 else 144) for x in range(24, 217, 12))
    br = " ".join("M%g %g V%g M%g %g H%g" % (x, y + 10 * dy, y, x, y, x + 10 * dx)
                  for x, y, dx, dy in ((6, 6, 1, 1), (234, 6, -1, 1), (6, 154, 1, -1), (234, 154, -1, -1)))
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="240" height="160" viewBox="0 0 240 160">\n'
            '<g fill="none" stroke="#ffffff" stroke-linecap="square" stroke-linejoin="miter">\n'
            '  <path d="%s" stroke-width="2" stroke-opacity="0.6"/>\n'
            '  <path d="M44 130 A76 76 0 0 1 196 130" stroke-width="3"/>\n'
            '  <path d="M30 130 A90 90 0 0 1 210 130" stroke-width="1.5" stroke-dasharray="2 7" stroke-opacity="0.6"/>\n'
            '  <path d="M114 32 H126 L120 40 Z" stroke-width="2"/>\n'
            '  <path d="M16 130 H224" stroke-width="3"/>\n'
            '  <path d="%s" stroke-width="1.5" stroke-opacity="0.6"/>\n'
            '  <path d="M92 110 C100 96 150 94 162 122 L164 130" stroke-width="3"/>\n'
            '  <path d="M163 129 C150 114 120 115 102 126" stroke-width="3"/>\n'
            '  <path d="M106 127 L109 122 M112 124 L115 119.5 M118 122 L120.5 118" stroke-width="2" stroke-opacity="0.7"/>\n'
            '  <polygon points="%s" stroke-width="3"/>\n'
            '  <path d="M68 116 Q71 119 74 117 M87 116 Q84 119 81 117" stroke-width="2.5"/>\n'
            '  <path d="M76 124 H79" stroke-width="2.5"/>\n'
            '</g>\n</svg>\n' % (br, ticks, head))


def render(svg, width):
    import resvg_py
    from PIL import Image

    data = resvg_py.svg_to_bytes(svg_string=svg, width=width)
    return Image.open(io.BytesIO(bytes(data))).convert("RGBA")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sheet")
    a = ap.parse_args()
    for pid, parts in GLYPHS.items():
        d = os.path.join(OUT, pid)
        os.makedirs(d, exist_ok=True)
        files = {"glyph.svg": glyph_svg(parts), "icon.svg": tile_svg(parts, DARK),
                 "icon-light.svg": tile_svg(parts, LIGHT)}
        for name, text in files.items():
            open(os.path.join(d, name), "w", encoding="utf-8", newline="\n").write(text)
        for name, ink in (("tray-white.png", "#ffffff"), ("tray-black.png", "#000000")):
            render(glyph_svg(parts, mono=ink), 32).save(os.path.join(d, name))
    open(os.path.join(OUT, "empty.svg"), "w", encoding="utf-8", newline="\n").write(empty_svg())
    print("wrote", len(GLYPHS), "icon sets and empty.svg to", OUT)
    if a.sheet:
        sheet(a.sheet)


def sheet(path):
    """Every icon on dark and light backgrounds at the sizes the app uses, plus the empty state."""
    from PIL import Image, ImageDraw

    themes = (("#1b1b1f", "#e6e6e6", DARK, "icon.svg"), ("#f8f8f8", "#202020", LIGHT, "icon-light.svg"))
    row_h, W = 110, 1100
    im = Image.new("RGBA", (W, row_h * len(GLYPHS) * 2 + 2 * 340), "#808080")
    d = ImageDraw.Draw(im)
    y = 0
    for bg, fg, pal, tile in themes:
        for pid, parts in GLYPHS.items():
            d.rectangle((0, y, W, y + row_h - 2), fill=bg)
            d.text((8, y + 4), pid, fill=fg)
            x = 8
            for w in (48, 96):  # tile at 1x and 2x
                t = render(tile_svg(parts, pal), w)
                im.alpha_composite(t, (x, y + 18))
                x += w + 20
            for w in (16, 20, 24, 48):  # tinted glyph in rows and menus
                g = render(glyph_svg(parts, mono=fg), w)
                im.alpha_composite(g, (x, y + 18 + (48 - w) // 2))
                x += w + 20
            tray = render(glyph_svg(parts, mono="#ffffff" if pal is DARK else "#000000"), 32)
            im.alpha_composite(tray.resize((16, 16), Image.LANCZOS), (x, y + 34))
            im.alpha_composite(tray, (x + 30, y + 26))
            y += row_h
    for bg, fg, pal, _ in themes:
        d.rectangle((0, y, W, y + 338), fill=bg)
        e = render(empty_svg().replace("#ffffff", fg), 480)
        im.alpha_composite(e, (8, y + 8))
        im.alpha_composite(render(empty_svg().replace("#ffffff", fg), 240), (520, y + 8))
        y += 340
    im.save(path)
    print("sheet", path)


if __name__ == "__main__":
    main()
