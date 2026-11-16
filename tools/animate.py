"""Adobe Animate texture atlas (Better-TA "BTA" compact format) flattener.

Follows flixel-animate (MaybeMaru, the library Friday Night Funkin' uses):
  * the sprite frame of an atlas animation is the whole-timeline bounds of
    the main timeline (constant for every frame),
  * a frame label animation plays main timeline frames I .. I+DU-1,
  * graphic symbols pick their frame with FF / LP (loop, play once, single
    frame), movie clips behave like looping graphics (no swfMode),
  * layers are drawn from the last to the first one.

Each output frame is rendered with Pillow at the requested texture scale.
"""
import json
import math
import os

from PIL import Image


def _load_json(path):
    with open(path, encoding='utf-8-sig') as f:
        return json.load(f)


# verbose (non optimized) export -> compact keys
_LONG = {'ANIMATION': 'AN', 'SYMBOL_DICTIONARY': 'SD', 'Symbols': 'S', 'SYMBOL_name': 'SN', 'TIMELINE': 'TL',
         'LAYERS': 'L', 'Layer_name': 'LN', 'Layer_type': 'LT', 'Frames': 'FR', 'index': 'I', 'duration': 'DU',
         'elements': 'E', 'SYMBOL_Instance': 'SI', 'ATLAS_SPRITE_instance': 'ASI', 'name': 'N', 'firstFrame': 'FF',
         'loop': 'LP', 'symbolType': 'ST', 'Matrix3D': 'M3D', 'transformationPoint': 'TRP', 'color': 'C', 'mode': 'M',
         'metadata': 'MD', 'framerate': 'FRT', 'Instance_Name': 'IN', 'RedMultiplier': 'RM', 'redMultiplier': 'RM',
         'greenMultiplier': 'GM', 'blueMultiplier': 'BM', 'alphaMultiplier': 'AM', 'redOffset': 'RO',
         'greenOffset': 'GO', 'blueOffset': 'BO', 'AlphaOffset': 'AO', 'alphaOffset': 'AO', 'brightness': 'BRT',
         'tintMultiplier': 'TM', 'tintColor': 'TC', 'StageInstance': 'STI', 'Matrix': 'MX'}


def _compact(o):
    if isinstance(o, dict):
        return {_LONG.get(k, k): _compact(v) for k, v in o.items()}
    if isinstance(o, list):
        return [_compact(v) for v in o]
    return o


def _matrix(e):
    m = e.get('MX')
    if m is not None:
        return tuple(float(v) for v in m)
    m = e.get('M3D')
    if m is not None:
        if isinstance(m, dict):
            m = [m[k] for k in ('m00', 'm01', 'm02', 'm03', 'm10', 'm11', 'm12', 'm13',
                                'm20', 'm21', 'm22', 'm23', 'm30', 'm31', 'm32', 'm33')]
        return (float(m[0]), float(m[1]), float(m[4]), float(m[5]), float(m[12]), float(m[13]))
    return (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


def mmul(p, c):
    """p * c: apply c first, then p (flash matrix concat: child in parent)."""
    a, b, cc, d, tx, ty = c
    pa, pb, pc, pd, ptx, pty = p
    return (a * pa + b * pc, a * pb + b * pd,
            cc * pa + d * pc, cc * pb + d * pd,
            tx * pa + ty * pc + ptx, tx * pb + ty * pd + pty)


def mapply(m, x, y):
    return (m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5])


IDENT_CT = (1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0)


def _color(c):
    if not c:
        return IDENT_CT
    mode = c.get('M')
    if mode in ('AD', 'Advanced'):
        return (c.get('RM', 1), c.get('GM', 1), c.get('BM', 1), c.get('AM', 1),
                c.get('RO', 0), c.get('GO', 0), c.get('BO', 0), c.get('AO', 0))
    if mode in ('CA', 'Alpha'):
        return (1, 1, 1, c.get('AM', 1), 0, 0, 0, 0)
    if mode in ('CBRT', 'Brightness'):
        br = c.get('BRT', 0)
        m = 1 - abs(br)
        o = br * 255 if br >= 0 else 0
        return (m, m, m, 1, o, o, o, 0)
    if mode in ('T', 'Tint'):
        tc = c.get('TC', '#000000').lstrip('#')
        r, g, b = int(tc[0:2], 16), int(tc[2:4], 16), int(tc[4:6], 16)
        tm = c.get('TM', 0)
        m = 1 - tm
        return (m, m, m, 1, r * tm, g * tm, b * tm, 0)
    return IDENT_CT


def ccat(child, parent):
    """child transform applied first, then parent."""
    m1, m2 = child, parent
    return (m1[0] * m2[0], m1[1] * m2[1], m1[2] * m2[2], m1[3] * m2[3],
            m1[4] * m2[0] + m2[4], m1[5] * m2[1] + m2[5], m1[6] * m2[2] + m2[6], m1[7] * m2[3] + m2[7])


class Timeline:
    def __init__(self, name, tl):
        self.name = name
        self.layers = []
        self.count = 0
        for L in tl.get('L', []):
            if L.get('LT') in ('Clp', 'Clipper', 'Fld', 'Folder'):
                # masks are not supported: the mask layer itself is skipped
                continue
            frames = L.get('FR', [])
            self.layers.append(frames)
            for fr in frames:
                self.count = max(self.count, fr['I'] + fr['DU'])

    def labels(self):
        out = {}
        for frames in self.layers:
            for fr in frames:
                if fr.get('N'):
                    out.setdefault(fr['N'], list(range(fr['I'], fr['I'] + fr['DU'])))
        return out

    @staticmethod
    def frame_at(frames, idx):
        # keyframes are sorted by index
        lo, hi = 0, len(frames) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            fr = frames[mid]
            if idx < fr['I']:
                hi = mid - 1
            elif idx >= fr['I'] + fr['DU']:
                lo = mid + 1
            else:
                return fr
        return None


class Atlas:
    def __init__(self, folder):
        self.folder = folder
        d = _load_json(os.path.join(folder, 'Animation.json'))
        if 'AN' not in d:
            d = _compact(d)
        self.symbols = {}
        for s in d.get('SD', {}).get('S', []):
            self.symbols[s['SN']] = Timeline(s['SN'], s['TL'])
        an = d['AN']
        if 'TL' not in an and 'STI' in an:   # main timeline given as a stage instance of a symbol
            an['TL'] = self.symbols[an['STI']['SI']['SN']].__dict__ and {'L': []}
        self.main = Timeline(an.get('SN', an.get('N', 'main')), an['TL'])
        if not self.main.layers and 'STI' in an:
            self.main = self.symbols[an['STI']['SI']['SN']]
        self.fps = float(d.get('MD', {}).get('FRT', 24))
        # spritemaps
        self.sprites = {}
        i = 1
        while True:
            p = os.path.join(folder, 'spritemap%d.json' % i)
            if not os.path.exists(p):
                break
            sm = _load_json(p)
            img = Image.open(os.path.join(folder, sm['meta']['image'])).convert('RGBA')
            res = float(sm['meta'].get('resolution', 1) or 1)
            for s in sm['ATLAS']['SPRITES']:
                s = s['SPRITE']
                c = img.crop((s['x'], s['y'], s['x'] + s['w'], s['y'] + s['h']))
                if s.get('rotated'):
                    c = c.rotate(90, expand=True)
                self.sprites[s['name']] = (c, res)
            i += 1
        self._bounds = {}
        self._scaled = {}

    # ------------------------------------------------------------------
    def items(self, tl, idx, mat=(1, 0, 0, 1, 0, 0), ct=IDENT_CT, depth=0):
        """Atlas sprites drawn at timeline frame idx, bottom first:
        list of (sprite name, matrix, color transform)."""
        out = []
        if depth > 32:
            return out
        for frames in reversed(tl.layers):
            fr = Timeline.frame_at(frames, idx)
            if fr is None:
                continue
            for e in fr.get('E', []):
                if 'ASI' in e:
                    a = e['ASI']
                    out.append((a['N'], mmul(mat, _matrix(a)), ct))
                elif 'SI' in e:
                    si = e['SI']
                    sym = self.symbols.get(si['SN'])
                    if sym is None or sym.count == 0:
                        continue
                    st = si.get('ST', 'G')
                    ff = int(si.get('FF', 0) or 0)
                    lp = si.get('LP', 'LP')
                    if st in ('B', 'button'):
                        sub = 0
                    else:
                        sub = ff + (idx - fr['I'])
                        if lp in ('PO', 'playonce'):
                            sub = min(sub, sym.count - 1)
                        elif lp in ('SF', 'singleframe'):
                            sub = ff
                        else:
                            sub %= sym.count
                    sub = max(0, min(sub, sym.count - 1))
                    nct = ccat(_color(si.get('C')), ct)
                    if nct[3] <= 0.001 and nct[7] <= 0:
                        continue
                    out += self.items(sym, sub, mmul(mat, _matrix(si)), nct, depth + 1)
        return out

    def item_bounds(self, items):
        x0 = y0 = 1e18
        x1 = y1 = -1e18
        for name, m, ct in items:
            if name not in self.sprites:
                continue
            im, res = self.sprites[name]
            w, h = im.width / res, im.height / res
            for px, py in ((0, 0), (w, 0), (0, h), (w, h)):
                x, y = mapply(m, px, py)
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
        if x0 > x1:
            return None
        return (x0, y0, x1, y1)

    def bounds(self, tl=None):
        """flixel-animate Timeline.getWholeBounds: (x, y, w, h)."""
        tl = tl or self.main
        if tl.name in self._bounds:
            return self._bounds[tl.name]
        bb = None
        for i in range(tl.count):
            b = self.item_bounds(self.items(tl, i))
            if b is None:
                continue
            bb = b if bb is None else (min(bb[0], b[0]), min(bb[1], b[1]), max(bb[2], b[2]), max(bb[3], b[3]))
        if bb is None:
            bb = (0, 0, 1, 1)
        r = (bb[0], bb[1], bb[2] - bb[0], bb[3] - bb[1])
        self._bounds[tl.name] = r
        return r

    def label_frames(self, label):
        return self.main.labels().get(label)

    # ------------------------------------------------------------------
    def _sprite_scaled(self, name, s, nearest, ct):
        key = (name, round(s, 4), nearest, tuple(round(v, 3) for v in ct))
        r = self._scaled.get(key)
        if r is None:
            im, res = self.sprites[name]
            k = s / res
            w = max(1, int(round(im.width * k)))
            h = max(1, int(round(im.height * k)))
            sc = im.resize((w, h), Image.NEAREST if nearest else Image.LANCZOS)
            if ct != IDENT_CT:
                import numpy as np
                a = np.asarray(sc).astype(np.float32)
                for ch in range(4):
                    a[..., ch] = a[..., ch] * ct[ch] + ct[4 + ch]
                sc = Image.fromarray(np.clip(a + 0.5, 0, 255).astype(np.uint8), 'RGBA')
            r = (sc, w / (im.width / res), h / (im.height / res))
            self._scaled[key] = r
        return r

    def render(self, items, scale, ox, oy, nearest=False):
        """Render items with world->texel mapping t = (p - (ox, oy)) * scale.
        Returns (image, x, y): trimmed image and its top-left texel position,
        or None when empty."""
        b = self.item_bounds(items)
        if b is None:
            return None
        X0 = int(math.floor((b[0] - ox) * scale)) - 1
        Y0 = int(math.floor((b[1] - oy) * scale)) - 1
        X1 = int(math.ceil((b[2] - ox) * scale)) + 1
        Y1 = int(math.ceil((b[3] - oy) * scale)) + 1
        W, H = max(1, X1 - X0), max(1, Y1 - Y0)
        canvas = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        for name, m, ct in items:
            if name not in self.sprites:
                continue
            sc, kx, ky = self._sprite_scaled(name, scale, nearest, ct)
            # sprite texel (u, v) -> world: m * (u / kx, v / ky)
            # canvas pixel = (world - o) * scale - (X0, Y0)
            a = m[0] * scale / kx
            bb = m[1] * scale / kx
            c = m[2] * scale / ky
            d = m[3] * scale / ky
            tx = (m[4] - ox) * scale - X0
            ty = (m[5] - oy) * scale - Y0
            det = a * d - bb * c
            if abs(det) < 1e-9:
                continue
            # bbox of the sprite on the canvas
            pts = [(a * u + c * v + tx, bb * u + d * v + ty) for u, v in ((0, 0), (sc.width, 0), (0, sc.height), (sc.width, sc.height))]
            px0 = max(0, int(math.floor(min(p[0] for p in pts))))
            py0 = max(0, int(math.floor(min(p[1] for p in pts))))
            px1 = min(W, int(math.ceil(max(p[0] for p in pts))))
            py1 = min(H, int(math.ceil(max(p[1] for p in pts))))
            if px1 <= px0 or py1 <= py0:
                continue
            simple = abs(bb) < 1e-4 and abs(c) < 1e-4 and abs(a - 1) < 1e-3 and abs(d - 1) < 1e-3
            if simple:
                piece = sc
                px0, py0 = int(round(tx)), int(round(ty))
            else:
                # inverse affine, relative to the piece's top-left
                ia, ib, ic, idd = d / det, -c / det, -bb / det, a / det
                ttx, tty = tx - px0, ty - py0
                coeffs = (ia, ib, -(ia * ttx + ib * tty), ic, idd, -(ic * ttx + idd * tty))
                piece = sc.transform((px1 - px0, py1 - py0), Image.AFFINE, coeffs,
                                     resample=Image.NEAREST if nearest else Image.BICUBIC)
            layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
            layer.paste(piece, (px0, py0))
            canvas = Image.alpha_composite(canvas, layer)
        bb2 = canvas.getchannel('A').point(lambda v: 255 if v >= 8 else 0).getbbox()
        if bb2 is None:
            return None
        return canvas.crop(bb2), X0 + bb2[0], Y0 + bb2[1]


def item_key(items):
    return tuple((n, tuple(round(v, 2) for v in m), tuple(round(v, 3) for v in ct)) for n, m, ct in items)
