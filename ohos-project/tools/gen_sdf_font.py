# Offline SDF font-atlas generator for the OHOS GLES demo.
#
# Mirrors MikanEngine's atlas contract (see engine/shaders/glsl/ui2d_sdf.frag):
# RGBA PNG where R=G=B hold the normalised signed distance (0.5 = glyph edge,
# >0.5 inside) so the runtime median() reconstruction degenerates to the plain
# SDF value.  Metrics are emitted as a C++ header; the atlas PNG ships in the
# HAP rawfile, loaded exactly like BrdfLut.png.
#
# Pipeline per glyph: render at 4x supersample -> box-downsample to the 1x
# cell grid -> threshold to a binary mask -> Felzenszwalb EDT for inside and
# outside distances -> signed distance in 1x pixels -> normalise by pxRange.

import json
import os
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts", "simhei.ttf")
OUT_PNG = r"D:\ohosengine\sdl3-ohos-vulkan\ohos-project\entry\src\main\resources\rawfile\SdfAtlas.png"
OUT_HEADER = r"D:\ohosengine\sdl3-ohos-vulkan\ohos-project\entry\src\main\cpp\application\rhi\sdf_font_metrics.h"

# ASCII letters cover the title (MIKAN), the splash subtitle, the debug
# animation-state HUD (WALK/IDLE/ATTACK/...), the FPS counter and the value
# labels; digits/punct serve the FPS.  The CJK range holds the localized UI
# strings (menus/settings/death screen/health bars), baked from the same
# SimHei face -- full-width glyphs advance 1em instead of 0.5em.
CHARSET = ("0123456789.:FPSfps-+%ACDEHIJKLMNOPRTUWBGVXYZx×"
           "开始游戏设置菜单你死了重试关视角灵敏度泛光渲染分辨率显示血量敌人调试面板鸿蒙运行时返回标题"
           "背包物品装备武器盔甲护剑弓药水空卸轻点用铁法透明度")
CHARSET += "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ基于自研游戏引擎的鸿蒙系统客户端实例"
CHARSET = "".join(dict.fromkeys(CHARSET))  # dedupe, keep first-seen order
EM = 64            # 1x em size the metrics are measured at
PAD = 8            # 1x margin around the em box inside a cell
CELL = EM + 2 * PAD   # 80 atlas texels per cell
SS = 4             # supersample factor for rasterisation
PX_RANGE = 8.0     # signed-distance range in texels (edge at 0.5)
COLUMNS = 8

INF = 1e12

def dt1d(f):
    """Felzenszwalb 1D squared-distance transform."""
    n = len(f)
    d = [0.0] * n
    v = [0] * n
    z = [0.0] * (n + 1)
    k = 0
    v[0] = 0
    z[0] = -INF
    z[1] = INF
    for q in range(1, n):
        s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2 * q - 2 * v[k])
        while s <= z[k]:
            k -= 1
            s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2 * q - 2 * v[k])
        k += 1
        v[k] = q
        z[k] = s
        z[k + 1] = INF
    k = 0
    for q in range(n):
        while z[k + 1] < q:
            k += 1
        d[q] = (q - v[k]) * (q - v[k]) + f[v[k]]
    return d

def edt(mask):
    """Distance (in grid units) from every cell to the nearest True cell."""
    h, w = len(mask), len(mask[0])
    grid = [[0.0 if mask[y][x] else INF for x in range(w)] for y in range(h)]
    for y in range(h):
        grid[y] = dt1d(grid[y])
    for x in range(w):
        col = dt1d([grid[y][x] for y in range(h)])
        for y in range(h):
            grid[y][x] = col[y]
    return grid

font1x = ImageFont.truetype(FONT_PATH, EM)
fontss = ImageFont.truetype(FONT_PATH, EM * SS)

glyphs = []
for ch in CHARSET:
    bbox = font1x.getbbox(ch)
    advance = font1x.getlength(ch)
    if bbox is None or bbox[2] - bbox[0] <= 0 or bbox[3] - bbox[1] <= 0:
        # No ink (e.g. space): advance-only glyph, no atlas cell.
        glyphs.append({"ch": ch, "offsetX": 0.0, "offsetY": 0.0, "w": 0.0,
                       "h": 0.0, "advance": round(advance, 2),
                       "cellX": -1, "cellY": -1})
        continue
    x0, y0, x1, y1 = bbox
    gw, gh = x1 - x0, y1 - y0
    assert gw <= EM and gh <= EM, (ch, bbox)

    # Render supersampled, ink top-left pinned at PAD*SS.
    bx0, by0, bx1, by1 = fontss.getbbox(ch)
    img = Image.new("L", (CELL * SS, CELL * SS), 0)
    draw = ImageDraw.Draw(img)
    draw.text((PAD * SS - bx0, PAD * SS - by0), ch, font=fontss, fill=255)
    coverage = img.resize((CELL, CELL), Image.BOX)
    mask = [[coverage.getpixel((x, y)) >= 128 for x in range(CELL)] for y in range(CELL)]

    dist_in = edt(mask)
    inv = [[not mask[y][x] for x in range(CELL)] for y in range(CELL)]
    dist_out = edt(inv)

    rows = []
    for y in range(CELL):
        row = bytearray(CELL)
        for x in range(CELL):
            sd = (dist_out[y][x] ** 0.5) - (dist_in[y][x] ** 0.5)  # >0 inside
            v = 0.5 + 0.5 * sd / PX_RANGE
            row[x] = round(max(0.0, min(1.0, v)) * 255.0)
        rows.append(row)

    glyphs.append({"ch": ch, "offsetX": float(x0), "offsetY": float(y0),
                   "w": float(gw), "h": float(gh), "advance": round(advance, 2),
                   "cellX": -1, "cellY": -1, "_rows": rows})

placed = [g for g in glyphs if g["cellX"] != -2 and "_rows" in g]
rows_count = (len(placed) + COLUMNS - 1) // COLUMNS
atlas = Image.new("RGBA", (COLUMNS * CELL, rows_count * CELL), (0, 0, 0, 255))
index = 0
for g in glyphs:
    if "_rows" not in g:
        continue
    cx = (index % COLUMNS) * CELL
    cy = (index // COLUMNS) * CELL
    g["cellX"] = cx
    g["cellY"] = cy
    for y in range(CELL):
        for x in range(CELL):
            v = g["_rows"][y][x]
            atlas.putpixel((cx + x, cy + y), (v, v, v, 255))
    del g["_rows"]
    index += 1

atlas.save(OUT_PNG)

# ---- emit the C++ metrics header -------------------------------------------
def fnum(v):
    return "%.2ff" % v

lines = []
lines.append("// GENERATED by gen_sdf_font.py -- do not edit by hand.")
lines.append("// Atlas: rawfile/SdfAtlas.png, RGB = normalised signed distance")
lines.append("// (0.5 = glyph edge, >0.5 inside), metrics measured at a %dpx em." % EM)
lines.append("#pragma once")
lines.append("")
lines.append("namespace rhi {")
lines.append("")
lines.append("constexpr int kSdfAtlasWidth = %d;" % atlas.width)
lines.append("constexpr int kSdfAtlasHeight = %d;" % atlas.height)
lines.append("constexpr int kSdfCellSize = %d;" % CELL)
lines.append("constexpr int kSdfFontEm = %d;" % EM)
lines.append("constexpr float kSdfPxRange = %.1ff;" % PX_RANGE)
lines.append("")
lines.append("struct SdfGlyph {")
lines.append("    unsigned int cp;  // Unicode codepoint (glyphs bake one per cp)")
lines.append("    float offsetX;")
lines.append("    float offsetY;")
lines.append("    float w;")
lines.append("    float h;")
lines.append("    float advance;")
lines.append("    int cellX;")
lines.append("    int cellY;")
lines.append("};")
lines.append("")
lines.append("inline constexpr SdfGlyph kSdfGlyphs[] = {")
for g in glyphs:
    lines.append("    {0x%04X, %s, %s, %s, %s, %s, %d, %d}," %
                 (ord(g["ch"]), fnum(g["offsetX"]), fnum(g["offsetY"]), fnum(g["w"]),
                  fnum(g["h"]), fnum(g["advance"]), g["cellX"], g["cellY"]))
lines.append("};")
lines.append("inline constexpr int kSdfGlyphCount = %d;" % len(glyphs))
lines.append("")
lines.append("// Decodes one UTF-8 sequence at *text and advances it past the sequence.")
lines.append("// Invalid or truncated bytes decode to U+FFFD, which misses every glyph,")
lines.append("// so the callers' unknown-glyph fallback advance kicks in safely.")
lines.append("inline unsigned int SdfNextCodepoint(const char** text) {")
lines.append("    const unsigned char* p = reinterpret_cast<const unsigned char*>(*text);")
lines.append("    const unsigned char lead = p[0];")
lines.append("    if (lead < 0x80u) {")
lines.append("        *text += 1;")
lines.append("        return lead;")
lines.append("    }")
lines.append("    int len = 0;")
lines.append("    unsigned int cp = 0;")
lines.append("    if ((lead & 0xE0u) == 0xC0u) { len = 2; cp = lead & 0x1Fu; }")
lines.append("    else if ((lead & 0xF0u) == 0xE0u) { len = 3; cp = lead & 0x0Fu; }")
lines.append("    else if ((lead & 0xF8u) == 0xF0u) { len = 4; cp = lead & 0x07u; }")
lines.append("    else {")
lines.append("        *text += 1;")
lines.append("        return 0xFFFDu;")
lines.append("    }")
lines.append("    for (int i = 1; i < len; ++i) {")
lines.append("        const unsigned char cont = p[i];")
lines.append("        if ((cont & 0xC0u) != 0x80u) {")
lines.append("            *text += i;")
lines.append("            return 0xFFFDu;")
lines.append("        }")
lines.append("        cp = (cp << 6) | (cont & 0x3Fu);")
lines.append("    }")
lines.append("    *text += len;")
lines.append("    return cp;")
lines.append("}")
lines.append("")
lines.append("// Linear scan is fine: the atlas holds ~90 glyphs.")
lines.append("inline const SdfGlyph* SdfFindGlyph(unsigned int cp) {")
lines.append("    for (int i = 0; i < kSdfGlyphCount; ++i) {")
lines.append("        if (kSdfGlyphs[i].cp == cp) {")
lines.append("            return &kSdfGlyphs[i];")
lines.append("        }")
lines.append("    }")
lines.append("    return nullptr;")
lines.append("}")
lines.append("")
lines.append("} // namespace rhi")
with open(OUT_HEADER, "w", encoding="ascii", newline="\n") as fh:
    fh.write("\n".join(lines) + "\n")

print("atlas %dx%d -> %s" % (atlas.width, atlas.height, OUT_PNG))
print("glyphs: %d (%d inked, %d advance-only)" %
      (len(glyphs), len(placed), len(glyphs) - len(placed)))
print("header -> %s" % OUT_HEADER)
