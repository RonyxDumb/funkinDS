"""Sheet builder shared by the converters (textures + frames made of parts,
see gfx.c), fonts and bottom screen images. Taken from WWCD DS."""
import os
import re
from PIL import Image, ImageDraw, ImageFont
from common import *  # noqa

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'nitrofiles')


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


def trim(im, thr=8):
    bb = im.getchannel('A').point(lambda a: 255 if a >= thr else 0).getbbox()
    if bb is None:
        return im.crop((0, 0, 1, 1)), (0, 0)
    return im.crop(bb), (bb[0], bb[1])


C_OUT = []
H_OUT = []


# ---------------------------------------------------------------------------
# Sheets: groups of textures + frames made of textured parts (see gfx.c).
# Frame origin = (0,0); parts are placed at (ox, oy) texels from it.
# ---------------------------------------------------------------------------
class Sheet:
    def __init__(self, name, scale, fmt=FMT_256COL, colors=255, dither=False, atlas_w=256, atlas_h=256):
        self.name = name
        self.scale = scale          # texels per drawn unit
        self.fmt = fmt
        self.ncolors = colors
        self.dither = dither
        self.atlas_w = atlas_w
        self.atlas_h = atlas_h
        self.items = []

    def add_tiled(self, fname, im, ox=0, oy=0):
        self.items.append((fname, 'tiled', im, (ox, oy)))

    def add_packed(self, fname, im, ox=0, oy=0):
        if im.width > self.atlas_w or im.height > self.atlas_h:
            # too big for the shared atlas: own power-of-two tiles
            self.items.append((fname, 'tiled', im, (ox, oy)))
        else:
            self.items.append((fname, 'packed', im, (ox, oy)))

    def build(self):
        imgs = [it[2] for it in self.items]
        if self.fmt == FMT_256COL:
            palimg, colors = build_palette(imgs, self.ncolors)
        else:
            palimg, colors = build_palette(imgs, 15)
        textures = []
        frames = []
        parts = []
        atlas = None
        packed = [it for it in self.items if it[1] == 'packed']
        # pack tall images first for a tighter atlas, but keep frame order
        order = sorted(range(len(self.items)), key=lambda i: -self.items[i][2].height if self.items[i][1] == 'packed' else 0)
        placed = {}
        for i in order:
            fname, kind, im, (ox, oy) = self.items[i]
            if kind != 'packed':
                continue
            w, h = im.size
            if atlas is None or atlas['x'] + w > self.atlas_w:
                if atlas is not None:
                    atlas['y'] += atlas['rowh'] + 1
                    atlas['x'] = 0
                    atlas['rowh'] = 0
            if atlas is None or atlas['y'] + h > self.atlas_h:
                atlas = {'img': Image.new('RGBA', (self.atlas_w, 256), (0, 0, 0, 0)), 'x': 0, 'y': 0, 'rowh': 0, 'used_h': 0}
                textures.append(('atlas', atlas))
                atlas['tex'] = len(textures) - 1
            atlas['img'].paste(im, (atlas['x'], atlas['y']))
            placed[i] = (atlas['tex'], atlas['x'], atlas['y'], w, h, ox, oy)
            atlas['x'] += w + 1
            atlas['rowh'] = max(atlas['rowh'], h)
            atlas['used_h'] = max(atlas['used_h'], atlas['y'] + h)
        for i, (fname, kind, im, (ox, oy)) in enumerate(self.items):
            if kind == 'tiled':
                w, h = im.size
                first = len(parts)
                ys = 0
                for th in pow2_pieces(h):
                    xs = 0
                    for tw in pow2_pieces(w):
                        tile = Image.new('RGBA', (tw, th), (0, 0, 0, 0))
                        tile.paste(im.crop((xs, ys, min(xs + tw, w), min(ys + th, h))), (0, 0))
                        cw = min(tw, w - xs)
                        ch = min(th, h - ys)
                        if tile.getchannel('A').getbbox() is not None:
                            textures.append(('img', tile))
                            parts.append((len(textures) - 1, 0, 0, cw, ch, ox + xs, oy + ys))
                        xs += tw
                    ys += th
                frames.append((fname, first, len(parts) - first))
            else:
                parts.append(placed[i])
                frames.append((fname, len(parts) - 1, 1))
        blob = bytearray(pal_bytes(colors, 256 if self.fmt == FMT_256COL else 16))
        texdefs = []
        for kind, obj in textures:
            if kind == 'atlas':
                hh = 8
                while hh < obj['used_h']:
                    hh *= 2
                ww = self.atlas_w
                # shrink the atlas width when possible
                maxx = obj['img'].getchannel('A').getbbox()
                if maxx:
                    while ww > 8 and ww // 2 >= maxx[2]:
                        ww //= 2
                timg = obj['img'].crop((0, 0, ww, hh))
            else:
                timg = obj
            tw, th = timg.size
            idx = index_image(timg, palimg, dither=self.dither)
            if self.fmt == FMT_16COL:
                pk = bytearray(len(idx) // 2)
                for k in range(0, len(idx), 2):
                    pk[k // 2] = (idx[k] & 15) | ((idx[k + 1] & 15) << 4)
                idx = pk
            off = len(blob)
            blob += idx
            while len(blob) % 4:
                blob.append(0)
            texdefs.append((self.fmt, log2size(tw), log2size(th), off, len(idx)))
        fn = 'gfx/%s.bin' % self.name
        with open(outpath(fn), 'wb') as f:
            f.write(blob)
        n = self.name
        C_OUT.append('static const TexDef %s_tex[] = {%s};' % (n, ','.join('{%d,%d,%d,0,%d,%d}' % t for t in texdefs)))
        C_OUT.append('static const PartDef %s_parts[] = {%s};' % (n, ','.join('{%d,0,%d,%d,%d,%d,%d,%d}' % p for p in parts)))
        C_OUT.append('static const FrameDef %s_frames[] = {%s};' % (n, ','.join('{%d,%d}' % (f[1], f[2]) for f in frames)))
        C_OUT.append('const SheetDef sheet_%s = {%s, %s, %d, %d, %d, 0, %s_tex, %s_frames, %s_parts};' % (
            n, cstr('/' + fn), cfloat(self.scale), len(texdefs), len(frames),
            256 if self.fmt == FMT_256COL else 16, n, n, n))
        H_OUT.append('extern const SheetDef sheet_%s;' % n)
        for i, f in enumerate(frames):
            H_OUT.append('#define %s_%s %d' % (n.upper(), cid(f[0]), i))
        self.vram = sum(t[4] for t in texdefs)
        print('sheet %-12s %3d tex %3d frames %7d bytes vram' % (n, len(texdefs), len(frames), self.vram))
        return self


# ---------------------------------------------------------------------------
# Fonts: A3I5 glyph atlases (alpha in the top 3 bits, luminance in the low 5:
# white fill / black outline). Used as 3D textures and for the bottom screen.
# ---------------------------------------------------------------------------
FONTS = []   # (name, ttf path, px size, outline px) - filled by convert.py
FALLBACK = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
MDP_MISSING = ''
MDPF = None


def convert_fonts():
    H_OUT.append('enum { %s, FONT_COUNT };' % ', '.join('FONT_' + f[0].upper() for f in FONTS))
    names = []
    for nm, path, size, ol in FONTS:
        font = ImageFont.truetype(path, size)
        fb = ImageFont.truetype(FALLBACK, max(6, size - 2)) if path == MDPF and os.path.exists(FALLBACK) else None
        asc, desc = font.getmetrics()
        lh = asc + desc
        glyphs = []
        for ch in range(32, 127):
            c = chr(ch)
            fnt = fb if (fb and c in MDP_MISSING) else font
            adv = int(round(fnt.getlength(c)))
            bb = fnt.getbbox(c)
            w = max(1, bb[2] - min(0, bb[0])) + 2 * ol + 4
            gi = Image.new('RGBA', (w, lh + 2 * ol + 4), (0, 0, 0, 0))
            dr = ImageDraw.Draw(gi)
            pos = (2 + ol - min(0, bb[0]), 2 + ol)
            if ol:
                dr.text(pos, c, font=fnt, fill=(255, 255, 255, 255), stroke_width=ol, stroke_fill=(0, 0, 0, 255))
            else:
                dr.text(pos, c, font=fnt, fill=(255, 255, 255, 255))
            bbx = gi.getchannel('A').getbbox()
            if bbx is None:
                glyphs.append((ch, None, 0, 0, adv))
                continue
            g = gi.crop(bbx)
            glyphs.append((ch, g, bbx[0] - pos[0] + min(0, bb[0]), bbx[1] - pos[1], adv))
        W = 256
        x = y = rowh = 0
        place = []
        for ch, g, ox, oy, adv in glyphs:
            if g is None:
                place.append((0, 0, 0, 0, 0, 0, adv))
                continue
            if x + g.width > W:
                x = 0
                y += rowh + 1
                rowh = 0
            place.append((x, y, g.width, g.height, ox, oy, adv))
            x += g.width + 1
            rowh = max(rowh, g.height)
        H = 8
        while H < y + rowh:
            H *= 2
        atlas = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        for (ch, g, *_), (px, py, *_) in zip(glyphs, place):
            if g is not None:
                atlas.paste(g, (px, py))
        out = bytearray(W * H)
        data = atlas.tobytes()
        for i in range(W * H):
            r, gg, b, a = data[i * 4:i * 4 + 4]
            a = min(255, int(255 * (a / 255.0) ** 0.6))
            a3 = (a + 18) // 36
            if a3 <= 0:
                continue
            out[i] = (min(7, a3) << 5) | (r >> 3)
        fn = 'font/%s.bin' % nm
        with open(outpath(fn), 'wb') as fo:
            fo.write(out)
        C_OUT.append('static const Glyph font_%s_g[] = {%s};' % (nm, ','.join('{%d,%d,%d,%d,%d,%d,%d}' % p for p in place)))
        C_OUT.append('const FontDef font_%s = {%s, %d, %d, %d, %d, font_%s_g};' % (nm, cstr('/' + fn), log2size(W), log2size(H), lh, asc, nm))
        names.append('&font_' + nm)
    C_OUT.append('const FontDef *const font_defs[] = {%s};' % ','.join(names))
    H_OUT.append('extern const FontDef *const font_defs[];')


# ---------------------------------------------------------------------------
# Bottom-screen images (RGB15 + alpha bit)
# ---------------------------------------------------------------------------
def write_b16(name, im, bg=None):
    im = im.convert('RGBA')
    if bg is not None:
        b = Image.new('RGBA', im.size, bg)
        b.alpha_composite(im)
        im = b
    out = bytearray((im.width & 255, im.width >> 8, im.height & 255, im.height >> 8))
    data = im.tobytes()
    for i in range(im.width * im.height):
        r, g, b, a = data[i * 4:i * 4 + 4]
        v = rgb15(r, g, b) | (0x8000 if a >= 128 else 0)
        out += bytes((v & 255, v >> 8))
    with open(outpath('sub', name + '.b16'), 'wb') as f:
        f.write(out)


MS = 0.2


class MSheet(Sheet):
    def __init__(self, name, fmt=FMT_256COL, colors=255):
        Sheet.__init__(self, name, MS, fmt=fmt, colors=colors)

    def image(self, fname, im, bake, tiled=False, trimit=True):
        """box top-left = frame origin"""
        if trimit:
            t, (bx, by) = trim(im)
        else:
            t, bx, by = im, 0, 0
        d = MS * bake
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
    C_OUT.append('const u8 %s[] = {%s};' % (name, ','.join(str(i) for i in idx)))
    H_OUT.append('extern const u8 %s[];\n#define %s_LEN %d' % (name, name.upper(), len(idx)))


