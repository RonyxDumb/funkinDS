"""Shared helpers for the Funkin 3DS asset converter."""
import math
import xml.etree.ElementTree as ET
import zlib

import numpy as np
from PIL import Image

# virtual (HaxeFlixel) screen -> 3DS top screen factor: the 1280x720 game
# view is 400x225 (full width of the 400x240 screen).
K = 400.0 / 1280.0

TF_RGBA8 = 0
TF_RGB565 = 1
MAXTEX = 1024


def log2size(n):
    """texture size code: 0 -> 8 px ... 7 -> 1024 px."""
    s = 0
    while (8 << s) < n:
        s += 1
    return s


def pow2(n):
    p = 8
    while p < n:
        p *= 2
    return p


def pieces(n, maxdim=MAXTEX):
    """Split a length into power-of-two textures: [(start, size, drawn)].
    Consecutive pieces overlap by one texel so that bilinear filtering is
    seamless (a piece is drawn up to its last texel but one)."""
    out = []
    pos = 0
    while True:
        rem = n - pos
        if rem <= 8:
            out.append((pos, 8, rem))
            break
        up = pow2(rem)
        if up <= maxdim and (up - rem <= rem * 0.3 or up <= 32):
            out.append((pos, up, rem))
            break
        p = min(1 << int(math.floor(math.log2(rem))), maxdim)
        if p >= rem:
            out.append((pos, p, rem))
            break
        out.append((pos, p, p - 1))
        pos += p - 1
    return out


# ---------------------------------------------------------------------------
# GPU texture encoding: tiles of 8x8 texels, Morton order inside a tile,
# tile rows from the top (the engine samples v = 0 at the top).
# ---------------------------------------------------------------------------
_MORTON = np.zeros(64, dtype=np.int64)
for _y in range(8):
    for _x in range(8):
        _m = (_x & 1) | ((_y & 1) << 1) | ((_x & 2) << 1) | ((_y & 2) << 2) | ((_x & 4) << 2) | ((_y & 4) << 3)
        _MORTON[_m] = _y * 8 + _x


def tile(a):
    """(H, W, ...) array with H, W multiples of 8 -> tiled flat array."""
    h, w = a.shape[:2]
    rest = a.shape[2:]
    t = a.reshape((h // 8, 8, w // 8, 8) + rest).swapaxes(1, 2).reshape((h // 8, w // 8, 64) + rest)
    return t[:, :, _MORTON].reshape((h * w,) + rest)


def premultiplied(im):
    a = np.asarray(im.convert('RGBA'), dtype=np.uint32)
    al = a[:, :, 3:4]
    rgb = (a[:, :, :3] * al + 127) // 255
    return np.concatenate([rgb, al], axis=2).astype(np.uint8)


def encode_texture(im, w, h):
    """RGBA image -> (fmt, bytes) for a w x h texture (image at the top left)."""
    can = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    can.paste(im, (0, 0))
    px = premultiplied(can)
    opaque = im.width == w and im.height == h and int(px[:, :, 3].min()) == 255
    t = tile(px)
    if opaque:
        r, g, b = t[:, 0].astype(np.uint16), t[:, 1].astype(np.uint16), t[:, 2].astype(np.uint16)
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        return TF_RGB565, v.astype('<u2').tobytes()
    # RGBA8 texel = u32 0xRRGGBBAA little endian: bytes A, B, G, R
    return TF_RGBA8, t[:, ::-1].tobytes()


def zpack(raw):
    return zlib.compress(raw, 9)


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


# ---------------------------------------------------------------------------
# 8 bpp RGBA palettes (characters): k-means in premultiplied space
# ---------------------------------------------------------------------------
def _nearest(x, c):
    """index of the nearest center for every row of x (chunked)."""
    out = np.empty(len(x), dtype=np.int64)
    cc = (c * c).sum(1)
    for i in range(0, len(x), 65536):
        xs = x[i:i + 65536]
        d = cc[None, :] - 2 * xs @ c.T
        out[i:i + 65536] = d.argmin(1)
    return out


def rgba_palette(images, n=255, iters=8, seed=1):
    """Shared palette for RGBA images: returns float array (n, 4) of
    premultiplied colors (0..255). Index 0 of the final palette is reserved
    for transparency (see index_rgba)."""
    pix = []
    for im in images:
        p = premultiplied(im).reshape(-1, 4)
        p = p[p[:, 3] >= 4]
        if len(p):
            pix.append(p)
    if not pix:
        return np.zeros((n, 4), dtype=np.float32)
    x = np.concatenate(pix).astype(np.float32)
    rng = np.random.default_rng(seed)
    if len(x) > 250000:
        x = x[rng.choice(len(x), 250000, replace=False)]
    # unique colors are weighted by their count
    q = x.astype(np.uint8).view(np.uint32).reshape(-1)
    u, inv, cnt = np.unique(q, return_inverse=True, return_counts=True)
    ux = u.view(np.uint8).reshape(-1, 4).astype(np.float32)
    if len(ux) <= n:
        c = np.zeros((n, 4), dtype=np.float32)
        c[:len(ux)] = ux
        c[len(ux):] = ux[-1]
        return c
    w = cnt.astype(np.float32)
    # init: k-means++ on the weighted unique colors (sampled)
    c = [ux[rng.choice(len(ux), p=w / w.sum())]]
    d = ((ux - c[0]) ** 2).sum(1)
    for _ in range(1, n):
        pr = d * w
        s = pr.sum()
        if s <= 0:
            break
        c.append(ux[rng.choice(len(ux), p=pr / s)])
        d = np.minimum(d, ((ux - c[-1]) ** 2).sum(1))
    c = np.array(c, dtype=np.float32)
    if len(c) < n:
        c = np.concatenate([c, np.repeat(c[-1:], n - len(c), 0)])
    for _ in range(iters):
        a = _nearest(ux, c)
        sw = np.bincount(a, weights=w, minlength=n)
        for k in range(4):
            sk = np.bincount(a, weights=w * ux[:, k], minlength=n)
            c[:, k] = np.where(sw > 0, sk / np.maximum(sw, 1e-9), c[:, k])
    return c


def index_rgba(im, pal):
    """RGBA image -> u8 array (H, W) of palette indices (1..255), 0 = transparent."""
    p = premultiplied(im)
    h, w = p.shape[:2]
    flat = p.reshape(-1, 4)
    q = flat.view(np.uint32).reshape(-1)
    u, inv = np.unique(q, return_inverse=True)
    ux = u.view(np.uint8).reshape(-1, 4).astype(np.float32)
    idx = _nearest(ux, pal) + 1
    idx[ux[:, 3] < 4] = 0
    return idx[inv.reshape(-1)].astype(np.uint8).reshape(h, w)


def pal_bytes(pal, count=256):
    """Palette as u32 GPU texels (0xRRGGBBAA little endian), entry 0 = transparent."""
    out = bytearray(4 * count)
    p = np.clip(np.round(pal), 0, 255).astype(np.uint8)
    for i in range(min(len(p), count - 1)):
        r, g, b, a = (int(v) for v in p[i])
        r, g, b = min(r, a), min(g, a), min(b, a)
        out[4 * (i + 1):4 * (i + 2)] = bytes((a, b, g, r))
    return bytes(out)


# ---------------------------------------------------------------------------
# sparrow / frames
# ---------------------------------------------------------------------------
def parse_sparrow(xml_path, png_path):
    """Returns (image, list of dict(name, x, y, w, h, fx, fy, fw, fh, rot))."""
    img = Image.open(png_path).convert('RGBA')
    root = ET.parse(xml_path).getroot()
    frames = []
    for t in root.iter('SubTexture'):
        a = t.attrib
        x, y, w, h = int(a['x']), int(a['y']), int(a['width']), int(a['height'])
        rot = a.get('rotated') == 'true'
        fx = int(a.get('frameX', 0))
        fy = int(a.get('frameY', 0))
        if rot:
            fw = int(a.get('frameWidth', h))
            fh = int(a.get('frameHeight', w))
        else:
            fw = int(a.get('frameWidth', w))
            fh = int(a.get('frameHeight', h))
        frames.append(dict(name=a['name'], x=x, y=y, w=w, h=h, fx=fx, fy=fy,
                           fw=fw, fh=fh, rot=rot))
    return img, frames


def crop_frame(img, f):
    c = img.crop((f['x'], f['y'], f['x'] + f['w'], f['y'] + f['h']))
    if f['rot']:
        c = c.rotate(90, expand=True)
    return c


def full_frame(img, f):
    """Frame pasted into its untrimmed source box."""
    c = crop_frame(img, f)
    box = Image.new('RGBA', (f['fw'], f['fh']), (0, 0, 0, 0))
    box.alpha_composite(c, (-f['fx'], -f['fy']))
    return box


def frames_by_prefix(frames, prefix):
    """Flixel addByPrefix: frames whose name starts with prefix, sorted by
    the numeric suffix (flixel sorts by the trailing digits)."""
    sel = [f for f in frames if f['name'].startswith(prefix)]

    def key(f):
        n = f['name'][len(prefix):]
        digits = ''
        for ch in reversed(n):
            if ch.isdigit():
                digits = ch + digits
            else:
                break
        return int(digits) if digits else 0
    sel.sort(key=key)
    return sel


def lz10_compress(data):
    """GBA BIOS LZ77 (type 0x10) compressor (slow fallback, tools/lz10.c is used)."""
    n = len(data)
    out = bytearray((0x10, n & 255, (n >> 8) & 255, (n >> 16) & 255))
    i = 0
    head = {}
    prev = [-1] * n
    while i < n:
        flag_pos = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best_len = 0
            best_disp = 0
            if i + 3 <= n:
                key = data[i:i + 3]
                j = head.get(key, -1)
                tries = 0
                maxlen = min(18, n - i)
                while j >= 0 and i - j <= 4096 and tries < 64:
                    if i - j >= 1:
                        l = 0
                        while l < maxlen and data[j + l] == data[i + l]:
                            l += 1
                        if l > best_len:
                            best_len = l
                            best_disp = i - j
                            if l == maxlen:
                                break
                    j = prev[j]
                    tries += 1
            if best_len >= 3:
                flags |= 0x80 >> bit
                d = best_disp - 1
                out.append(((best_len - 3) << 4) | (d >> 8))
                out.append(d & 255)
                for k in range(best_len):
                    if i + 3 <= n:
                        key = data[i:i + 3]
                        prev[i] = head.get(key, -1)
                        head[key] = i
                    i += 1
            else:
                out.append(data[i])
                if i + 3 <= n:
                    key = data[i:i + 3]
                    prev[i] = head.get(key, -1)
                    head[key] = i
                i += 1
        out[flag_pos] = flags
    while len(out) % 4:
        out.append(0)
    return bytes(out)
