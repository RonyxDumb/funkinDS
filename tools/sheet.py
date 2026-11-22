"""Sheet builder shared by the converters (textures + frames made of parts,
see source/gfx.c) and fonts."""
import os
import re
from PIL import Image, ImageDraw, ImageFont
from common import *  # noqa

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'romfs')


def outpath(*p):
    path = os.path.join(OUT, *p)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def cfloat(v):
    return repr(float(v)) + 'f'


def cid(s):
    return re.sub(r'[^A-Za-z0-9]', '_', s).upper()


def scaled(im, s, nearest=False):
    w = max(1, int(round(im.width * s)))
    h = max(1, int(round(im.height * s)))
    return im.resize((w, h), Image.NEAREST if nearest else Image.LANCZOS)


def trim(im, thr=4):
    bb = im.getchannel('A').point(lambda a: 255 if a >= thr else 0).getbbox()
    if bb is None:
        return im.crop((0, 0, 1, 1)), (0, 0)
    return im.crop(bb), (bb[0], bb[1])


C_OUT = []
H_OUT = []
GAP = 2           # texels between packed images (bilinear filtering)


def write_textures(fn, images, nearest=False):
    """Writes the textures (zlib compressed) to romfs/fn, returns the TexDef rows."""
    blob = bytearray()
    defs = []
    for im, w, h in images:
        fmt, raw = encode_texture(im, w, h)
        z = zpack(raw)
        defs.append((fmt, log2size(w), log2size(h), 1 if nearest else 0, len(blob), len(z), len(raw)))
        blob += z
        while len(blob) % 4:
            blob.append(0)
    with open(outpath(fn), 'wb') as f:
        f.write(blob)
    return defs


# ---------------------------------------------------------------------------
# Sheets: groups of textures + frames made of textured parts (see gfx.c).
# Frame origin = (0,0); parts are placed at (ox, oy) texels from it.
# ---------------------------------------------------------------------------
class Sheet:
    def __init__(self, name, scale, nearest=False, atlas_w=1024, atlas_h=1024, **_):
        self.name = name
        self.scale = scale          # texels per drawn unit
        self.nearest = nearest
        self.atlas_w = atlas_w
        self.atlas_h = atlas_h
        self.items = []

    def add_tiled(self, fname, im, ox=0, oy=0):
        self.items.append((fname, 'tiled', im, (ox, oy)))

    def add_packed(self, fname, im, ox=0, oy=0):
        if im.width + GAP > self.atlas_w or im.height + GAP > self.atlas_h:
            # too big for the shared atlas: own power-of-two textures
            self.items.append((fname, 'tiled', im, (ox, oy)))
        else:
            self.items.append((fname, 'packed', im, (ox, oy)))

    def build(self):
        textures = []       # (img, w, h)
        frames = []
        parts = []
        atlas = None
        atlases = []
        # pack tall images first for a tighter atlas, but keep frame order
        order = sorted(range(len(self.items)), key=lambda i: -self.items[i][2].height if self.items[i][1] == 'packed' else 0)
        placed = {}
        for i in order:
            fname, kind, im, (ox, oy) = self.items[i]
            if kind != 'packed':
                continue
            w, h = im.size
            if atlas is not None and atlas['x'] + w > self.atlas_w:
                atlas['y'] += atlas['rowh'] + GAP
                atlas['x'] = 0
                atlas['rowh'] = 0
            if atlas is None or atlas['y'] + h > self.atlas_h:
                atlas = {'img': Image.new('RGBA', (self.atlas_w, self.atlas_h), (0, 0, 0, 0)), 'x': 0, 'y': 0, 'rowh': 0,
                         'used_w': 0, 'used_h': 0, 'tex': len(atlases)}
                atlases.append(atlas)
            atlas['img'].paste(im, (atlas['x'], atlas['y']))
            placed[i] = (atlas['tex'], atlas['x'], atlas['y'], w, h, ox, oy)
            atlas['x'] += w + GAP
            atlas['rowh'] = max(atlas['rowh'], h)
            atlas['used_w'] = max(atlas['used_w'], atlas['x'])
            atlas['used_h'] = max(atlas['used_h'], atlas['y'] + h + 1)
        for a in atlases:
            ww, hh = pow2(a['used_w']), pow2(a['used_h'])
            textures.append((a['img'].crop((0, 0, ww, hh)), ww, hh))
        for i, (fname, kind, im, (ox, oy)) in enumerate(self.items):
            if kind == 'tiled':
                w, h = im.size
                first = len(parts)
                for ys, th, dh in pieces(h):
                    for xs, tw, dw in pieces(w):
                        piece = im.crop((xs, ys, min(xs + tw, w), min(ys + th, h)))
                        if piece.getchannel('A').getbbox() is not None:
                            textures.append((piece, tw, th))
                            parts.append((len(textures) - 1, 0, 0, dw, dh, ox + xs, oy + ys))
                frames.append((fname, first, len(parts) - first))
            else:
                t, x, y, w, h, ox_, oy_ = placed[i]
                parts.append((t, x, y, w, h, ox_, oy_))
                frames.append((fname, len(parts) - 1, 1))
        fn = 'gfx/%s.bin' % self.name
        texdefs = write_textures(fn, textures, self.nearest)
        n = self.name
        C_OUT.append('static const TexDef %s_tex[] = {%s};' % (n, ','.join('{%d,%d,%d,%d,%d,%d,%d}' % t for t in texdefs) or '{0}'))
        C_OUT.append('static const PartDef %s_parts[] = {%s};' % (n, ','.join('{%d,0,%d,%d,%d,%d,%d,%d}' % p for p in parts) or '{0}'))
        C_OUT.append('static const FrameDef %s_frames[] = {%s};' % (n, ','.join('{%d,%d}' % (f[1], f[2]) for f in frames) or '{0}'))
        C_OUT.append('const SheetDef sheet_%s = {%s, %s, %d, %d, %s_tex, %s_frames, %s_parts};' % (
            n, cstr('/' + fn), cfloat(self.scale), len(texdefs), len(frames), n, n, n))
        H_OUT.append('extern const SheetDef sheet_%s;' % n)
        for i, f in enumerate(frames):
            H_OUT.append('#define %s_%s %d' % (n.upper(), cid(f[0]), i))
        self.vram = sum(t[6] for t in texdefs)
        print('sheet %-20s %3d tex %3d frames %7d KB' % (n, len(texdefs), len(frames), self.vram // 1024))
        return self


# ---------------------------------------------------------------------------
# Fonts: RGBA glyph atlases (white fill, black outline), 1 texel per pixel
# ---------------------------------------------------------------------------
FONTS = []   # (name, ttf path, px size, outline px) - filled by convert.py


def convert_fonts():
    H_OUT.append('enum { %s, FONT_COUNT };' % ', '.join('FONT_' + f[0].upper() for f in FONTS))
    names = []
    for nm, path, size, ol in FONTS:
        font = ImageFont.truetype(path, size)
        asc, desc = font.getmetrics()
        lh = asc + desc
        glyphs = []
        for ch in range(32, 127):
            c = chr(ch)
            adv = int(round(font.getlength(c)))
            bb = font.getbbox(c)
            w = max(1, bb[2] - min(0, bb[0])) + 2 * ol + 4
            gi = Image.new('RGBA', (w, lh + 2 * ol + 4), (0, 0, 0, 0))
            dr = ImageDraw.Draw(gi)
            pos = (2 + ol - min(0, bb[0]), 2 + ol)
            if ol:
                dr.text(pos, c, font=font, fill=(255, 255, 255, 255), stroke_width=ol, stroke_fill=(0, 0, 0, 255))
            else:
                dr.text(pos, c, font=font, fill=(255, 255, 255, 255))
            bbx = gi.getchannel('A').getbbox()
            if bbx is None:
                glyphs.append((ch, None, 0, 0, adv))
                continue
            g = gi.crop(bbx)
            glyphs.append((ch, g, bbx[0] - pos[0] + min(0, bb[0]), bbx[1] - pos[1], adv))
        W = 512
        x = y = rowh = 0
        place = []
        for ch, g, ox, oy, adv in glyphs:
            if g is None:
                place.append((0, 0, 0, 0, 0, 0, adv))
                continue
            if x + g.width + GAP > W:
                x = 0
                y += rowh + GAP
                rowh = 0
            place.append((x, y, g.width, g.height, ox, oy, adv))
            x += g.width + GAP
            rowh = max(rowh, g.height)
        H = pow2(y + rowh + 1)
        atlas = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        for (ch, g, *_), (px, py, *_) in zip(glyphs, place):
            if g is not None:
                atlas.paste(g, (px, py))
        fn = 'font/%s.bin' % nm
        t = write_textures(fn, [(atlas, W, H)])[0]
        C_OUT.append('static const TexDef font_%s_tex[] = {{%d,%d,%d,%d,%d,%d,%d}};' % ((nm,) + t))
        C_OUT.append('static const PartDef font_%s_parts[] = {{0,0,0,0,%d,%d,0,0}};' % (nm, W, H))
        C_OUT.append('static const FrameDef font_%s_frames[] = {{0,1}};' % nm)
        C_OUT.append('static const SheetDef font_%s_sheet = {%s, 1.0f, 1, 1, font_%s_tex, font_%s_frames, font_%s_parts};' % (
            nm, cstr('/' + fn), nm, nm, nm))
        C_OUT.append('static const Glyph font_%s_g[] = {%s};' % (nm, ','.join('{%d,%d,%d,%d,%d,%d,%d}' % p for p in place)))
        C_OUT.append('const FontDef font_%s = {&font_%s_sheet, %d, %d, font_%s_g};' % (nm, nm, lh, asc, nm))
        names.append('&font_' + nm)
    C_OUT.append('const FontDef *const font_defs[] = {%s};' % ','.join(names))
    H_OUT.append('extern const FontDef *const font_defs[];')


MS = K


class MSheet(Sheet):
    """Menu sheets: images baked at MS texels per menu unit (1280x720 view)."""
    def __init__(self, name, q=1.0, **kw):
        Sheet.__init__(self, name, MS * q, **kw)
        self.q = q

    def image(self, fname, im, bake, tiled=False, trimit=True):
        """box top-left = frame origin"""
        if trimit:
            t, (bx, by) = trim(im)
        else:
            t, bx, by = im, 0, 0
        d = MS * self.q * bake
        ts = t.resize((max(1, int(round(t.width * d))), max(1, int(round(t.height * d)))), Image.LANCZOS)
        ox, oy = int(round(bx * d)), int(round(by * d))
        (self.add_tiled if tiled else self.add_packed)(fname, ts, ox, oy)

    def sparrow(self, prefix, xmlpath, pngpath, bake, names=None):
        im, frames = parse_sparrow(xmlpath, pngpath)
        seen = {}
        out = []
        for f in frames:
            if names and not any(f['name'].startswith(n) for n in names):
                continue
            k = (f['x'], f['y'], f['w'], f['h'], f['fx'], f['fy'])
            if k not in seen:
                seen[k] = len(self.items)
                self.image('%s%d' % (prefix, len(seen) - 1), full_frame(im, f), bake)
            out.append((f['name'], seen[k]))
        return out, frames


def emit_seq(name, idx):
    C_OUT.append('const u8 %s[] = {%s};' % (name, ','.join(str(i) for i in idx) or '0'))
    H_OUT.append('extern const u8 %s[];\n#define %s_LEN %d' % (name, name.upper(), len(idx)))
