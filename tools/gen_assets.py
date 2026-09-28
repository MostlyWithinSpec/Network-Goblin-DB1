#!/usr/bin/env python3
"""Generates main/assets.h (goblin sprites + anti-aliased fonts) and a preview PNG.

Goblin art is drawn as ASCII pixel-art. The body is written as a LEFT HALF
(16 columns) and mirrored, then face / prop overlays are stamped on top.
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_H = os.path.join(HERE, "..", "main", "assets.h")
OUT_PNG = os.path.join(HERE, "preview_sprites.png")

# ---------------------------------------------------------------- palette
# index 0 = transparent. Index 9/10 ("device screen") are recoloured at
# runtime to show network health (cyan = good, amber = slow, red = down).
PAL = {
    ".": (0, None),
    "K": (1, (0x14, 0x18, 0x14)),   # outline
    "G": (2, (0x5C, 0xB3, 0x38)),   # goblin green
    "g": (3, (0x3A, 0x7A, 0x25)),   # shade green
    "L": (4, (0x9A, 0xE0, 0x5E)),   # highlight green
    "R": (5, (0xFF, 0x2A, 0x2A)),   # red eye
    "W": (6, (0xF4, 0xF4, 0xE8)),   # white
    "P": (7, (0x6E, 0x14, 0x22)),   # mouth
    "b": (8, (0x6B, 0x4A, 0x2B)),   # brown vest
    "C": (9, (0x3F, 0xF0, 0xFF)),   # device glow (runtime recolour)
    "c": (10, (0x1A, 0x90, 0xA0)),  # device glow dark (runtime recolour)
    "d": (11, (0x45, 0x30, 0x1C)),  # dark brown
    "T": (12, (0xE0, 0x5A, 0x78)),  # tongue / pink
    "Y": (13, (0xFF, 0xD0, 0x40)),  # yellow
    "B": (14, (0x6A, 0xC8, 0xFF)),  # blue (sweat)
    "O": (15, (0xC8, 0x8A, 0x4A)),  # cookie
}

# ---------------------------------------------------------------- body
# left half, 16 cols, mirrored to 32 cols
BODY_L = [
    "................",  # 0
    "................",  # 1
    "..........KKKKKK",  # 2
    "........KKGGGGGG",  # 3
    "K......KGGGLLGGG",  # 4
    "KK....KGGLLGGGGG",  # 5
    "KgK..KGGLGGGGGGG",  # 6
    "KggK.KGGGGGGGGGG",  # 7
    ".KggKGGGGGGGGGGG",  # 8
    ".KGggGGGGGGGGGGG",  # 9
    "..KGgGGGGGGGGGGG",  # 10
    "...KKGGGGGGGGGGG",  # 11
    ".....KGGGGGGGGGG",  # 12
    ".....KGGGGGGGGGG",  # 13
    ".....KgGGGGGGGGG",  # 14
    "......KgGGGGGGGG",  # 15
    ".......KgGGGGGGG",  # 16
    "........KKgGGGGG",  # 17
    "..........KKKKGG",  # 18
    "............KKGG",  # 19
    "........KKKKbbKG",  # 20
    ".......KbbbbbbbK",  # 21
    "......KGKbbbbbbK",  # 22
    "......KGKbbbbbbK",  # 23
    ".....KGGKbbbbbbK",  # 24
    ".....KGGKbbbbbbK",  # 25
    ".....KGGKbbbbbbK",  # 26
    "......KKKbbbbbbK",  # 27
    ".......KddddddddK"[:16],  # 28 belt
    "........KbbbKKKK",  # 29
    "........KgggK...",  # 30
    ".......KKKKKK...",  # 31
]

# device held in both hands (full width, '_' = keep)
DEVICE = {
    22: "__________KKKKKKKKKKKK__________",
    23: "__________KCCCCCCCCCCK__________",
    24: "_______KKKKCcCCCCCcCCKKKK_______",
    25: "_______KGGKCCcCCCcCcCKGGK_______",
    26: "_______KGGKCCCcCcCCCCKGGK_______",
    27: "________KKKCCCCCCCCCCKKK________",
    28: "__________KKKKKKKKKKKK__________",
}

# ---------------------------------------------------------------- faces
# left-half overlays for rows 9..17, mirrored. '_' = keep.
FACES = {
    "idle": {
        10: "_______KKK______",
        11: "_______KRRRK____",
        12: "_______KRWRK____",
        13: "________KKK_____",
        14: "_______________g",
        15: "__________K_____",
        16: "___________KKKKK",
    },
    "blink": {
        10: "________________",
        11: "________________",
        12: "_______KKKKK____",
        13: "________________",
        14: "_______________g",
        15: "__________K_____",
        16: "___________KKKKK",
    },
    "happy": {
        10: "________________",
        11: "________KKK_____",
        12: "_______K___K____",
        13: "________________",
        14: "_________K_____g",
        15: "__________KKKKKK",
        16: "__________KWPPPP",
        17: "___________KKKKK",
    },
    "sweat": {
        10: "________KKKK____",
        11: "_______KRRRK____",
        12: "_______KRWRK____",
        13: "________KKK_____",
        14: "_______________g",
        15: "___________K_K_K",
        16: "__________K_K_K_",
    },
    "panic": {
        9:  "_______KK_______",
        10: "______KWWWK_____",
        11: "______KWRWK_____",
        12: "______KWWWK_____",
        13: "_______KKK______",
        14: "_______________g",
        15: "____________KKKK",
        16: "___________KPPPP",
        17: "____________KKKK",
    },
    "sleep": {
        10: "________________",
        11: "________________",
        12: "_______KKKKK____",
        13: "________KK______",
        14: "_______________g",
        15: "________________",
        16: "_____________KKK",
    },
    "surprise": {
        10: "_______KWWK_____",
        11: "______KWWWWK____",
        12: "______KWRRWK____",
        13: "_______KWWK_____",
        14: "_______________g",
        15: "______________KK",
        16: "_____________KPP",
        17: "______________KK",
    },
    "hungry": {
        10: "________K_______",
        11: "_______KRRRK____",
        12: "_______KRWRK____",
        13: "________KKK_____",
        14: "_______________g",
        15: "___________KKKKK",
        16: "___________KPPPP",
        17: "____________KTTT",
    },
    "grumpy": {
        10: "_______KKKK_____",
        11: "_______KKKKK____",
        12: "_______KRWRK____",
        13: "________KKK_____",
        14: "_______________g",
        15: "________________",
        16: "__________KKKKKK",
    },
    "angry": {
        9:  "______KK________",
        10: "_______KKK______",
        11: "________KKKK____",
        12: "_______KRRRK____",
        13: "________KKK_____",
        14: "_______________g",
        15: "__________KKKKKK",
        16: "__________KWKWKW",
        17: "__________KKKKKK",
    },
    "eat": {
        10: "________________",
        11: "________KKK_____",
        12: "_______K___K____",
        13: "________________",
        14: "_______________g",
        15: "_________KKKKKKK",
        16: "_________KWPPPPP",
        17: "__________KPPPPP",
    },
}


def mirror(half):
    assert len(half) == 16, (half, len(half))
    return half + half[::-1]


def stamp(grid, row, line):
    assert len(line) == 32, (row, line, len(line))
    r = list(grid[row])
    for i, ch in enumerate(line):
        if ch != "_":
            r[i] = ch
    grid[row] = "".join(r)


def build(face, device=True):
    grid = [mirror(r) for r in BODY_L]
    for row, half in FACES[face].items():
        stamp(grid, row, mirror(half))
    if device:
        for row, line in DEVICE.items():
            stamp(grid, row, line)
    return grid


# small props (full rows, any width)
PROPS = {
    "sweat": [
        "..B..",
        ".BBB.",
        "BBWBB",
        "BBBBB",
        ".BBB.",
    ],
    "heart": [
        ".RR.RR.",
        "RRRRRRR",
        "RWRRRRR",
        ".RRRRR.",
        "..RRR..",
        "...R...",
    ],
    "cookie": [
        "..KKKK..",
        ".KOOOOK.",
        "KOdOOdOK",
        "KOOOOOOK",
        "KOOdOOdK",
        "KOOOOOOK",
        ".KOdOOK.",
        "..KKKK..",
    ],
    "vein": [
        "RR.RR",
        "R...R",
        ".....",
        "R...R",
        "RR.RR",
    ],
    "bang": [
        "YY",
        "YY",
        "YY",
        "YY",
        "..",
        "YY",
    ],
}

FRAME_ORDER = ["idle", "blink", "happy", "sweat", "panic", "sleep", "surprise", "hungry", "eat", "grumpy", "angry"]


def pack4(grid):
    out = []
    w = len(grid[0])
    for row in grid:
        assert len(row) == w
        vals = [PAL[ch][0] for ch in row]
        if len(vals) % 2:
            vals.append(0)
        for i in range(0, len(vals), 2):
            out.append((vals[i] << 4) | vals[i + 1])
    return out


def rgb565(c):
    r, g, b = c
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


# ---------------------------------------------------------------- fonts
def gen_font(name, path, size, chars):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    line_h = ascent + descent
    glyphs = []
    data = []
    for ch in chars:
        bbox = font.getbbox(ch)
        adv = round(font.getlength(ch))
        if bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            glyphs.append((len(data), 0, 0, 0, 0, adv))
            continue
        w = bbox[2] - bbox[0]
        h = bbox[3] - bbox[1]
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-bbox[0], -bbox[1]), ch, font=font, fill=255)
        px = list(img.tobytes())
        off = len(data)
        vals = [p >> 4 for p in px]
        if len(vals) % 2:
            vals.append(0)
        for i in range(0, len(vals), 2):
            data.append((vals[i] << 4) | vals[i + 1])
        glyphs.append((off, w, h, bbox[0], bbox[1], adv))
    return {"name": name, "glyphs": glyphs, "data": data, "line_h": line_h, "ascent": ascent,
            "first": ord(chars[0]), "count": len(chars)}


def c_array(name, ctype, vals, per_line=24):
    lines = []
    for i in range(0, len(vals), per_line):
        lines.append("    " + ", ".join(f"0x{v:02X}" if ctype == "uint8_t" else f"0x{v:04X}" for v in vals[i:i + per_line]) + ",")
    return f"static const {ctype} {name}[{len(vals)}] = {{\n" + "\n".join(lines) + "\n};\n"


def main():
    frames = {f: build(f) for f in FRAME_ORDER}

    # ---- preview
    S = 8
    prev = Image.new("RGB", (len(FRAME_ORDER) * (32 * S + 16) + 16, 32 * S + 60), (12, 18, 16))
    dr = ImageDraw.Draw(prev)
    for i, f in enumerate(FRAME_ORDER):
        ox = 16 + i * (32 * S + 16)
        for y, row in enumerate(frames[f]):
            for x, ch in enumerate(row):
                c = PAL[ch][1]
                if c:
                    dr.rectangle([ox + x * S, 16 + y * S, ox + x * S + S - 1, 16 + y * S + S - 1], fill=c)
        dr.text((ox, 32 * S + 24), f, fill=(255, 255, 255))
    prev.save(OUT_PNG)

    # ---- header
    out = []
    out.append("// AUTO-GENERATED by tools/gen_assets.py - do not edit\n#pragma once\n#include <stdint.h>\n")
    pal = [0] * 16
    for ch, (idx, col) in PAL.items():
        if col:
            pal[idx] = rgb565(col)
    out.append(c_array("goblin_palette", "uint16_t", pal))
    out.append("#define SPR_W 32\n#define SPR_H 32\n")
    enum = ", ".join(f"FR_{f.upper()}" for f in FRAME_ORDER)
    out.append(f"typedef enum {{ {enum}, FR_COUNT }} goblin_frame_t;\n")
    allpx = []
    for f in FRAME_ORDER:
        allpx += pack4(frames[f])
    out.append(c_array("goblin_frames", "uint8_t", allpx))
    out.append("#define GOBLIN_FRAME(i) (&goblin_frames[(i) * (SPR_W * SPR_H / 2)])\n")

    out.append("typedef struct { const uint8_t *px; uint8_t w, h; } prop_t;\n")
    for name, grid in PROPS.items():
        out.append(c_array(f"prop_{name}_px", "uint8_t", pack4(grid)))
        out.append(f"static const prop_t PROP_{name.upper()} = {{ prop_{name}_px, {len(grid[0])}, {len(grid)} }};\n")

    out.append("typedef struct { uint16_t off; uint8_t w, h; int8_t xo, yo; uint8_t adv; } glyph_t;\n")
    out.append("typedef struct { const glyph_t *g; const uint8_t *data; uint8_t first, count, line_h, ascent; } font_t;\n")
    chars = "".join(chr(c) for c in range(32, 127))
    bold = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
    for fname, size in (("small", 13), ("med", 17), ("big", 30)):
        f = gen_font(fname, bold, size, chars)
        assert len(f["data"]) < 65535
        out.append(c_array(f"font_{fname}_data", "uint8_t", f["data"]))
        gl = ",\n".join(f"    {{{o},{w},{h},{xo},{yo},{a}}}" for (o, w, h, xo, yo, a) in f["glyphs"])
        out.append(f"static const glyph_t font_{fname}_glyphs[] = {{\n{gl}\n}};\n")
        out.append(f"static const font_t FONT_{fname.upper()} = {{ font_{fname}_glyphs, font_{fname}_data, {f['first']}, {f['count']}, {f['line_h']}, {f['ascent']} }};\n")

    # inject the idle goblin into the web page header
    import json, re
    html_p = os.path.join(HERE, "..", "main", "web", "index.html")
    html = open(html_p).read()
    pal_js = {ch: "#%02x%02x%02x" % col for ch, (idx, col) in PAL.items() if col}
    html = re.sub(r"/\*ROWS\*/.*?/\*END\*/", lambda m: "/*ROWS*/" + json.dumps(frames["happy"]) + "/*END*/", html, flags=re.S)
    html = re.sub(r"/\*PAL\*/.*?/\*END\*/", lambda m: "/*PAL*/" + json.dumps(pal_js) + "/*END*/", html, flags=re.S)
    open(html_p, "w").write(html)
    import gzip
    with open(html_p + ".gz", "wb") as gz:
        gz.write(gzip.compress(html.encode(), compresslevel=9, mtime=0))

    with open(OUT_H, "w") as fh:
        fh.write("\n".join(out))
    print("wrote", OUT_H, "and", OUT_PNG)


if __name__ == "__main__":
    main()
