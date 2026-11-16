// Bottom screen: 16bpp bitmap drawn in software
#include "engine.h"
#pragma GCC target("arm")

static u16 *fb;      // RAM back buffer
static u16 *vfb;     // VRAM
static volatile bool subDirtyFlag;
static int dirtyY0 = 0, dirtyY1 = 192;     // rows to copy to VRAM

static inline void mark_rows(int y0, int y1) {
	if (y0 < 0) y0 = 0;
	if (y1 > 192) y1 = 192;
	if (y0 >= y1) return;
	if (!subDirtyFlag) { dirtyY0 = y0; dirtyY1 = y1; }
	else { if (y0 < dirtyY0) dirtyY0 = y0; if (y1 > dirtyY1) dirtyY1 = y1; }
	subDirtyFlag = true;
}

void sub_init(void) {
	videoSetModeSub(MODE_5_2D);
	int bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	vfb = bgGetGfxPtr(bg);
	fb = malloc(256 * 192 * 2);
	sub_clear(0x8000);
}

u16 *sub_fb(void) { return fb; }

void sub_clear(u16 color) {
	u32 c = (color | 0x8000) | ((u32)(color | 0x8000) << 16);
	u32 *p = (u32 *)fb;
	for (int i = 0; i < 256 * 192 / 2; i++) p[i] = c;
	mark_rows(0, 192);
}

// only the rows that changed are copied (a full copy takes ~2.5 ms)
void sub_present(void) {
	if (!subDirtyFlag) return;
	int n = (dirtyY1 - dirtyY0) * 512;
	DC_FlushRange(fb + dirtyY0 * 256, n);
	dmaCopyWords(3, fb + dirtyY0 * 256, vfb + dirtyY0 * 256, n);
	subDirtyFlag = false;
}

void sub_mark(void) { mark_rows(0, 192); }

void sub_fill(int x, int y, int w, int h, u16 color) {
	mark_rows(y, y + h);
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > 256) w = 256 - x;
	if (y + h > 192) h = 192 - y;
	color |= 0x8000;
	for (int j = 0; j < h; j++) {
		u16 *p = fb + (y + j) * 256 + x;
		for (int i = 0; i < w; i++) p[i] = color;
	}
}

static inline u16 blend15(u16 d, u16 s, int a) {
	a += a >> 4;                 // 0..31 -> 0..32
	int r = ((s & 31) * a + (d & 31) * (32 - a)) >> 5;
	int g = (((s >> 5) & 31) * a + ((d >> 5) & 31) * (32 - a)) >> 5;
	int b = (((s >> 10) & 31) * a + ((d >> 10) & 31) * (32 - a)) >> 5;
	return r | (g << 5) | (b << 10) | 0x8000;
}

void sub_fill_blend(int x, int y, int w, int h, u16 color, int a) {
	mark_rows(0, 192);
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > 256) w = 256 - x;
	if (y + h > 192) h = 192 - y;
	for (int j = 0; j < h; j++) {
		u16 *p = fb + (y + j) * 256 + x;
		for (int i = 0; i < w; i++) p[i] = blend15(p[i], color, a);
	}
}

bool img_load(Img16 *img, const char *path) {
	u32 size;
	u8 *b = file_load(path, &size);
	if (!b) { img->px = NULL; img->w = img->h = 0; return false; }
	img->w = b[0] | (b[1] << 8);
	img->h = b[2] | (b[3] << 8);
	img->px = malloc(img->w * img->h * 2);
	memcpy(img->px, b + 4, img->w * img->h * 2);
	free(b);
	return true;
}

void img_free(Img16 *img) {
	if (img->px) free(img->px);
	img->px = NULL;
}

void sub_blit(const Img16 *img, int x, int y) {
	mark_rows(0, 192);
	if (!img->px) return;
	for (int j = 0; j < img->h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= 192) continue;
		const u16 *s = img->px + j * img->w;
		u16 *d = fb + yy * 256;
		for (int i = 0; i < img->w; i++) {
			int xx = x + i;
			if (xx < 0 || xx >= 256) continue;
			if (s[i] & 0x8000) d[xx] = s[i];
		}
	}
}

void sub_blit_alpha(const Img16 *img, int x, int y, int a) {
	mark_rows(0, 192);
	if (!img->px || a <= 0) return;
	if (a > 31) a = 31;
	for (int j = 0; j < img->h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= 192) continue;
		const u16 *s = img->px + j * img->w;
		u16 *d = fb + yy * 256;
		for (int i = 0; i < img->w; i++) {
			int xx = x + i;
			if (xx < 0 || xx >= 256) continue;
			if (s[i] & 0x8000) d[xx] = blend15(d[xx], s[i], a);
		}
	}
}

void sub_rect(int x, int y, int w, int h, u16 color) {
	sub_fill(x, y, w, 1, color);
	sub_fill(x, y + h - 1, w, 1, color);
	sub_fill(x, y, 1, h, color);
	sub_fill(x + w - 1, y, 1, h, color);
}

void sub_blit_flip(const Img16 *img, int x, int y) {
	mark_rows(0, 192);
	if (!img->px) return;
	for (int j = 0; j < img->h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= 192) continue;
		const u16 *s = img->px + j * img->w;
		u16 *d = fb + yy * 256;
		for (int i = 0; i < img->w; i++) {
			int xx = x + img->w - 1 - i;
			if (xx < 0 || xx >= 256) continue;
			if (s[i] & 0x8000) d[xx] = s[i];
		}
	}
}

// multiplies the image by a tint color, only the first clipw columns
void sub_blit_tint(const Img16 *img, int x, int y, int clipw, u16 tint) {
	mark_rows(0, 192);
	if (!img->px) return;
	int tr = (tint & 31) + 1, tg = ((tint >> 5) & 31) + 1, tb = ((tint >> 10) & 31) + 1;
	if (clipw > img->w) clipw = img->w;
	for (int j = 0; j < img->h; j++) {
		int yy = y + j;
		if (yy < 0 || yy >= 192) continue;
		const u16 *s = img->px + j * img->w;
		u16 *d = fb + yy * 256;
		for (int i = 0; i < clipw; i++) {
			int xx = x + i;
			if (xx < 0 || xx >= 256) continue;
			u16 c = s[i];
			if (!(c & 0x8000)) continue;
			int r = ((c & 31) * tr) >> 5, g = (((c >> 5) & 31) * tg) >> 5, b = (((c >> 10) & 31) * tb) >> 5;
			d[xx] = r | (g << 5) | (b << 10) | 0x8000;
		}
	}
}

void sub_text(int fid, int x, int y, const char *s, u16 color) {
	Font *f = font_get(fid);
	if (!f->alpha) font_load(fid, false);
	if (!f->alpha) return;
	const FontDef *d = font_defs[fid];
	mark_rows(y - 4, y + d->lineh + 6);
	int aw = 8 << d->wlog;
	// glyph texel (A3I5: 3 bit alpha, 5 bit luminance) -> color, per call
	static u16 lum[32];
	static const u8 alphaOf[8] = { 0, 4, 9, 13, 18, 22, 27, 31 };
	for (int l = 0; l < 32; l++) {
		int k = l + 1;
		lum[l] = (((color & 31) * k) >> 5) | (((((color >> 5) & 31) * k) >> 5) << 5) | (((((color >> 10) & 31) * k) >> 5) << 10);
	}
	for (; *s; s++) {
		int c = (u8)*s;
		if (c < 32 || c > 126) c = '?';
		const Glyph *g = &d->glyphs[c - 32];
		int x0 = x + g->ox;
		for (int j = 0; j < g->h; j++) {
			int yy = y + g->oy + j;
			if (yy < 0 || yy >= 192) continue;
			const u8 *a = f->alpha + (g->y + j) * aw + g->x;
			u16 *p = fb + yy * 256;
			for (int i = 0; i < g->w; i++) {
				int v = a[i];
				if (!v) continue;
				int xx = x0 + i;
				if ((unsigned)xx >= 256) continue;
				int al = alphaOf[v >> 5];
				u16 sc = lum[v & 31];
				p[xx] = al >= 31 ? (sc | 0x8000) : blend15(p[xx], sc, al);
			}
		}
		x += g->adv;
	}
}

void sub_text_centered(int fid, int cx, int y, const char *s, u16 color) {
	sub_text(fid, cx - font_width(fid, s) / 2, y, s, color);
}
