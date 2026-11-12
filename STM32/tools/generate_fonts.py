"""Build the screen's 2-bit glyph tables from the strings actually used by ui.c."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import argparse

root = Path(__file__).resolve().parents[1] / 'AntDec-O1'
parser = argparse.ArgumentParser()
parser.add_argument('--font', default='C:/Windows/Fonts/msyh.ttc')
args = parser.parse_args()
source = (root / 'Core/Src/ui.c').read_text(encoding='utf-8')
chars = sorted(set(chr(c) for c in range(32, 127)) | {c for c in source if ord(c) > 127})
out = ['#ifndef FONT_UI_H', '#define FONT_UI_H', '#include <stdint.h>',
       'typedef struct { uint16_t codepoint; uint8_t width, advance; uint32_t offset; } ui_glyph_t;',
       'typedef struct { uint8_t height; uint16_t count; const ui_glyph_t *glyphs; const uint8_t *bitmap; } ui_font_t;']
for size in (12, 16, 24):
    font = ImageFont.truetype(args.font, size)
    height = size + 2
    bitmap = []
    glyphs = []
    for ch in chars:
        advance = max(1, round(font.getlength(ch)))
        box = font.getbbox(ch)
        width = max(advance, box[2])
        canvas = Image.new('L', (width, height))
        ImageDraw.Draw(canvas).text((0, -2), ch, font=font, fill=255)
        pixels = [min(3, (v + 42) // 85) for v in canvas.getdata()]
        offset = len(bitmap)
        pixels.extend([0] * ((-len(pixels)) % 4))
        bitmap.extend((pixels[i] << 6) | (pixels[i+1] << 4) | (pixels[i+2] << 2) | pixels[i+3]
                      for i in range(0, len(pixels), 4))
        glyphs.append((ord(ch), width, advance, offset))
    out.append(f'static const uint8_t g_font_ui_{size}_bitmap[] = {{')
    out.extend('  ' + ', '.join(f'0x{x:02X}' for x in bitmap[i:i+24]) + ',' for i in range(0, len(bitmap), 24))
    out.append('};')
    out.append(f'static const ui_glyph_t g_font_ui_{size}_glyphs[] = {{')
    out.extend(f'  {{0x{cp:04X}U, {w}U, {adv}U, {off}UL}},' for cp, w, adv, off in glyphs)
    out.append('};')
    out.append(f'static const ui_font_t g_font_ui_{size} = {{{height}U, {len(glyphs)}U, g_font_ui_{size}_glyphs, g_font_ui_{size}_bitmap}};')
out.append('#endif')
(root / 'Core/Inc/font_ui.h').write_text('\n'.join(out) + '\n', encoding='utf-8')
print(f'Generated {len(chars)} glyphs at 12/16/24 pixels')
