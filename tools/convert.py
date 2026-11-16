#!/usr/bin/env python3
"""Friday Night Funkin' DS asset converter.

Usage:  python3 tools/convert.py <Funkin.assets checkout> [--no-audio] [--only=chars,stages,...]

Reads the original game data (V-Slice JSON: characters, stages, levels,
songs, note styles) and assets and writes:
  nitrofiles/...          binary data loaded at runtime from NitroFS
  maxmod_data/*.wav       short sound effects (maxmod soundbank)
  source/gen_assets.c     generated metadata tables
  include/gen_assets.h

The converted assets are NOT redistributable (see the Funkin.assets license):
they are generated locally from your own copy of the game assets.
"""
import json
import math
import os
import re
import struct
import subprocess
import sys
import ctypes
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa
import sheet as SH
from sheet import Sheet, MSheet, outpath, cstr, cfloat, cid, scaled, trim, C_OUT, H_OUT, write_b16, emit_seq  # noqa
from animate import Atlas, item_key

ROOT = SH.ROOT
TOOLS = os.path.dirname(os.path.abspath(__file__))
ARGS = [a for a in sys.argv[1:] if not a.startswith('--')]
A = os.path.abspath(ARGS[0] if ARGS else os.path.join(ROOT, '..', 'funkin', 'assets_repo'))
OPTS = {a.split('=')[0]: (a.split('=')[1] if '=' in a else True) for a in sys.argv[1:] if a.startswith('--')}
DATA = os.path.join(A, 'preload', 'data')
LIBS = ['shared', 'preload', 'tutorial', 'week1', 'week2', 'week3', 'week4', 'week5', 'week6', 'week7']
BUILD = os.path.join(ROOT, 'build_tools')
os.makedirs(BUILD, exist_ok=True)


def jload(p):
    with open(p, encoding='utf-8-sig') as f:
        return json.load(f)


def ctool(name, lib=False):
    src = os.path.join(TOOLS, name + '.c')
    dst = os.path.join(BUILD, name + ('.so' if lib else ''))
    if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
        cmd = ['gcc', '-O2', src, '-o', dst]
        if lib:
            cmd[1:1] = ['-shared', '-fPIC', '-DLZ10_LIB']
        subprocess.run(cmd, check=True)
    return dst


_lz = ctypes.CDLL(ctool('lz10', lib=True))
_lz.lz10.restype = ctypes.c_long


def lz10(data):
    data = bytes(data)
    out = ctypes.create_string_buffer(len(data) * 2 + 16)
    n = _lz.lz10(data, len(data), out)
    return out.raw[:n]


def resolve(path, prefer=None, ext='.png'):
    """Paths.image: 'lib:path' or 'path' searched in the libraries."""
    libs = list(LIBS)
    if ':' in path:
        lib, path = path.split(':', 1)
        libs = [lib] + libs
    elif prefer:
        libs = [prefer] + libs
    for lib in libs:
        base = os.path.join(A, lib, 'images', path)
        if os.path.exists(base + ext) or (ext == '' and os.path.isdir(base)):
            return base
    raise FileNotFoundError(path)


def load_img(path, prefer=None):
    return Image.open(resolve(path, prefer) + '.png').convert('RGBA')


def parse_packer(txt, png):
    img = Image.open(png).convert('RGBA')
    frames = []
    for line in open(txt):
        if '=' not in line:
            continue
        name, r = line.split('=')
        x, y, w, h = map(int, r.split())
        frames.append(dict(name=name.strip(), x=x, y=y, w=w, h=h, fx=0, fy=0, fw=w, fh=h, rot=False))
    return img, frames


# ===========================================================================
# Characters (.chr)
#
# Every frame is stored relative to the sprite position x' = x - (animOffset
# - globalOffset) * scale (Bopper.getScreenPosition), in world units:
#   world = x' + p * scale    (p = point in the untrimmed flixel frame,
#                              mirrored inside the frame when flipX)
# For Animate atlases the flixel frame is the whole-timeline bounds.
# ===========================================================================
CHAR_TS = 0.165        # texels per world unit (non pixel characters)
DEAD_ANIMS = ('firstDeath', 'deathLoop', 'deathConfirm')
SKIP_ANIM = re.compile(r'^(fakeoutDeath|combo\d+|drop\d+|.*-censor|.*-explosion|burp.*|intro\d|laughCutscene|shit|bfCatch)$')


class Src:
    """An animation source: a sparrow/packer sheet or an Animate atlas."""
    cache = {}

    @classmethod
    def get(cls, kind, path):
        k = (kind, path)
        if k not in cls.cache:
            cls.cache[k] = cls(kind, path)
        return cls.cache[k]

    def __init__(self, kind, path):
        self.kind = kind
        if kind == 'atlas':
            self.atlas = Atlas(resolve(path, ext=''))
            self.bounds = self.atlas.bounds()
        else:
            base = resolve(path)
            if kind == 'packer':
                self.img, self.frames = parse_packer(base + '.txt', base + '.png')
            else:
                self.img, self.frames = parse_sparrow(base + '.xml', base + '.png')

    def anim_frames(self, prefix, indices, animtype='framelabel'):
        """Returns a list of opaque frame handles."""
        if self.kind == 'atlas':
            if animtype == 'symbol':
                tl = self.atlas.symbols.get(prefix)
                if tl is None and prefix == self.atlas.main.name:
                    tl = self.atlas.main
                if tl is None:
                    return []
                fr = [(tl, i) for i in range(tl.count)]
            else:
                lf = self.atlas.label_frames(prefix)
                if lf is None:
                    lf = next((v for k, v in self.atlas.main.labels().items() if k.strip() == prefix.strip()), None)
                if lf is None:
                    return []
                fr = [(self.atlas.main, i) for i in lf]
            if indices:
                fr = [fr[i] for i in indices if 0 <= i < len(fr)]
            return [('a', self, t, i) for t, i in fr]
        if self.kind == 'packer' and not prefix:
            sel = list(self.frames)
        else:
            sel = frames_by_prefix(self.frames, prefix)
        if indices:
            sel = [sel[i] for i in indices if 0 <= i < len(sel)]
        return [('s', self, f) for f in sel]

    def frame_size(self, h):
        if h[0] == 'a':
            return self.bounds[2], self.bounds[3]
        return h[2]['fw'], h[2]['fh']


def frame_key(h):
    if h[0] == 'i':
        return ('i', h[1])
    if h[0] == 'a':
        src, tl, i = h[1], h[2], h[3]
        return ('a', id(src), item_key(src.atlas.items(tl, i)))
    f = h[2]
    return ('s', id(h[1]), f['x'], f['y'], f['w'], f['h'], f['fx'], f['fy'], f['rot'], f['fw'])


def render_frame(h, scale, ts, flip, nearest):
    """-> (img, dx, dy, ww, wh) with (dx, dy) world offset of the top-left."""
    k = scale * ts       # texels per frame pixel
    if h[0] == 'i':      # pre-rendered: (tag, key, img, dx, dy, ww, wh)
        return h[2], h[3], h[4], h[5], h[6]
    if h[0] == 'a':
        src, tl, i = h[1], h[2], h[3]
        b = src.bounds
        r = src.atlas.render(src.atlas.items(tl, i), k, b[0], b[1], nearest=nearest)
        if r is None:
            return None
        im, X, Y = r
        px, py = X / k, Y / k                    # in frame pixels
        pw, ph = im.width / k, im.height / k
        fw = b[2]
    else:
        f = h[2]
        c = crop_frame(h[1].img, f)
        px, py = -f['fx'], -f['fy']
        bb = c.getchannel('A').point(lambda v: 255 if v >= 8 else 0).getbbox()
        if bb is None:
            return None
        c = c.crop(bb)
        px, py = px + bb[0], py + bb[1]
        pw, ph = c.width, c.height
        im = c.resize((max(1, int(round(c.width * k))), max(1, int(round(c.height * k)))),
                      Image.NEAREST if nearest else Image.LANCZOS)
        fw = f['fw']
    if flip:
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
        px = fw - (px + pw)
    return im, px * scale, py * scale, pw * scale, ph * scale


CHR_INFO = {}


def char_data(cid_):
    d = jload(os.path.join(DATA, 'characters', cid_ + '.json'))
    d.setdefault('scale', 1.0)
    d.setdefault('offsets', [0, 0])
    d.setdefault('cameraOffsets', [0, 0])
    d.setdefault('singTime', 8.0)
    d.setdefault('danceEvery', 1)
    d.setdefault('flipX', False)
    d.setdefault('isPixel', False)
    d.setdefault('renderType', 'sparrow')
    return d


def convert_character(cid_, player):
    """Writes chr/<cid>[_p].chr and chr/<cid>[_p]_dead.chr (death animations)."""
    d = char_data(cid_)
    rt = d['renderType']
    scale = float(d['scale'])
    flip = (not d['flipX']) if player else bool(d['flipX'])
    pixel = bool(d['isPixel'])
    ts = (1.0 / scale) if pixel else CHAR_TS
    kind = {'sparrow': 'sparrow', 'multisparrow': 'sparrow', 'packer': 'packer',
            'animateatlas': 'atlas', 'multianimateatlas': 'atlas'}[rt]
    multi = rt.startswith('multi')
    anims = []
    for an in d['animations']:
        if SKIP_ANIM.match(an['name']):
            continue
        path = an.get('assetPath') if multi and an.get('assetPath') else d['assetPath']
        akind = kind
        if multi and an.get('assetPath') and kind == 'atlas':
            try:
                resolve(path, ext='')
            except FileNotFoundError:
                akind = 'sparrow'
        try:
            src = Src.get(akind, path)
        except FileNotFoundError:
            print('  %s: missing asset %s' % (cid_, path))
            continue
        hs = src.anim_frames(an.get('prefix', ''), an.get('frameIndices'), an.get('animType', 'framelabel'))
        if not hs:
            print('  %s: no frames for %s (%s)' % (cid_, an['name'], an.get('prefix')))
            continue
        off = an.get('offsets') or [0, 0]
        anims.append(dict(name=an['name'], fps=float(an.get('frameRate', 24)), loop=bool(an.get('looped', False)),
                          offx=-(off[0] - d['offsets'][0]) * scale, offy=-(off[1] - d['offsets'][1]) * scale,
                          frames=hs, src=src))
    names = [a['name'] for a in anims]
    first = 'danceLeft' if 'danceLeft' in names else 'idle'
    if first not in names:
        first = names[0]
    a0 = anims[names.index(first)]
    fw0, fh0 = a0['src'].frame_size(a0['frames'][0])
    W, H = fw0 * scale, fh0 * scale
    tag = cid_ + ('_p' if player else '')
    live = [a for a in anims if a['name'] not in DEAD_ANIMS]
    dead = [a for a in anims if a['name'] in DEAD_ANIMS]
    info = dict(W=W, H=H, gx=d['offsets'][0], gy=d['offsets'][1], camx=d['cameraOffsets'][0],
                camy=d['cameraOffsets'][1], sing=float(d['singTime']), dance=float(d['danceEvery']),
                pixel=pixel, scale=scale, flip=flip, icon=(d.get('healthIcon') or {}).get('id', cid_),
                iconPixel=bool((d.get('healthIcon') or {}).get('isPixel', pixel)), dead=bool(dead),
                deathCam=(d.get('death') or {}).get('cameraOffsets', [0, 0]))
    if not pixel:
        ts = fit_ts(live, scale, ts)
    write_chr(tag, live, info, scale, ts, flip, pixel)
    if dead:
        write_chr(tag + '_dead', dead, info, scale, fit_ts(dead, scale, CHAR_TS) if not pixel else ts, flip, pixel)
    CHR_INFO[tag] = info
    return info


def fit_ts(anims, scale, ts, max_area=128 * 160):
    """Lower the texture scale so that the VRAM slot stays small."""
    mw = mh = 1
    for a in anims:
        for h in a['frames']:
            fw, fh = a['src'].frame_size(h)
            if h[0] == 's':
                c = crop_frame(h[1].img, h[2]).getchannel('A').getbbox() or (0, 0, 1, 1)
                fw, fh = c[2] - c[0], c[3] - c[1]
            else:
                b = h[1].atlas.item_bounds(h[1].atlas.items(h[2], h[3]))
                if b:
                    fw, fh = b[2] - b[0], b[3] - b[1]
            mw, mh = max(mw, fw * scale * ts), max(mh, fh * scale * ts)
    k = max(min(1.0, cw / mw, ch / mh) for cw, ch in ((126, 126), (254, 126), (126, 254)))
    return ts * k


def write_chr(tag, anims, info, scale, ts, flip, pixel):
    uniq = {}
    frames = []          # (img, dx, dy, ww, wh)
    adefs = []
    for a in anims:
        ids = []
        for h in a['frames']:
            k = frame_key(h)
            if k not in uniq:
                r = render_frame(h, scale, ts, flip, pixel)
                if r is None:
                    r = (Image.new('RGBA', (4, 1), (0, 0, 0, 0)), 0, 0, 0.01, 0.01)
                uniq[k] = len(frames)
                frames.append(r)
            ids.append(uniq[k])
        adefs.append((a, ids))
    palimg, colors = build_palette([f[0] for f in frames], 255)
    pal = pal_bytes(colors, 256)
    pix = bytearray()
    fdefs = []
    maxw = maxh = 8
    for im, dx, dy, ww, wh in frames:
        w4 = (im.width + 3) & ~3
        pad = Image.new('RGBA', (w4, im.height), (0, 0, 0, 0))
        pad.paste(im, (0, 0))
        idx = index_image(pad, palimg)
        comp = lz10(idx)
        fdefs.append((len(pix), len(comp), w4, im.height, im.width, 0, dx, dy, ww, wh))
        pix += comp
        maxw, maxh = max(maxw, w4), max(maxh, im.height)
    seq = []
    arecs = []
    for a, ids in adefs:
        arecs.append((a['name'], a['fps'], a['loop'], len(ids), len(seq), a['offx'], a['offy']))
        seq += ids
    # layout
    HDR = struct.Struct('<4sfBBBBffffffffHHIIIII')
    ANIM = struct.Struct('<24sfBBHHHff')
    FRM = struct.Struct('<IIHHHHffff')
    off_anims = HDR.size
    off_seq = off_anims + ANIM.size * len(arecs)
    off_frames = (off_seq + 2 * len(seq) + 3) & ~3
    off_pal = off_frames + FRM.size * len(fdefs)
    off_pix = off_pal + 512
    out = bytearray(HDR.pack(b'CHR1', ts, log2size(maxw), log2size(maxh), 1 if pixel else 0, 1 if flip else 0,
                             info['W'], info['H'], info['gx'], info['gy'], info['camx'], info['camy'], info['sing'],
                             info['dance'], len(fdefs), len(arecs), off_anims, off_seq, off_frames, off_pal, off_pix))
    for n, fps, loop, nf, st, ox, oy in arecs:
        out += ANIM.pack(n.encode()[:23], fps, 1 if loop else 0, 0, nf, st, 0, ox, oy)
    for v in seq:
        out += struct.pack('<H', v)
    while len(out) < off_frames:
        out.append(0)
    for f in fdefs:
        out += FRM.pack(*f)
    out += pal
    out += pix
    with open(outpath('chr', tag + '.chr'), 'wb') as fo:
        fo.write(out)
    print('chr %-22s %3d frames %2d anims slot %dx%d  file %6d KB' % (tag, len(fdefs), len(arecs), 8 << log2size(maxw),
                                                                     8 << log2size(maxh), len(out) // 1024))


# ===========================================================================
# Stages
# ===========================================================================
KDS = 192.0 / 720.0
STAGE_BUDGET = 156 * 1024     # VRAM bytes for the stage props
STAGES = []


def color_hex(s, default=0xFFFFFF):
    if not s:
        return default
    s = s.strip().lstrip('#')
    if s.lower().startswith('0x'):
        s = s[2:]
    if len(s) == 8:
        s = s[2:]
    try:
        return int(s, 16)
    except ValueError:
        return {'white': 0xFFFFFF, 'black': 0, 'red': 0xFF0000}.get(s.lower(), default)


def rgb15c(c):
    return rgb15((c >> 16) & 255, (c >> 8) & 255, c & 255)


def prop_frames(p, lib):
    """-> (src, {anim: (handles, fps, loop, offx, offy)}) for animated props."""
    kind = {'packer': 'packer', 'animateatlas': 'atlas'}.get(p.get('animType'), 'sparrow')
    path = p['assetPath']
    if ':' not in path:
        try:
            resolve(path, lib, ext='' if kind == 'atlas' else '.png')
            path = lib + ':' + path
        except FileNotFoundError:
            pass
    src = Src.get(kind, path)
    out = {}
    for an in p['animations']:
        if an['name'].endswith('-scared'):
            continue
        if kind == 'packer':
            hs = [('s', src, src.frames[i]) for i in an.get('frameIndices') or [] if i < len(src.frames)]
        else:
            hs = src.anim_frames(an.get('prefix', ''), an.get('frameIndices'), an.get('animType', 'framelabel'))
        if not hs:
            print('  prop %s: no frames for %s' % (p['name'], an['name']))
            continue
        off = an.get('offsets') or [0, 0]
        out[an['name']] = (hs, float(an.get('frameRate', 24)), bool(an.get('looped', False)), off[0], off[1])
    return src, out


NOCROP = {('phillyTrain', 'train'), ('limoRide', 'fastCar')}
PROP_SKIP = {('school', 'petals')}
PROP_C16 = {('spookyMansion', 'halloweenBG'), ('school', 'freaks'), ('school', 'petals'), ('school', 'treesBG')}
PROP_STRIDE = {('spookyMansion', 'halloweenBG'): 10, ('school', 'treesBG'): 19, ('school', 'freaks'): 3}


def crop_window(r, ts, w, px, py):
    """Crop a rendered frame (img, dx, dy, ww, wh) placed at world (px+dx, py+dy)
    to the world window w."""
    im, dx, dy, ww, wh = r
    x0 = px + dx
    y0 = py + dy
    kx = im.width / max(ww, 1e-6)
    ky = im.height / max(wh, 1e-6)
    a = max(0, int(math.floor((w[0] - x0) * kx)))
    b = max(0, int(math.floor((w[1] - y0) * ky)))
    c = min(im.width, int(math.ceil((w[2] - x0) * kx)))
    e = min(im.height, int(math.ceil((w[3] - y0) * ky)))
    if c <= a or e <= b:
        return None
    if (a, b, c, e) == (0, 0, im.width, im.height):
        return r
    return im.crop((a, b, c, e)), dx + a / kx, dy + b / ky, (c - a) / kx, (e - b) / ky


def convert_stage(sid):
    d = jload(os.path.join(DATA, 'stages', sid + '.json'))
    bsid = sid.replace('Erect', '')      # the erect stages share the base stage overrides
    lib = d.get('directory', 'shared')
    zoom = float(d.get('cameraZoom', 1.0))
    # world area the camera can show (stage camera focus range, zoomed out a bit)
    ch = d['characters']
    fx, fy = [], []
    for k in ('bf', 'gf', 'dad'):
        c = ch.get(k)
        if not c:
            continue
        co = c.get('cameraOffsets', [0, 0])
        tags = [t for (st, role, t) in STAGE_CHARS if st == sid and role == k] or [None]
        for t in tags:
            info = CHR_INFO.get(t)
            if info:   # BaseCharacter.cameraFocusPoint
                fx.append(c['position'][0] + info['gx'] + info['camx'] + co[0])
                fy.append(c['position'][1] - info['H'] / 2 + info['gy'] + info['camy'] + co[1])
            else:
                fx.append(c['position'][0] + co[0])
                fy.append(c['position'][1] - 300 + co[1])
    cx0, cx1 = min(fx) - 120, max(fx) + 120
    cy0, cy1 = min(fy) - 120, max(fy) + 120
    zmin = zoom * 0.8 * 0.8      # DS_VIEW in play.c

    def window(q):
        if (bsid, q['name']) in NOCROP:
            return (-1e9, -1e9, 1e9, 1e9)
        sfx, sfy = q['sfx'], q['sfy']
        return ((cx0 - 640) * sfx + 640 - 480 / zmin, (cy0 - 360) * sfy + 360 - 360 / zmin,
                (cx1 - 640) * sfx + 640 + 480 / zmin, (cy1 - 360) * sfy + 360 + 360 / zmin)
    props = []
    for p in d['props']:
        if (bsid, p['name']) in PROP_SKIP or p['name'].endswith('_lightmap') or \
                p['name'] in ('foregroundMultiply', 'additionalLighten', 'fog'):
            continue   # blend-mode overlays (add / multiply) the DS cannot draw
        sc = p.get('scale', [1, 1])
        if not isinstance(sc, list):
            sc = [sc, sc]
        scr = p.get('scroll', [1, 1])
        q = dict(name=p['name'], x=float(p['position'][0]), y=float(p['position'][1]), sx=float(sc[0]), sy=float(sc[1]),
                 sfx=float(scr[0]), sfy=float(scr[1]), z=int(p.get('zIndex', 0) or 0), alpha=float(p.get('alpha', 1)),
                 dance=float(p.get('danceEvery', 0) or 0), pixel=bool(p.get('isPixel', False)),
                 color=color_hex(p.get('color'), 0xFFFFFF), start=p.get('startingAnimation'),
                 flip=bool(p.get('flipX', False)))
        if p['assetPath'].startswith('#'):
            q['kind'] = 'solid'
            q['color'] = color_hex(p['assetPath'], 0)
            q['w'], q['h'] = sc[0], sc[1]
        elif p.get('animations'):
            q['kind'] = 'anim'
            q['src'], q['anims'] = prop_frames(p, lib)
            if not q['anims']:
                continue
            stride = PROP_STRIDE.get((bsid, p['name']), 1)
            if stride > 1:
                q['anims'] = {k: (v[0][::stride], v[1] / stride) + v[2:] for k, v in q['anims'].items()}
        else:
            q['kind'] = 'static'
            try:
                q['img'] = load_img(p['assetPath'], lib)
            except FileNotFoundError:
                print('  stage %s: missing %s' % (sid, p['assetPath']))
                continue
            ang = float(p.get('angle', 0) or 0)
            if ang:
                im0 = q['img']
                r = im0.rotate(-ang, resample=Image.BICUBIC, expand=True)
                q['x'] -= (r.width - im0.width) / 2 * q['sx']
                q['y'] -= (r.height - im0.height) / 2 * q['sy']
                q['img'] = r
        if (bsid, q['name']) in PROP_C16:
            q['c16'] = True
        elif q['pixel'] and q['kind'] != 'solid':
            im0 = q['img'] if q['kind'] == 'static' else getattr(q['src'], 'img', None)
            if im0 is not None:
                cols = im0.getcolors(4096)
                q['c16'] = cols is not None and len([c for c in cols if c[1][3] >= 100]) <= 15
        props.append(q)
    # texture scale: fit the VRAM budget
    def frame_list(q):
        seen = {}
        for an, (hs, *_r) in q['anims'].items():
            for h in hs:
                seen.setdefault(frame_key(h), h)
        return list(seen.values())

    def est(q, ts):
        if q['kind'] == 'static':
            t, (bx, by) = trim(q['img'])
            w = window(q)
            x0, y0 = max(w[0], q['x'] + bx * q['sx']), max(w[1], q['y'] + by * q['sy'])
            x1, y1 = min(w[2], q['x'] + (bx + t.width) * q['sx']), min(w[3], q['y'] + (by + t.height) * q['sy'])
            return max(0, x1 - x0) * ts * max(0, y1 - y0) * ts * 1.15 * (0.5 if q.get('c16') else 1)
        if q['kind'] == 'anim':
            tot = 0
            w = window(q)
            for h in q.setdefault('flist', frame_list(q)):
                if h[0] == 's':
                    fw, fh = h[2]['w'], h[2]['h']
                else:
                    b = h[1].atlas.item_bounds(h[1].atlas.items(h[2], h[3])) or (0, 0, 1, 1)
                    fw, fh = b[2] - b[0], b[3] - b[1]
                ww = min(fw * q['sx'], w[2] - w[0])
                hh = min(fh * q['sy'], w[3] - w[1])
                tot += ww * ts * hh * ts * 0.5 * (0.5 if q.get('c16') else 1)
            return tot
        return 0
    qual = 0.85
    while True:
        tsd = {}
        tot = 0
        for q in props:
            if q['kind'] == 'solid':
                continue
            if q['pixel']:
                ts = 1.0 / q['sx']
            else:
                ts = KDS * zoom * 0.8 * qual
            tsd[q['name']] = ts
            tot += est(q, ts)
        if tot <= STAGE_BUDGET or qual < 0.2:
            if '--verbose' in OPTS:
                for q in props:
                    if q['kind'] != 'solid':
                        print('   %-16s %6d' % (q['name'], est(q, tsd[q['name']])), len(q.get('flist', [])))
            break
        qual *= 0.92
    print('stage %s: quality %.2f, estimated %d KB' % (sid, qual, tot // 1024))
    def build(tsd):
        c0, h0 = len(C_OUT), len(H_OUT)
        vram = 0
        shared = {}
        cprops = []
        for q in props:
            nm = cid(sid + '_' + q['name']).lower()
            anims = []
            sheetname = 'NULL'
            ts = tsd.get(q['name'], 1)
            if q['kind'] == 'static':
                img0 = q['img']
                if q['flip']:
                    img0 = img0.transpose(Image.FLIP_LEFT_RIGHT)
                w = window(q)
                # crop to the visible window (in image pixels)
                ix0 = max(0, int(math.floor((w[0] - q['x']) / q['sx'])))
                iy0 = max(0, int(math.floor((w[1] - q['y']) / q['sy'])))
                ix1 = min(img0.width, int(math.ceil((w[2] - q['x']) / q['sx'])))
                iy1 = min(img0.height, int(math.ceil((w[3] - q['y']) / q['sy'])))
                if ix1 <= ix0 or iy1 <= iy0:
                    print('  prop %s never visible' % q['name'])
                    continue
                t, (bx, by) = trim(img0.crop((ix0, iy0, ix1, iy1)))
                bx += ix0
                by += iy0
                tw = max(1, int(round(t.width * q['sx'] * ts)))
                th = max(1, int(round(t.height * q['sy'] * ts)))
                im = t.resize((tw, th), Image.NEAREST if q['pixel'] else Image.LANCZOS)
                sh = Sheet('st_' + nm, ts, fmt=FMT_16COL if q.get('c16') else FMT_256COL)
                sh.add_tiled('main', im, 0, 0)
                vram += sh.build().vram
                sheetname = '&sheet_st_' + nm
                q['x'] += bx * q['sx']
                q['y'] += by * q['sy']
            elif q['kind'] == 'anim':
                key = (id(q['src']), tuple(sorted((k, len(v[0])) for k, v in q['anims'].items())), q['sx'], round(ts, 4))
                if key in shared:
                    sheetname, fidx = shared[key]
                else:
                    sh = Sheet('st_' + nm, ts, fmt=FMT_16COL if q.get('c16') else FMT_256COL)
                    fidx = {}
                    for h in q['flist']:
                        r = render_frame(h, q['sx'], ts, q['flip'], q['pixel'])
                        k = frame_key(h)
                        if r is not None:
                            r = crop_window(r, ts, window(q), q['x'], q['y'])
                        if r is None:
                            r = (Image.new('RGBA', (1, 1), (0, 0, 0, 0)), 0, 0, 0, 0)
                        fidx[k] = len(sh.items)
                        sh.add_packed('f%d' % len(sh.items), r[0], int(round(r[1] * ts)), int(round(r[2] * ts)))
                    vram += sh.build().vram
                    sheetname = '&sheet_st_' + nm
                    shared[key] = (sheetname, fidx)
                for an, (hs, fps, loop, ox, oy) in q['anims'].items():
                    seqn = 'st_%s_%s' % (nm, cid(an).lower())
                    C_OUT.append('static const u8 %s[] = {%s};' % (seqn, ','.join(str(fidx[frame_key(h)]) for h in hs)))
                    anims.append('{%s,%s,%d,%d,%s,%s,%s}' % (cstr(an), seqn, len(hs), 1 if loop else 0, cfloat(fps),
                                                           cfloat(-ox * q['sx']), cfloat(-oy * q['sy'])))
            start = 0
            if q['kind'] == 'anim':
                names = list(q['anims'].keys())
                if q['start'] in names:
                    start = names.index(q['start'])
            if anims:
                C_OUT.append('static const PropAnim st_%s_anims[] = {%s};' % (nm, ','.join(anims)))
            cprops.append('{%s,%s,%d,%s,%s,%s,%s,%s,%s,0x%04X,%s,%d,%s,%d,%s,%d,%d}' % (
                cstr(q['name']), sheetname, {'static': 0, 'anim': 1, 'solid': 2}[q['kind']], cfloat(q['x']), cfloat(q['y']),
                cfloat(q['sfx']), cfloat(q['sfy']), cfloat(q.get('w', 0)), cfloat(q.get('h', 0)), rgb15c(q['color']),
                cfloat(q['alpha']), q['z'], cfloat(q['dance']), len(anims), ('st_%s_anims' % nm) if anims else 'NULL', start,
                1 if q['pixel'] else 0))
        return cprops, vram, c0, h0

    for it in range(5):
        cprops, vram, c0, h0 = build(tsd)
        k = math.sqrt(STAGE_BUDGET / vram) * 0.97
        if STAGE_BUDGET * 0.85 <= vram <= STAGE_BUDGET * 1.05 or it == 4 or (k > 1 and qual >= 1.0) or (k < 1 and qual < 0.2) \
                or all(q['pixel'] or q['kind'] == 'solid' for q in props):
            break
        k = min(k, 1.0 / qual)
        del C_OUT[c0:]
        del H_OUT[h0:]
        qual *= k
        tsd = {n: (t if next(q for q in props if q['name'] == n)['pixel'] else t * k) for n, t in tsd.items()}
    print('stage %s: quality %.2f, %d KB vram' % (sid, qual, vram // 1024))
    def cv(k, f, dflt):
        v = ch.get(k, {}).get(f, dflt)
        return v
    cdefs = []
    for k in ('bf', 'gf', 'dad'):
        pos = cv(k, 'position', [0, 0])
        co = cv(k, 'cameraOffsets', [0, 0])
        scr = cv(k, 'scroll', [1, 1])
        cdefs.append('{%s,%s,%s,%s,%d,%s,%s,%s}' % (cfloat(pos[0]), cfloat(pos[1]), cfloat(co[0]), cfloat(co[1]),
                                                   int(cv(k, 'zIndex', 0)), cfloat(cv(k, 'scale', 1)), cfloat(scr[0]), cfloat(scr[1])))
    C_OUT.append('static const PropDef stage_%s_props[] = {%s};' % (cid(sid).lower(), ','.join(cprops)))
    C_OUT.append('const StageDef stage_%s = {%s, %s, {%s}, %d, stage_%s_props};' % (
        cid(sid).lower(), cstr(sid), cfloat(zoom), ','.join(cdefs), len(cprops), cid(sid).lower()))
    H_OUT.append('extern const StageDef stage_%s;' % cid(sid).lower())
    STAGES.append(sid)


# ===========================================================================
# Charts (.cht), weeks and songs
# ===========================================================================
EASES = ['linear', 'quad', 'cube', 'quart', 'quint', 'sine', 'circ', 'expo', 'back', 'smoothStep', 'smootherStep',
         'elastic', 'bounce']
EV_TYPES = ['FocusCamera', 'ZoomCamera', 'SetCameraBop', 'PlayAnimation', 'ScrollSpeed']
NOTE_KINDS = {'noanim': 1, 'mom': 2, 'ugh': 3, 'hehPrettyGood': 4}
WEEKS = ['tutorial', 'week1', 'week2', 'week3', 'week4', 'week5', 'week6', 'week7', 'weekend1']
DIFFS = ['easy', 'normal', 'hard', 'erect', 'nightmare']
EXTRA_SONGS = ['spaghetti']       # songs that are not in a week (freeplay only)


def ease_code(ease, d):
    if ease is None:
        ease = 'linear'
    if ease == 'CLASSIC':
        return 255
    if ease == 'INSTANT':
        return 254
    m = re.match(r'^([a-zA-Z]+?)(In|Out|InOut)?$', ease)
    base, dr = m.group(1), m.group(2) or d or 'In'
    if base not in EASES:
        return 0
    i = EASES.index(base)
    return 0 if i == 0 else i * 3 + {'In': 0, 'Out': 1, 'InOut': 2}[dr]


KINDS = ['']


def kind_index(k):
    k = k or ''
    if k in ('', 'normal'):
        return 0
    if k not in KINDS:
        KINDS.append(k)
    return KINDS.index(k)


def convert_chart(song, var, tag, diff, meta):
    d = jload(os.path.join(DATA, 'songs', song, '%s-chart%s.json' % (song, '-' + var if var else '')))
    notes = d['notes'].get(diff)
    if notes is None:
        return False
    speed = d['scrollSpeed'].get(diff, d['scrollSpeed'].get('default', 1.0))
    times = [(float(t['t']), float(t['bpm'])) for t in meta['timeChanges']]
    evs = []
    for e in sorted(d['events'], key=lambda e: e['t']):
        v = e.get('v')
        n = e['e']
        f = [0.0, 0.0, 0.0, 0.0]
        i0 = 0
        ease = 255
        st = b''
        if n == 'FocusCamera':
            if not isinstance(v, dict):
                v = {'char': v}
            i0 = int(v.get('char', 0))
            f = [float(v.get('x', 0)), float(v.get('y', 0)), float(v.get('duration', 4)), 0]
            ease = ease_code(v.get('ease', 'CLASSIC'), v.get('easeDir'))
        elif n == 'ZoomCamera':
            f = [float(v.get('zoom', 1)), float(v.get('duration', 4)), 0, 0]
            i0 = 1 if v.get('mode', 'stage') == 'direct' else 0
            ease = ease_code(v.get('ease', 'linear'), v.get('easeDir'))
        elif n == 'SetCameraBop':
            f = [float(v.get('rate', 4)), float(v.get('intensity', 1)), float(v.get('offset', 0)), 0]
        elif n == 'PlayAnimation':
            tgt = v.get('target', 'bf')
            i0 = {'boyfriend': 0, 'bf': 0, 'player': 0, 'dad': 1, 'opponent': 1, 'girlfriend': 2, 'gf': 2}.get(tgt, 3)
            st = v.get('anim', 'idle').encode()[:15]
            f = [1.0 if v.get('force') else 0.0, 0, 0, 0]
        elif n == 'ScrollSpeed':
            f = [float(v.get('scroll', 1)), float(v.get('duration', 4)), 0, 0]
            i0 = 1 if v.get('absolute') else 0
            ease = ease_code(v.get('ease', 'linear'), v.get('easeDir'))
        else:
            continue
        evs.append(struct.pack('<fBbBBffff16s', float(e['t']), EV_TYPES.index(n), i0, ease, 0, *f, st))
    lanes = [[], []]
    for nt in notes:
        dd = int(nt['d'])
        if dd < 0 or dd > 7:
            continue
        side = 0 if dd < 4 else 1
        k = kind_index(nt.get('k'))
        lanes[side].append((float(nt['t']), float(nt.get('l', 0) or 0), dd & 3, k))
    for l in lanes:
        l.sort()
    out = bytearray(struct.pack('<4sfHHII', b'CHT1', speed, len(times), len(evs), len(lanes[0]), len(lanes[1])))
    for t, b in times:
        out += struct.pack('<ff', t, b)
    for e in evs:
        out += e
    for l in lanes:
        for t, ln, dd, k in l:
            out += struct.pack('<ffBBH', t, ln, dd, k, 0)
    with open(outpath('song', '%s-%s.cht' % (tag, diff)), 'wb') as fo:
        fo.write(out)
    return True


SONGS = []        # dicts
STAGE_CHARS = []  # (stage, role, chr tag)
NEED_CHARS = {}   # tag -> (cid, player)


PLAYERS = ['bf', 'pico']


def song_entry(sid, var, week):
    tag = sid + ('-' + var if var else '')
    meta = jload(os.path.join(DATA, 'songs', sid, '%s-metadata%s.json' % (sid, '-' + var if var else '')))
    pd = meta['playData']
    c = pd['characters']
    mask = 0
    for i, df in enumerate(DIFFS):
        if df in pd['difficulties'] and convert_chart(sid, var, tag, df, meta):
            mask |= 1 << i
    info = dict(id=sid, tag=tag, var=var, name=meta['songName'], artist=meta.get('artist', ''), stage=pd['stage'],
                style=pd.get('noteStyle', 'funkin'), player=c.get('player', 'bf'), gf=c.get('girlfriend') or '',
                opp=c.get('opponent', 'dad'), mask=mask, week=week, bpm=meta['timeChanges'][0]['bpm'],
                inst=c.get('instrumental', ''), pvoc=c.get('playerVocals') or [], ovoc=c.get('opponentVocals') or [],
                vars=pd.get('songVariations') or [])
    NEED_CHARS[info['player'] + '_p'] = (info['player'], True)
    NEED_CHARS[info['opp']] = (info['opp'], False)
    if info['gf']:
        NEED_CHARS[info['gf']] = (info['gf'], False)
    SONGS.append(info)
    STAGE_CHARS.append((info['stage'], 'bf', info['player'] + '_p'))
    STAGE_CHARS.append((info['stage'], 'dad' if info['gf'] else 'gf', info['opp']))
    if info['gf']:
        STAGE_CHARS.append((info['stage'], 'gf', info['gf']))
    print('song %-24s %-24s %s/%s/%s  diffs %d' % (tag, pd['stage'], info['player'], info['gf'], info['opp'], mask))
    return len(SONGS) - 1


def collect_songs():
    weeks = []
    order = []
    for w in WEEKS:
        lv = jload(os.path.join(DATA, 'levels', w + '.json'))
        ids = []
        for s in lv['songs']:
            ids.append(song_entry(s, '', len(weeks)))
            order.append((s, len(weeks)))
        weeks.append(dict(id=w, name=lv['name'], songs=ids, title=lv.get('titleAsset'), props=lv.get('props', []),
                          bg=color_hex(lv.get('background', '#F9CF51'))))
    for s in EXTRA_SONGS:
        if os.path.exists(os.path.join(DATA, 'songs', s)):
            order.append((s, len(weeks)))
            song_entry(s, '', len(weeks))
    # variations (erect / pico / bf mixes)
    for s, w in order:
        base = next(x for x in SONGS if x['id'] == s and x['var'] == '')
        for v in base['vars']:
            if os.path.exists(os.path.join(DATA, 'songs', s, '%s-metadata-%s.json' % (s, v))):
                song_entry(s, v, w)
    # freeplay: per song, the (entry, difficulty) options of each playable character
    owned = {p: set(jload(os.path.join(DATA, 'players', p + '.json'))['ownedChars']) for p in PLAYERS}
    for s, w in order:
        opts = {p: [] for p in PLAYERS}
        for i, e in enumerate(SONGS):
            if e['id'] != s:
                continue
            p = next((q for q in PLAYERS if e['player'] in owned[q]), 'bf')
            for d in range(len(DIFFS)):
                if e['mask'] & (1 << d):
                    opts[p].append((i, d))
        FREEPLAY.append(dict(name=next(x['name'] for x in SONGS if x['id'] == s), week=w, opts=opts))
    return weeks


FREEPLAY = []


def emit_kind_anims():
    """Note kinds handled by the character scripts (preload/scripts/characters/*.hxc):
    case "<kind>": playAnimation('x') / someFunction() -> playAnimation('x')."""
    rows = []
    SD = os.path.join(A, 'preload', 'scripts', 'characters')
    for f in sorted(os.listdir(SD)):
        chrid = f[:-4]
        if not any(t == chrid or t == chrid + '_p' for t in CHR_INFO):
            continue
        src = open(os.path.join(SD, f)).read()
        funcs = {}
        for m in re.finditer(r'function (\w+)\([^)]*\)[^{]*\{(.*?)\n  \}', src, re.S):
            funcs[m.group(1)] = re.findall(r"playAnimation\('([^']+)'", m.group(2))
        res = {}
        for sect, slot in (('onNoteHit', 0), ('onNoteMiss', 1)):
            m = re.search(r'function ' + sect + r'\(.*?\n  \}', src, re.S)
            if not m:
                continue
            body = m.group(0)
            for c in re.finditer(r'case "([\w-]+)":(.*?)(?=\n\s*case |\n\s*default|\n    \})', body, re.S):
                kind = c.group(1)
                if kind not in KINDS:
                    continue
                anims = re.findall(r"playAnimation\('([^']+)'", c.group(2))
                if not anims:
                    for fn in re.findall(r'(\w+)\(\);', c.group(2)):
                        if funcs.get(fn):
                            anims = funcs[fn]
                            break
                if anims:
                    res.setdefault(kind, ['', ''])[slot] = anims[0]
        for kind, (hit, miss) in res.items():
            rows.append('{%s,%d,%s,%s}' % (cstr(chrid), KINDS.index(kind), cstr(hit), cstr(miss)))
    C_OUT.append('const KindAnim kind_anims[] = {%s};\nconst int kind_anim_count = %d;' % (','.join(rows) or '{0}', len(rows)))
    H_OUT.append('extern const KindAnim kind_anims[];\nextern const int kind_anim_count;')
    print('kind animations:', len(rows))


def emit_songs(weeks, stage_ids):
    H_OUT.append('#define SONG_COUNT %d\n#define WEEK_COUNT %d\n#define DIFF_COUNT %d' % (len(SONGS), len(weeks), len(DIFFS)))
    rows = []
    for s in SONGS:
        rows.append('{%s,%s,%s,%s,&stage_%s,%d,%s,%s,%s,%s,%s,%d,%d}' % (
            cstr(s['id']), cstr(s['tag']), cstr(s['name']), cstr(s['artist']), cid(s['stage']).lower(), 1 if s['style'] == 'pixel' else 0,
            cstr(s['player'] + '_p'), cstr(s['gf']), cstr(s['opp']), cstr(CHR_INFO[s['player'] + '_p']['icon']),
            cstr(CHR_INFO[s['opp']]['icon']), s['mask'], s['week']))
    C_OUT.append('const SongInfo song_info[SONG_COUNT] = {%s};' % ',\n'.join(rows))
    rows = []
    for i, w in enumerate(weeks):
        rows.append('{%s,%s,%d,{%s},0x%06X}' % (cstr(w['id']), cstr(w['name']), len(w['songs']),
                                               ','.join(str(x) for x in w['songs']), w['bg']))
    C_OUT.append('const WeekInfo week_info[WEEK_COUNT] = {%s};' % ',\n'.join(rows))
    rows = []
    for f in FREEPLAY:
        o = [f['opts'][p][:12] for p in PLAYERS]
        rows.append('{%s,%d,{%d,%d},{%s}}' % (cstr(f['name']), f['week'], len(o[0]), len(o[1]), ','.join(
            '{%s}' % ','.join(str((i << 3) | d) for i, d in x) if x else '{0}' for x in o)))
    H_OUT.append('#define FREEPLAY_COUNT %d' % len(FREEPLAY))
    C_OUT.append('const FreeplaySong freeplay_songs[FREEPLAY_COUNT] = {%s};' % ',\n'.join(rows))
    H_OUT.append('extern const FreeplaySong freeplay_songs[FREEPLAY_COUNT];')
    C_OUT.append('const char *const note_kinds[] = {%s};' % ','.join(cstr(k) for k in KINDS))
    H_OUT.append('#define NOTE_KIND_COUNT %d\nextern const char *const note_kinds[];' % len(KINDS))
    H_OUT.append('extern const SongInfo song_info[SONG_COUNT];\nextern const WeekInfo week_info[WEEK_COUNT];')
    # character table (positions / camera data used before the .chr is loaded)
    rows = []
    for tag, info in sorted(CHR_INFO.items()):
        rows.append('{%s,%s,%d}' % (cstr(tag), cstr(info['icon']), 1 if info['dead'] else 0))
    C_OUT.append('const ChrInfo chr_info[] = {%s};\nconst int chr_info_count = %d;' % (','.join(rows), len(rows)))
    H_OUT.append('extern const ChrInfo chr_info[];\nextern const int chr_info_count;')


# ===========================================================================
# Audio: songs are IMA ADPCM streams (tools/adpcm.c), sound effects are WAVs
# ===========================================================================
RATE = 22050
RAW = ['-ac', '1', '-ar', str(RATE), '-f', 's16le', '-acodec', 'pcm_s16le']


def ff(srcs, dst, extra=(), filt=None):
    cmd = ['ffmpeg', '-v', 'error', '-y']
    for s_ in srcs:
        cmd += ['-i', s_]
    if filt:
        cmd += ['-filter_complex', filt]
    subprocess.run(cmd + list(extra) + [dst], check=True)


def adpcm(srcs, dst, filt=None):
    tmp = os.path.join(BUILD, 'tmp.raw')
    ff(srcs, tmp, RAW, filt)
    subprocess.run([ctool('adpcm'), tmp, outpath(dst)], check=True)


def convert_audio():
    S = os.path.join(A, 'songs')
    for s in SONGS:
        d = os.path.join(S, s['id'])
        v = s['var']
        inst = os.path.join(d, 'Inst%s.ogg' % ('-' + s['inst'] if s['inst'] else ''))
        if not os.path.exists(inst):
            inst = os.path.join(d, 'Inst.ogg')

        def voice(ids, fallback):
            cands = []
            for i in ids + [fallback]:
                for nm in (i, i.split('-')[0]):
                    cands += ['Voices-%s-%s.ogg' % (nm, v)] if v else []
                    cands += ['Voices-%s.ogg' % nm]
            for c in cands:
                if os.path.exists(os.path.join(d, c)):
                    return os.path.join(d, c)
            return None
        vp = voice(s['pvoc'], s['player'])
        vo = voice(s['ovoc'], s['opp'])
        if vp is None and not s['pvoc'] and os.path.exists(os.path.join(d, 'Voices.ogg')):
            vp = os.path.join(d, 'Voices.ogg')
        dst = 'snd/%s_inst.adp' % s['tag']
        if os.path.exists(outpath(dst)) and '--redo-audio' not in OPTS:
            continue
        # the opponent voices are mixed into the instrumental (one stream less)
        if vo:
            adpcm([inst, vo], dst, '[0:a][1:a]amix=inputs=2:normalize=0')
        else:
            adpcm([inst], dst)
        if vp:
            adpcm([vp], 'snd/%s_voices.adp' % s['tag'])
        print('audio', s['tag'], os.path.basename(inst), vp and os.path.basename(vp), vo and os.path.basename(vo))
    P = os.path.join(A, 'preload', 'music')
    adpcm([os.path.join(P, 'freakyMenu', 'freakyMenu.ogg')], 'snd/menu.adp')
    adpcm([os.path.join(P, 'stayFunky', 'stayFunky.ogg')], 'snd/results.adp')
    G = os.path.join(A, 'shared', 'music', 'gameplay', 'gameover')
    for nm in ('gameOver', 'gameOverEnd', 'gameOver-pixel', 'gameOverEnd-pixel'):
        p = os.path.join(G, nm + '.ogg')
        if os.path.exists(p):
            adpcm([p], 'snd/%s.adp' % nm.lower().replace('-', '_'))
    for nm, src in (('breakfast', 'shared/music/breakfast.ogg'), ('breakfast_pixel', 'week6/music/breakfast-pixel.ogg')):
        p = os.path.join(A, src)
        if os.path.exists(p):
            adpcm([p], 'snd/%s.adp' % nm)
    md = os.path.join(ROOT, 'maxmod_data')
    os.makedirs(md, exist_ok=True)
    for f in os.listdir(md):
        os.remove(os.path.join(md, f))
    SFX = [('scroll', 'preload/sounds/scrollMenu.ogg'), ('confirm', 'preload/sounds/confirmMenu.ogg'),
           ('cancel', 'preload/sounds/cancelMenu.ogg'),
           ('missnote1', 'shared/sounds/missnote1.ogg'), ('missnote2', 'shared/sounds/missnote2.ogg'),
           ('missnote3', 'shared/sounds/missnote3.ogg')]
    for cs in ('funkin', 'pixel'):
        for n in ('THREE', 'TWO', 'ONE', 'GO'):
            SFX.append(('intro%s_%s' % (n.lower(), cs), 'shared/sounds/gameplay/countdown/%s/intro%s.ogg' % (cs, n)))
    SFX += [('loss', 'shared/sounds/gameplay/gameover/fnf_loss_sfx.ogg'),
            ('loss_pixel', 'shared/sounds/gameplay/gameover/fnf_loss_sfx-pixel.ogg'),
            ('thunder1', 'week2/sounds/thunder_1.ogg'), ('thunder2', 'week2/sounds/thunder_2.ogg'),
            ('train', 'week3/sounds/train_passes.ogg'), ('carpass', 'week4/sounds/carPass0.ogg')]
    for nm, src in SFX:
        p = os.path.join(A, src)
        if not os.path.exists(p):
            print('  missing sound', src)
            continue
        ff([p], os.path.join(md, nm + '.wav'), ['-ac', '1', '-ar', str(RATE), '-acodec', 'pcm_u8'])


# ===========================================================================
# HUD: note styles, splashes, rating popups, countdown, health icons
# HUD sheets use 1 texel per DS pixel at hud zoom 1 (sheet scale = KDS
# texels per HUD unit), pixel styles 1 texel per source pixel.
# ===========================================================================
DIRS = ['Left', 'Down', 'Up', 'Right']


def convert_noteskin(name):
    st = jload(os.path.join(DATA, 'notestyles', name + '.json'))['assets']
    pixel = name == 'pixel'

    def src_of(key):
        return parse_sparrow(resolve(st[key]['assetPath']) + '.xml', resolve(st[key]['assetPath']) + '.png')
    nsc = float(st['note'].get('scale', 0.7))
    ssc = float(st['noteStrumline'].get('scale', 0.7))
    tex = (1.0 / nsc) if pixel else KDS          # texels per hud unit
    sh = Sheet('note_' + name, tex)

    def add_centered(fname, im, f, sc):
        full = full_frame(im, f)
        t, (bx, by) = trim(full)
        k = sc * tex
        ts = scaled(t, k, pixel)
        sh.add_packed(fname, ts, int(round((bx - full.width / 2.0) * k)), int(round((by - full.height / 2.0) * k)))
    im, frames = src_of('noteStrumline')
    seq = {}
    for i, dname in enumerate(DIRS):
        dk = dname.lower()
        for kind in ('Static', 'Press', 'Confirm'):
            sel = frames_by_prefix(frames, st['noteStrumline']['data'][dk + kind]['prefix'])
            if kind == 'Press':
                sel = sel[:1]
            elif kind == 'Confirm' and not pixel:
                sel = sel[::2]
            ids = []
            seen = {}
            for f in sel:
                k = (f['x'], f['y'], f['w'], f['h'], f['fx'], f['fy'])
                if k not in seen:
                    seen[k] = len(sh.items)
                    add_centered('%s%d_%d' % (kind.lower(), i, len(seen)), im, f, ssc)
                ids.append(seen[k])
            seq[(kind, i)] = ids[:8]
    im, frames = src_of('note')
    fnote = []
    for i, dname in enumerate(DIRS):
        f = frames_by_prefix(frames, st['note']['data'][dname.lower()]['prefix'])[0]
        fnote.append(len(sh.items))
        add_centered('note%d' % i, im, f, nsc)
    hn = st['holdNote']
    hsc = float(hn.get('scale', 0.7))
    himg = load_img(hn['assetPath'])
    cw = himg.width // 8
    fhold, fend = [], []
    for i in range(4):
        for kind, lst in (('hold', fhold), ('end', fend)):
            col = himg.crop(((2 * i + (kind == 'end')) * cw, 0, (2 * i + (kind == 'end') + 1) * cw,
                             int(himg.height * (0.9 if kind == 'end' and not pixel else 1))))
            if kind == 'hold':
                col = col.crop((0, col.height // 4, cw, col.height * 3 // 4))
            k = hsc * tex
            c2 = col.resize((max(1, int(round(col.width * k))), max(1, int(round(col.height * k)))),
                            Image.NEAREST if pixel else Image.LANCZOS)
            lst.append(len(sh.items))
            sh.add_packed('%s%d' % (kind, i), c2, -c2.width // 2, 0)
    sh.build()
    endh = himg.height * hsc * (0.9 if not pixel else 1)
    rows = []
    for kind in ('Press', 'Confirm'):
        rows.append('{%s}' % ','.join('{%s}' % ','.join(str(v) for v in (seq[(kind, i)] + [0] * 8)[:8]) for i in range(4)))
        rows.append('{%s}' % ','.join(str(len(seq[(kind, i)])) for i in range(4)))
    C_OUT.append('const NoteSkin noteskin_%s = {&sheet_note_%s, {%s}, {%s}, {%s}, {%s}, %s, %s, %s, %s, %s, %s, %d};' % (
        name, name, ','.join(str(seq[('Static', i)][0]) for i in range(4)), ','.join(map(str, fnote)),
        ','.join(map(str, fhold)), ','.join(map(str, fend)), rows[0], rows[1], rows[2], rows[3],
        cfloat(cw * hsc), cfloat(endh), 1 if pixel else 0))
    H_OUT.append('extern const NoteSkin noteskin_%s;' % name)
    # splashes
    sp = st.get('noteSplash')
    ssh = Sheet('splash_' + name, tex * (0.75 if not pixel else 1), fmt=FMT_16COL)
    sim, sfr = parse_sparrow(resolve(sp['assetPath']) + '.xml', resolve(sp['assetPath']) + '.png')
    spsc = float(sp.get('scale', 1.0))
    seqs = []
    for i, dname in enumerate(DIRS):
        var = []
        for v in sp['data'][dname.lower() + 'Splashes'][:1]:
            sel = frames_by_prefix(sfr, v['prefix'])[::2]
            first = len(ssh.items)
            for j, f in enumerate(sel):
                full = full_frame(sim, f)
                t, (bx, by) = trim(full)
                k = spsc * ssh.scale
                ssh.add_packed('s%d_%d' % (i, len(ssh.items)), scaled(t, k, pixel),
                               int(round((bx - full.width / 2.0) * k)), int(round((by - full.height / 2.0) * k)))
            var.append((first, len(sel)))
        seqs.append(var)
    ssh.build()
    C_OUT.append('const SplashDef splash_%s = {&sheet_splash_%s, {%s}, %s};' % (
        name, name, ','.join('{{%d,%d},{%d,%d}}' % (v[0][0], v[0][1], v[-1][0], v[-1][1]) for v in seqs),
        cfloat(sp.get('alpha', 0.8))))
    H_OUT.append('extern const SplashDef splash_%s;' % name)


def convert_popups():
    """Rating / combo popups (camGame world sprites), baked at their drawn scale:
    frames are in world units through the sheet scale."""
    for style, pixel in (('funkin', False), ('pixel', True)):
        rs, ns = (0.65, 0.45) if not pixel else (6 * 0.7, 6 * 0.85)
        ts = 0.2 if not pixel else 1.0 / ns
        sh = Sheet('popup_' + style, ts, fmt=FMT_256COL)
        names = ['sick', 'good', 'bad', 'shit'] + ['num%d' % i for i in range(10)] + ['combo']
        sizes = []
        for n in names:
            p = os.path.join(A, 'preload', 'images', 'ui', 'popup', style, n + '.png')
            if not os.path.exists(p):
                p = os.path.join(A, 'preload', 'images', 'ui', 'popup', 'funkin', n + '.png')
            im = Image.open(p).convert('RGBA')
            ds = ns if n.startswith('num') else rs
            s2 = scaled(im, ds * ts, pixel)
            sh.add_packed(n, s2, 0, 0)
            sizes.append((im.width * ds, im.height * ds))
        sh.build()
        C_OUT.append('const float popup_%s_size[%d][2] = {%s};' % (style, len(names), ','.join('{%s,%s}' % (cfloat(w), cfloat(h)) for w, h in sizes)))
        H_OUT.append('extern const float popup_%s_size[%d][2];' % (style, len(names)))
    # countdown (camHUD, centered)
    for style, pixel in (('funkin', False), ('pixel', True)):
        sc = 1.0 if not pixel else 6.0
        ts = KDS * 0.5 if not pixel else 1.0 / 6
        sh = Sheet('countdown_' + style, ts, fmt=FMT_16COL if not pixel else FMT_256COL)
        for n in ('ready', 'set', 'go'):
            im = Image.open(os.path.join(A, 'shared', 'images', 'ui', 'countdown', style, n + '.png')).convert('RGBA')
            s2 = scaled(im, sc * ts, pixel)
            sh.add_packed(n, s2, -s2.width // 2, -s2.height // 2)
        sh.build()


ICONS = set()


def convert_icons():
    """Health icons: frame 0 normal, 1 losing (150x150 each, pixel ones 32x32)."""
    for icon in sorted(ICONS):
        p = os.path.join(A, 'preload', 'images', 'icons', 'icon-%s.png' % icon)
        if not os.path.exists(p):
            p = os.path.join(A, 'preload', 'images', 'icons', 'icon-face.png')
        im = Image.open(p).convert('RGBA')
        pixel = im.height <= 64
        n = 2 if im.width >= im.height * 2 else 1
        fw = im.width // n
        sh = Sheet('icon_' + cid(icon).lower(), KDS)
        for i in range(2):
            fr = im.crop((min(i, n - 1) * fw, 0, (min(i, n - 1) + 1) * fw, im.height))
            k = 150.0 / fw * KDS
            sh.add_packed('f%d' % i, scaled(fr, k, pixel), 0, 0)
        sh.build()
    rows = ','.join('{%s,&sheet_icon_%s}' % (cstr(i), cid(i).lower()) for i in sorted(ICONS))
    C_OUT.append('const IconDef icon_defs[] = {%s};\nconst int icon_count = %d;' % (rows, len(ICONS)))
    H_OUT.append('extern const IconDef icon_defs[];\nextern const int icon_count;')


# ===========================================================================
# Menus (top screen, the 1280x720 view letterboxed in 256x144: MS px per unit)
# ===========================================================================
MS = SH.MS


def msparrow(sh, prefix, path, bake, anims, stride=1):
    """anims: {name: (xml prefix, indices or None)} -> {name: [frame ids]}"""
    base = resolve(path)
    im, frames = parse_sparrow(base + '.xml', base + '.png')
    seen = {}
    out = {}
    for an, (pre, idx) in anims.items():
        sel = frames_by_prefix(frames, pre)
        if idx:
            sel = [sel[i] for i in idx if i < len(sel)]
        sel = sel[::stride]
        ids = []
        for f in sel:
            k = (f['x'], f['y'], f['w'], f['h'], f['fx'], f['fy'])
            if k not in seen:
                seen[k] = len(sh.items)
                sh.image('%s%d' % (prefix, len(seen)), full_frame(im, f), bake)
            ids.append(seen[k])
        out[an] = ids
    return out, frames


def emit_anim(name, ids):
    emit_seq(name, ids)


def convert_menus(weeks):
    P = os.path.join(A, 'preload', 'images')
    # --- title ---------------------------------------------------------------
    sh = MSheet('title')
    seq, frames = msparrow(sh, 'logo', 'preload:logoBumpin', 0.75, {'bump': ('logo bumpin', None)}, stride=5)
    emit_anim('title_logo', seq['bump'])
    seq, frames = msparrow(sh, 'gf', 'preload:gfDanceTitle', 0.75,
                           {'left': ('gfDance', [30] + list(range(15))), 'right': ('gfDance', list(range(15, 30)))}, stride=4)
    emit_anim('title_gf_left', seq['left'])
    emit_anim('title_gf_right', seq['right'])
    try:
        at = Atlas(os.path.join(P, 'title-screen-text'))
        labels = at.main.labels()
        b = at.bounds()
        for nm in ('idle', 'press'):
            lb = [l for l in labels if l.lower().startswith(nm)]
            fr = labels[lb[0]][0] if lb else 0
            r = at.render(at.items(at.main, fr), MS * 0.75, b[0], b[1])
            if r:
                sh.add_packed('enter_' + nm, r[0], r[1], r[2])
        H_OUT.append('#define TITLE_ENTER_W %s\n#define TITLE_ENTER_H %s' % (cfloat(b[2]), cfloat(b[3])))
    except Exception as e:  # noqa
        print('  title text:', e)
    sh.build()
    # --- main menu -----------------------------------------------------------
    for nm in ('menuBG', 'menuDesat'):
        s2 = MSheet(nm.lower())
        im = Image.open(os.path.join(P, nm + '.png')).convert('RGBA')
        s2.image('bg', im, 1280.0 / im.width * 1.0, tiled=True, trimit=False)
        s2.build()
    sh = MSheet('mainmenu')
    items = ['storymode', 'freeplay', 'options', 'credits']
    for it in items:
        seq, _ = msparrow(sh, it, 'preload:mainmenu/' + it, 1.0, {'idle': (it + ' idle', None), 'sel': (it + ' selected', None)})
        emit_anim('mm_%s_idle' % it, seq['idle'])
        emit_anim('mm_%s_sel' % it, seq['sel'])
    sh.build()
    # --- story menu ----------------------------------------------------------
    sh = MSheet('storymenu')
    for w in weeks:
        im = Image.open(resolve('preload:' + w['title']) + '.png').convert('RGBA')
        sh.image('title_' + w['id'], im, 1.0)
        H_OUT.append('#define WEEKTITLE_%s_W %s' % (cid(w['id']), cfloat(im.width)))
    for d in DIFFS:
        im = Image.open(os.path.join(P, 'storymenu', 'difficulties', d + '.png')).convert('RGBA')
        sh.image('diff_' + d, im, 1.0)
    seq, _ = msparrow(sh, 'arrow', 'preload:storymenu/ui/arrows', 1.0, {'l': ('leftIdle', None), 'r': ('rightIdle', None),
                                                                       'lc': ('leftConfirm', None), 'rc': ('rightConfirm', None)})
    emit_anim('sm_arrows', seq['l'] + seq['r'] + seq['lc'] + seq['rc'])
    sh.build()
    # week characters (one small sheet per prop)
    props = {}
    for w in weeks:
        for p in w['props']:
            props.setdefault(p['assetPath'], p)
    for path, p in props.items():
        nm = cid(os.path.basename(path)).lower()
        s2 = MSheet('smprop_' + nm)
        anims = {}
        for a in p['animations']:
            anims[a['name']] = (a['prefix'], a.get('frameIndices'))
        seq, _ = msparrow(s2, 'f', 'preload:' + path, float(p.get('scale', 1.0)), anims, stride=2)
        s2.build()
        for an, ids in seq.items():
            emit_anim('smprop_%s_%s' % (nm, cid(an).lower()), ids)
    rows = []
    for w in weeks:
        ps = []
        for p in w['props'][:3]:
            nm = cid(os.path.basename(p['assetPath'])).lower()
            names = [a['name'] for a in p['animations']]
            dance = 'danceLeft' in names
            first = 'danceLeft' if dance else 'idle'
            second = 'danceRight' if dance else ('confirm' if 'confirm' in names else 'idle')
            off = p.get('offsets', [0, 0])
            ps.append('{&sheet_smprop_%s,smprop_%s_%s,SMPROP_%s_%s_LEN,smprop_%s_%s,SMPROP_%s_%s_LEN,%d,%s,%s}' % (
                nm, nm, cid(first).lower(), nm.upper(), cid(first).upper(), nm, cid(second).lower(), nm.upper(),
                cid(second).upper(), 1 if dance else 0, cfloat(off[0]), cfloat(off[1])))
        while len(ps) < 3:
            ps.append('{0}')
        rows.append('{{%s},%d}' % (','.join(ps), min(3, len(w['props']))))
    C_OUT.append('const WeekProps week_props[WEEK_COUNT] = {%s};' % ',\n'.join(rows))
    H_OUT.append('extern const WeekProps week_props[WEEK_COUNT];')
    # --- alphabet (bold) -------------------------------------------------------
    im, frames = parse_sparrow(os.path.join(P, 'fonts', 'bold.xml'), os.path.join(P, 'fonts', 'bold.png'))
    special = {'&': '-andpersand-', "'": '-apostraphie-', '\\': '-back slash-', ',': '-comma-', '-': '-dash-',
               '!': '-exclamation point-', '/': '-forward slash-', '>': '-greater than-', '<': '-less than-',
               '.': '-period-', '?': '-question mark-', '"': '-start quote-', '*': '-multiply x-'}
    sh = Sheet('alphabet', 0.5)
    table = []
    for ch in range(32, 127):
        c = chr(ch)
        if 'a' <= c <= 'z':
            c = c.upper()
        pre = special.get(c, c)
        sel = [f for f in frames if f['name'][:len(pre)] == pre and f['name'][len(pre):].isdigit()]
        if c == ' ' or not sel:
            table.append((-1, 40))
            continue
        f = sorted(sel, key=lambda f: f['name'])[0]
        full = full_frame(im, f)
        t, (bx, by) = trim(full)
        table.append((len(sh.items), full.width))
        sh.add_packed('c%d' % ch, scaled(t, 0.5), int(round(bx * 0.5)), int(round(by * 0.5)))
    sh.build()
    C_OUT.append('const s16 alphabet_map[95][2] = {%s};' % ','.join('{%d,%d}' % t for t in table))
    H_OUT.append('extern const s16 alphabet_map[95][2];')


MENU_TS = 0.2 * 0.85        # texels per menu unit (the 1280x720 view is 256 px wide)


def convert_menu_atlas(name, path, anims):
    """An Animate atlas used in a menu, as a .chr: anims = [(name, label, loop)].
    Frames are relative to the timeline bounds; gx/gy hold the bounds origin."""
    src = Src.get('atlas', path)
    out = []
    for an, label, loop in anims:
        hs = src.anim_frames(label, None)
        if not hs:
            print('  %s: no label %s' % (name, label))
            continue
        out.append(dict(name=an, fps=24.0, loop=loop, offx=0, offy=0, frames=hs, src=src))
    b = src.bounds
    info = dict(W=b[2], H=b[3], gx=b[0], gy=b[1], camx=0, camy=0, sing=8.0, dance=0.0)
    write_chr(name, out, info, 1.0, fit_ts(out, 1.0, MENU_TS, max_area=128 * 128), False, False)


def convert_abot():
    """Nene's A-Bot speaker (scripts/characters/nene.hxc): the abotSystem atlas
    with stereoBG behind it, as a .chr drawn behind Nene."""
    for tag, path in (('abot', 'shared:characters/abot/abotSystem'), ('abot_dark', 'shared:characters/abot/dark/abotSystem')):
        try:
            src = Src.get('atlas', path)
        except FileNotFoundError:
            continue
        ts = CHAR_TS
        bg = Image.open(resolve('shared:characters/abot/stereoBG') + '.png').convert('RGBA')
        b = src.bounds
        frames = []
        for i in range(src.atlas.main.count):
            r = src.atlas.render(src.atlas.items(src.atlas.main, i), ts, 0, 0)
            # canvas in texels from world (0, 0) = abot position (flixel frame = bounds)
            X0 = int(min(150 * ts, r[1] if r else 0)) - 1
            Y0 = int(min(30 * ts, r[2] if r else 0)) - 1
            bgs = scaled(bg, ts)
            W = int(max(150 * ts + bgs.width, (r[1] + r[0].width) if r else 0)) - X0 + 2
            H = int(max(30 * ts + bgs.height, (r[2] + r[0].height) if r else 0)) - Y0 + 2
            can = Image.new('RGBA', (W, H), (0, 0, 0, 0))
            can.alpha_composite(bgs, (int(150 * ts) - X0, int(30 * ts) - Y0))
            if r:
                can.alpha_composite(r[0], (r[1] - X0, r[2] - Y0))
            frames.append(('i', '%s%d' % (tag, i), can, X0 / ts - b[0], Y0 / ts - b[1], W / ts, H / ts))
        info = dict(W=b[2], H=b[3], gx=b[0], gy=b[1], camx=0, camy=0, sing=8.0, dance=1.0)
        write_chr(tag, [dict(name='idle', fps=24.0, loop=False, offx=0, offy=0, frames=frames, src=src)], info, 1.0, ts, False, False)


def convert_freeplay_menus():
    P = os.path.join(A, 'preload', 'images')
    for pl in PLAYERS:
        convert_menu_atlas('dj_' + pl, 'preload:freeplay/freeplay-' + ('boyfriend' if pl == 'bf' else pl),
                           [('intro', 'Intro', False), ('idle', 'Idle', True), ('confirm', 'Confirm', False),
                            ('fistPump', 'Fist Pump', False), ('loss', 'Fist Pump Loss', False)])
        convert_menu_atlas('chill_' + pl, 'preload:charSelect/%sChill' % pl,
                           [('idle', 'idle', True), ('select', 'select', False), ('slidein', 'slidein', False)])
        sh = MSheet('fpbg_' + pl)
        im = Image.open(os.path.join(P, 'freeplay', 'freeplayBGweek1-%s.png' % pl)).convert('RGBA')
        sh.image('bg', im, 720.0 / im.height, tiled=True, trimit=False)
        sh.build()
        sh = MSheet('csname_' + pl)
        nt = os.path.join(P, 'charSelect', '%sNametag.png' % ('boyfriend' if pl == 'bf' else pl))
        if os.path.exists(nt):
            sh.image('tag', Image.open(nt).convert('RGBA'), 1.0)
        sh.build()
    sh = MSheet('charselect')
    for nm, fn, bake in (('bg', 'charSelectBG.png', 1.0), ('stage', 'charSelectStage.png', 1.0), ('curtains', 'curtains.png', 1.0)):
        im = Image.open(os.path.join(P, 'charSelect', fn)).convert('RGBA')
        sh.image(nm, im, bake, tiled=True)
    sh.build()
    for d in DIFFS:
        f = os.path.join(P, 'freeplay', 'freeplay%s.png' % d)
        if os.path.exists(f):
            s2 = MSheet('fpdiff_' + d)
            im = Image.open(f).convert('RGBA')
            if os.path.exists(f[:-4] + '.xml'):      # animated (nightmare): first frame
                im2, fr = parse_sparrow(f[:-4] + '.xml', f)
                im = full_frame(im2, fr[0])
            s2.image('main', im, 1.0)
            s2.build()
    C_OUT.append('const SheetDef *const fpdiff_sheets[%d] = {%s};' % (len(DIFFS), ','.join(
        '&sheet_fpdiff_' + d if os.path.exists(os.path.join(P, 'freeplay', 'freeplay%s.png' % d)) else 'NULL' for d in DIFFS)))
    H_OUT.append('extern const SheetDef *const fpdiff_sheets[%d];' % len(DIFFS))


# Weekend 1 video cutscenes (songs/darnell.hxc, 2hot.hxc, blazin.hxc): 8 bpp
# frames + palette, LZ77 compressed (see source/video.c), audio as ADPCM
VIDEOS = [('darnellCutscene', 0), ('2hotCutscene', 1), ('blazinCutscene', 1)]   # (name, 0 start / 1 ending)
VIDEO_SONG = {'darnellCutscene': 'darnell', '2hotCutscene': '2hot', 'blazinCutscene': 'blazin'}


def convert_videos():
    rows = []
    for name, ending in VIDEOS:
        src = os.path.join(A, 'videos', 'videos', name + '.mp4')
        if not os.path.exists(src):
            continue
        rows.append('{%s,%s,%d}' % (cstr(VIDEO_SONG[name]), cstr(name), ending))
        dst = outpath('vid', name + '.vid')
        if os.path.exists(dst) and '--redo-video' not in OPTS:
            continue
        w, h, fps = 256, 192, 12
        raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', src, '-vf', 'fps=%d,scale=-2:192:flags=lanczos,crop=256:192' % fps,
                              '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'], check=True, stdout=subprocess.PIPE).stdout
        fsize = w * h * 3
        n = len(raw) // fsize
        frames = []
        for i in range(n):
            im = Image.frombytes('RGB', (w, h), raw[i * fsize:(i + 1) * fsize])
            q = im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
            pal = q.getpalette()[:768]
            pal += [0] * (768 - len(pal))
            pb = b''.join(struct.pack('<H', rgb15(pal[k * 3], pal[k * 3 + 1], pal[k * 3 + 2])) for k in range(256))
            frames.append(pb + lz10(q.tobytes()))
        hdr = b'VID1' + struct.pack('<HHHH', w, h, fps, n)
        pos = len(hdr) + 4 * (n + 1)
        offs = []
        for fr in frames:
            offs.append(pos)
            pos += len(fr)
        offs.append(pos)
        with open(dst, 'wb') as f:
            f.write(hdr)
            f.write(struct.pack('<%dI' % (n + 1), *offs))
            for fr in frames:
                f.write(fr)
        adpcm([src], 'snd/%s.adp' % name)
        print('video %-16s %4d frames %6d KB' % (name, n, pos // 1024))
    C_OUT.append('const Cutscene cutscenes[] = {%s};\nconst int cutscene_count = %d;' % (','.join(rows) or '{0}', len(rows)))
    H_OUT.append('typedef struct { const char *song, *video; u8 ending; } Cutscene;\nextern const Cutscene cutscenes[];\nextern const int cutscene_count;')


# Week 6 dialogue (data/dialogue): conversations, speaker portraits, boxes.
# Sprites are in camHUD at scale 5.4 around their frame center, positioned
# at -offsets (FlxSprite.offset); frames are baked 1 texel per source pixel.
CONVERSATIONS = ['senpai', 'roses', 'thorns']


def sprite_box(fw, fh, sx, sy, x=0, y=0):
    """Top-left world position and size of a frame drawn with scale (sx, sy)
    around its center (no updateHitbox)."""
    ox, oy = fw / 2.0, fh / 2.0
    return x + ox - ox * sx, y + oy - oy * sy, fw * sx, fh * sy


def convert_dialogue():
    D = os.path.join(DATA, 'dialogue')
    spk, box = {}, {}

    def sprite(kind, name, path, anim_prefix, d):
        base = resolve(path)
        im, frames = parse_sparrow(base + '.xml', base + '.png')
        sel = frames_by_prefix(frames, anim_prefix) or frames
        f = sel[-1] if kind == 'box' else sel[0]
        full = full_frame(im, f)
        sc = float(d.get('scale', 1))
        off = d.get('offsets', [0, 0])
        x0, y0, w, h = sprite_box(full.width, full.height, sc, sc, -off[0], -off[1])
        if d.get('flipX'):
            full = full.transpose(Image.FLIP_LEFT_RIGHT)
        t, (bx, by) = trim(full)
        sh = Sheet('dlg_%s_%s' % (kind, cid(name).lower()), 1.0 / sc)
        sh.add_packed('main', t, bx, by)
        sh.build()
        return '{&sheet_dlg_%s_%s,%s,%s,%s,%s}' % (kind, cid(name).lower(), cfloat(x0), cfloat(y0), cfloat(w), cfloat(h))
    rows = []
    for c in CONVERSATIONS:
        p = os.path.join(D, 'conversations', c + '.json')
        if not os.path.exists(p):
            continue
        cv = jload(p)
        lines = []
        for ln in cv['dialogue']:
            sp, bx = ln['speaker'], ln['box']
            if sp not in spk:
                sd = jload(os.path.join(D, 'speakers', sp + '.json'))
                spk[sp] = (len(spk), sprite('spk', sp, sd['assetPath'], sd['animations'][-1]['prefix'], sd))
            if bx not in box:
                bd = jload(os.path.join(D, 'boxes', bx + '.json'))
                pre = next((a['prefix'] for a in bd['animations'] if a['name'] == 'idle'), bd['animations'][0]['prefix'])
                box[bx] = (len(box), sprite('box', bx, bd['assetPath'], pre, bd), color_hex(bd['text'].get('color', '#3F2021')))
            text = ' '.join(ln['text'])
            lines.append('{%d,%d,%s}' % (spk[sp][0], box[bx][0], cstr(text)))
        C_OUT.append('static const DialogueLine dlg_%s_lines[] = {%s};' % (cid(c).lower(), ','.join(lines)))
        mus = (cv.get('music') or {}).get('asset') or ''
        if mus and '--no-audio' not in OPTS:
            src = os.path.join(A, 'week6', 'music', mus + '.ogg')
            if os.path.exists(src) and not os.path.exists(outpath('snd', mus.lower() + '.adp')):
                adpcm([src], 'snd/%s.adp' % mus.lower())
        rows.append('{%s,0x%06X,%s,%d,dlg_%s_lines}' % (cstr(c), color_hex(cv['backdrop'].get('color', '#B3DFD8')),
                                                       cstr(mus.lower()), len(lines), cid(c).lower()))
    C_OUT.append('const DialogueSprite dlg_speakers[] = {%s};' % ','.join(v[1] for v in sorted(spk.values())))
    C_OUT.append('const DialogueSprite dlg_boxes[] = {%s};' % ','.join(v[1] for v in sorted(box.values())))
    C_OUT.append('const u16 dlg_box_color[] = {%s};' % ','.join('0x%04X' % rgb15c(v[2]) for v in sorted(box.values())))
    C_OUT.append('const Conversation conversations[] = {%s};\nconst int conversation_count = %d;' % (','.join(rows), len(rows)))
    H_OUT.append("""typedef struct { const SheetDef *sheet; float x, y, w, h; } DialogueSprite;
typedef struct { u8 speaker, box; const char *text; } DialogueLine;
typedef struct { const char *song; u32 bg; const char *music; u8 n; const DialogueLine *lines; } Conversation;
extern const DialogueSprite dlg_speakers[];
extern const DialogueSprite dlg_boxes[];
extern const u16 dlg_box_color[];
extern const Conversation conversations[];
extern const int conversation_count;""")


def convert_fonts_all():
    vcr = os.path.join(A, 'fonts', 'vcr.ttf')
    SH.FONTS[:] = [('vcr8', vcr, 8, 0), ('vcr8o', vcr, 8, 1), ('vcr12', vcr, 12, 0), ('vcr12o', vcr, 12, 1),
                   ('vcr16o', vcr, 16, 1)]
    SH.convert_fonts()


# ===========================================================================
def main():
    H_OUT.append('#pragma once')
    H_OUT.append('#include "assets.h"')
    C_OUT.append('#include "gen_assets.h"')
    weeks = collect_songs()
    stage_ids = sorted(set(s['stage'] for s in SONGS))
    H_OUT.append('#define CHART_DIFFS 3')
    for tag, (c, pl) in sorted(NEED_CHARS.items()):
        info = convert_character(c, pl)
        ICONS.add(info['icon'])
    for sid in stage_ids:
        convert_stage(sid)
    emit_songs(weeks, stage_ids)
    emit_kind_anims()
    for ns in ('funkin', 'pixel'):
        convert_noteskin(ns)
    convert_popups()
    convert_icons()
    convert_menus(weeks)
    convert_freeplay_menus()
    convert_abot()
    convert_videos()
    convert_dialogue()
    convert_fonts_all()
    if '--no-audio' not in OPTS:
        convert_audio()
    H_OUT.append('#define AUDIO_RATE %d' % RATE)
    with open(os.path.join(ROOT, 'source/gen_assets.c'), 'w') as f:
        f.write('// Generated by tools/convert.py - do not edit\n' + '\n'.join(C_OUT) + '\n')
    with open(os.path.join(ROOT, 'include/gen_assets.h'), 'w') as f:
        f.write('// Generated by tools/convert.py - do not edit\n' + '\n'.join(H_OUT) + '\n')


if __name__ == '__main__':
    main()
