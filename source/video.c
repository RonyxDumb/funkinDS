// Video playback (Weekend 1 cutscenes) into a texture.
// File format (.vid):  "VID3" u16 w, u16 h, u16 fps, u16 n, u32 offs[n+1]
// each frame = 256 RGB565 palette entries + LZ77 (type 0x10) pixels in GPU
// tile order (w, h multiples of 8). Two textures alternate so that the one
// being written is never the one the GPU is reading.
#include "engine.h"

static FILE *vf;
static u32 *offs;
static int vw, vh, vfps, vn;
static bool loopv, active;
static u8 *cbuf, *ibuf;
static u32 cbufSize;
static C3D_Tex *tex[2];
static int front;
static int shown = -1;

static void lz77_decode(const u8 *src, u8 *dst, u32 max) {
	u32 n = src[1] | (src[2] << 8) | (src[3] << 16);
	if (n > max) n = max;
	src += 4;
	u8 *end = dst + n;
	while (dst < end) {
		u32 flags = *src++;
		for (int b = 0; b < 8 && dst < end; b++, flags <<= 1) {
			if (flags & 0x80) {
				u32 b0 = src[0], b1 = src[1];
				src += 2;
				u32 len = (b0 >> 4) + 3;
				const u8 *p = dst - ((((b0 & 15) << 8) | b1) + 1);
				if (dst + len > end) len = end - dst;
				while (len--) *dst++ = *p++;
			} else *dst++ = *src++;
		}
	}
}

bool video_open(const char *path, bool loop) {
	video_close();
	vf = fopen(path, "rb");
	if (!vf) return false;
	u8 hdr[12];
	if (fread(hdr, 1, 12, vf) != 12 || memcmp(hdr, "VID3", 4)) { fclose(vf); vf = NULL; return false; }
	vw = hdr[4] | (hdr[5] << 8);
	vh = hdr[6] | (hdr[7] << 8);
	vfps = hdr[8] | (hdr[9] << 8);
	vn = hdr[10] | (hdr[11] << 8);
	offs = malloc((vn + 1) * 4);
	fread(offs, 4, vn + 1, vf);
	cbufSize = 0;
	for (int i = 0; i < vn; i++)
		if (offs[i + 1] - offs[i] > cbufSize) cbufSize = offs[i + 1] - offs[i];
	cbuf = malloc(cbufSize + 4);
	ibuf = malloc(vw * vh);
	for (int i = 0; i < 2; i++) {
		tex[i] = tex_new(512, 256, GPU_RGB565, true);
		if (tex[i]) { memset(tex[i]->data, 0, tex[i]->size); tex_upload_done(tex[i]); }
	}
	loopv = loop;
	front = 0;
	shown = -1;
	active = tex[0] && tex[1] && cbuf && ibuf;
	if (!active) video_close();
	return active;
}

void video_close(void) {
	if (vf) fclose(vf);
	vf = NULL;
	free(offs);
	offs = NULL;
	free(cbuf);
	cbuf = NULL;
	free(ibuf);
	ibuf = NULL;
	for (int i = 0; i < 2; i++) { tex_delete(tex[i]); tex[i] = NULL; }
	active = false;
	shown = -1;
}

bool video_active(void) { return active; }
float video_duration(void) { return active ? vn * 1000.0f / vfps : 0; }

static void decode_frame(int f) {
	u32 size = offs[f + 1] - offs[f];
	fseek(vf, offs[f], SEEK_SET);
	if (fread(cbuf, 1, size, vf) != size) return;
	lz77_decode(cbuf + 512, ibuf, vw * vh);
	const u16 *pal = (const u16 *)cbuf;
	C3D_Tex *t = tex[front ^ 1];
	u16 *dst = t->data;
	int tilesW = vw / 8, row = tilesW * 64;
	for (int ty = 0; ty < vh / 8; ty++) {
		const u8 *s = ibuf + ty * row;
		u16 *d = dst + ty * (512 / 8) * 64;
		for (int i = 0; i < row; i++) d[i] = pal[s[i]];
	}
	tex_upload_done(t);
	front ^= 1;
	shown = f;
}

bool video_update(float ms) {
	if (!active) return false;
	int f = (int)(ms * vfps / 1000.0f);
	if (f < 0) f = 0;
	if (f >= vn) {
		if (!loopv) return false;
		f %= vn;
	}
	if (f != shown) decode_frame(f);
	return true;
}

void video_draw_frame(float x0, float y0, float x1, float y1, int alpha) {
	if (!active || shown < 0) return;
	gfx_quad(tex[front], x0, y0, x1, y1, 0, 0, vw / 512.0f, vh / 256.0f, WHITE, alpha);
}
