// GPU renderer (citro3d): every draw call appends a textured quad to the
// draw list of the current screen; the lists are rendered at the end of the
// frame. The top screen list is drawn once per eye in stereoscopic 3D, each
// vertex carrying its own depth (stage layers get it from their scroll factor).
//
// Textures are premultiplied alpha, in the GPU tiled order (tools/convert.py
// writes them so; character frames and videos are expanded at run time).
#include "engine.h"
#include "render_shbin.h"
#include <zlib.h>

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct { float x, y, z, u, v; u32 c; } Vtx;
#define MAX_VTX (6 * 10000)
#define MAX_BATCH 3000

typedef struct { C3D_Tex *tex; u8 blend; int first, count; } Batch;

static DVLB_s *dvlb;
static shaderProgram_s prog;
static int uProj, uEye;
static C3D_Mtx projTop, projBot;
static C3D_RenderTarget *tgtTop, *tgtTopR, *tgtBot;
static Vtx *vbuf;
static int nv;
static Batch batches[2][MAX_BATCH];
static int nb[2];
static int curScreen;
static float curDepth;
static int curBlend;
static float stereo;
C3D_Tex *g_whiteTex;

// deferred texture deletes (the GPU may still read them)
#define MAX_PENDING 256
static C3D_Tex *pending[MAX_PENDING];
static int nPending;

// ---------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------
static u8 mortonX[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
static u8 mortonY[8] = { 0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A };

u32 tex_tiled_offset(int x, int y, int w) {
	return ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + mortonX[x & 7] + mortonY[y & 7];
}

C3D_Tex *tex_new(int w, int h, GPU_TEXCOLOR fmt, bool linear) {
	C3D_Tex *t = malloc(sizeof(C3D_Tex));
	if (!t) return NULL;
	if (!C3D_TexInit(t, w, h, fmt)) {
		// out of linear memory: release what is waiting and retry once
		free(t);
		return NULL;
	}
	C3D_TexSetFilter(t, linear ? GPU_LINEAR : GPU_NEAREST, linear ? GPU_LINEAR : GPU_NEAREST);
	C3D_TexSetWrap(t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	return t;
}

void tex_delete(C3D_Tex *t) {
	if (!t) return;
	if (nPending >= MAX_PENDING) {
		// should not happen: wait for the GPU and free everything
		C3D_FrameSync();
		gspWaitForP3D();
		gfx_flush_textures();
	}
	pending[nPending++] = t;
}

void gfx_flush_textures(void) {
	for (int i = 0; i < nPending; i++) {
		C3D_TexDelete(pending[i]);
		free(pending[i]);
	}
	nPending = 0;
}

void tex_upload_done(C3D_Tex *t) { GSPGPU_FlushDataCache(t->data, t->size); }

// ---------------------------------------------------------------------------
// init / frame
// ---------------------------------------------------------------------------
void gfx_init(void) {
	gfxInitDefault();
	gfxSet3D(true);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
	tgtTop = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
	tgtTopR = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16);
	tgtBot = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH16);
	C3D_RenderTargetSetOutput(tgtTop, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	C3D_RenderTargetSetOutput(tgtTopR, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
	C3D_RenderTargetSetOutput(tgtBot, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	dvlb = DVLB_ParseFile((u32 *)render_shbin, render_shbin_size);
	shaderProgramInit(&prog);
	shaderProgramSetVsh(&prog, &dvlb->DVLE[0]);
	C3D_BindProgram(&prog);
	uProj = shaderInstanceGetUniformLocation(prog.vertexShader, "projection");
	uEye = shaderInstanceGetUniformLocation(prog.vertexShader, "eye");

	C3D_AttrInfo *ai = C3D_GetAttrInfo();
	AttrInfo_Init(ai);
	AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
	AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);

	vbuf = linearAlloc(sizeof(Vtx) * MAX_VTX);
	C3D_BufInfo *bi = C3D_GetBufInfo();
	BufInfo_Init(bi);
	BufInfo_Add(bi, vbuf, sizeof(Vtx), 3, 0x210);

	Mtx_OrthoTilt(&projTop, 0.0f, 400.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);
	Mtx_OrthoTilt(&projBot, 0.0f, 320.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);

	// fragment: texture * vertex color (premultiplied)
	C3D_TexEnv *env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(true, GPU_GREATER, 0);

	g_whiteTex = tex_new(8, 8, GPU_RGBA8, false);
	memset(g_whiteTex->data, 0xFF, g_whiteTex->size);
	tex_upload_done(g_whiteTex);
}

void gfx_exit(void) {
	gfx_flush_textures();
	C3D_Fini();
	gfxExit();
}

float gfx_stereo(void) { return stereo; }

void gfx_frame_begin(void) {
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	gfx_flush_textures();
	nv = 0;
	nb[0] = nb[1] = 0;
	curScreen = SCREEN_TOP;
	curDepth = 0;
	curBlend = BLEND_NORMAL;
	stereo = osGet3DSliderState() * save.depth3d / 100.0f;
	if (stereo < 0.02f) stereo = 0;
}

void gfx_screen(int s) { curScreen = s; curDepth = 0; curBlend = BLEND_NORMAL; }
void gfx_depth(float d) { curDepth = d; }
void gfx_blend(int m) { curBlend = m; }

static void set_blend(int m) {
	switch (m) {
	case BLEND_ADD:
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
		break;
	case BLEND_MULTIPLY:
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
		break;
	case BLEND_SCREEN:
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_COLOR, GPU_ZERO, GPU_ONE);
		break;
	default:
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
		break;
	}
}

static void render_list(int scr, float eye) {
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uProj, scr == SCREEN_TOP ? &projTop : &projBot);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uEye, eye, 0, 0, 0);
	int blend = -1;
	C3D_Tex *tex = NULL;
	for (int i = 0; i < nb[scr]; i++) {
		Batch *b = &batches[scr][i];
		if (b->blend != blend) { set_blend(b->blend); blend = b->blend; }
		if (b->tex != tex) { C3D_TexBind(0, b->tex); tex = b->tex; }
		C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
	}
}

void gfx_frame_end(void) {
	GSPGPU_FlushDataCache(vbuf, nv * sizeof(Vtx));
	C3D_RenderTargetClear(tgtTop, C3D_CLEAR_ALL, 0x000000FF, 0);
	C3D_FrameDrawOn(tgtTop);
	render_list(SCREEN_TOP, -stereo);
	if (stereo > 0) {
		C3D_RenderTargetClear(tgtTopR, C3D_CLEAR_ALL, 0x000000FF, 0);
		C3D_FrameDrawOn(tgtTopR);
		render_list(SCREEN_TOP, stereo);
	}
	C3D_RenderTargetClear(tgtBot, C3D_CLEAR_ALL, 0x000000FF, 0);
	C3D_FrameDrawOn(tgtBot);
	render_list(SCREEN_BOTTOM, 0);
	C3D_FrameEnd(0);
}

// ---------------------------------------------------------------------------
// quads
// ---------------------------------------------------------------------------
static inline u32 pack_color(u32 rgb, int a) {
	if (a > 255) a = 255;
	u32 r = ((rgb >> 16) & 255) * a / 255, g = ((rgb >> 8) & 255) * a / 255, b = (rgb & 255) * a / 255;
	return r | (g << 8) | (b << 16) | ((u32)a << 24);
}

static Vtx *alloc_quad(C3D_Tex *t) {
	if (nv + 6 > MAX_VTX) return NULL;
	int s = curScreen;
	Batch *b = nb[s] ? &batches[s][nb[s] - 1] : NULL;
	if (!b || b->tex != t || b->blend != curBlend || b->first + b->count != nv) {
		if (nb[s] >= MAX_BATCH) return NULL;
		b = &batches[s][nb[s]++];
		b->tex = t;
		b->blend = curBlend;
		b->first = nv;
		b->count = 0;
	}
	b->count += 6;
	Vtx *v = vbuf + nv;
	nv += 6;
	return v;
}

// the PICA200 samples t = 0 at the last row in memory: textures are stored
// from the top, so v is flipped here
static inline void setv(Vtx *v, float x, float y, float u, float t, u32 c) {
	v->x = x; v->y = y; v->z = curDepth; v->u = u; v->v = 1.0f - t; v->c = c;
}

void gfx_quad(C3D_Tex *t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 color, int alpha) {
	if (alpha <= 0 || !t) return;
	float lim = curScreen == SCREEN_TOP ? SW : BW;
	if ((x0 >= lim && x1 >= lim) || (x0 <= 0 && x1 <= 0) || (y0 >= SH && y1 >= SH) || (y0 <= 0 && y1 <= 0)) {
		// off screen (stereo shifts by a few pixels at most)
		if (curDepth == 0 || ((x0 > lim + 16 && x1 > lim + 16) || (x0 < -16 && x1 < -16) || (y0 >= SH && y1 >= SH) || (y0 <= 0 && y1 <= 0)))
			return;
	}
	Vtx *v = alloc_quad(t);
	if (!v) return;
	u32 c = pack_color(color, alpha);
	setv(&v[0], x0, y0, u0, v0, c);
	setv(&v[1], x1, y0, u1, v0, c);
	setv(&v[2], x1, y1, u1, v1, c);
	setv(&v[3], x0, y0, u0, v0, c);
	setv(&v[4], x1, y1, u1, v1, c);
	setv(&v[5], x0, y1, u0, v1, c);
}

void gfx_quad4(C3D_Tex *t, const float *p, float u0, float v0, float u1, float v1, u32 color, int alpha) {
	if (alpha <= 0 || !t) return;
	Vtx *v = alloc_quad(t);
	if (!v) return;
	u32 c = pack_color(color, alpha);
	setv(&v[0], p[0], p[1], u0, v0, c);
	setv(&v[1], p[2], p[3], u1, v0, c);
	setv(&v[2], p[4], p[5], u1, v1, c);
	setv(&v[3], p[0], p[1], u0, v0, c);
	setv(&v[4], p[4], p[5], u1, v1, c);
	setv(&v[5], p[6], p[7], u0, v1, c);
}

static void quad_colors(float x0, float y0, float x1, float y1, u32 c00, u32 c10, u32 c11, u32 c01, int alpha) {
	if (alpha <= 0 || x1 <= x0 || y1 <= y0) return;
	Vtx *v = alloc_quad(g_whiteTex);
	if (!v) return;
	u32 a = pack_color(c00, alpha), b = pack_color(c10, alpha), c = pack_color(c11, alpha), d = pack_color(c01, alpha);
	setv(&v[0], x0, y0, 0.5f, 0.5f, a);
	setv(&v[1], x1, y0, 0.5f, 0.5f, b);
	setv(&v[2], x1, y1, 0.5f, 0.5f, c);
	setv(&v[3], x0, y0, 0.5f, 0.5f, a);
	setv(&v[4], x1, y1, 0.5f, 0.5f, c);
	setv(&v[5], x0, y1, 0.5f, 0.5f, d);
}

void gfx_rect(float x0, float y0, float x1, float y1, u32 color, int alpha) {
	if (x1 <= x0 || y1 <= y0) return;
	gfx_quad(g_whiteTex, x0, y0, x1, y1, 0.5f, 0.5f, 0.5f, 0.5f, color, alpha);
}

void gfx_gradient(float x0, float y0, float x1, float y1, u32 top, u32 bottom, int alpha) {
	quad_colors(x0, y0, x1, y1, top, top, bottom, bottom, alpha);
}

void gfx_gradient_h(float x0, float y0, float x1, float y1, u32 left, u32 right, int alpha) {
	quad_colors(x0, y0, x1, y1, left, right, right, left, alpha);
}

// ---------------------------------------------------------------------------
// sheets
// ---------------------------------------------------------------------------
bool sheet_load(Sheet *s, const SheetDef *d) {
	memset(s, 0, sizeof(*s));
	if (!d) return false;
	s->def = d;
	u32 size;
	u8 *buf = file_load(d->file, &size);
	if (!buf) return false;
	s->tex = calloc(d->nTex, sizeof(C3D_Tex));
	for (int i = 0; i < d->nTex; i++) {
		const TexDef *t = &d->tex[i];
		C3D_Tex *tx = &s->tex[i];
		GPU_TEXCOLOR fmt = t->fmt == TF_RGB565 ? GPU_RGB565 : GPU_RGBA8;
		if (!C3D_TexInit(tx, 8 << t->wlog, 8 << t->hlog, fmt)) {
			for (int k = 0; k < i; k++) C3D_TexDelete(&s->tex[k]);
			free(s->tex);
			free(buf);
			memset(s, 0, sizeof(*s));
			return false;
		}
		bool nearest = t->pad & 1;
		C3D_TexSetFilter(tx, nearest ? GPU_NEAREST : GPU_LINEAR, nearest ? GPU_NEAREST : GPU_LINEAR);
		C3D_TexSetWrap(tx, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		uLongf dl = tx->size;
		if (uncompress(tx->data, &dl, buf + t->off, t->size) != Z_OK) memset(tx->data, 0, tx->size);
		tex_upload_done(tx);
	}
	free(buf);
	s->loaded = true;
	return true;
}

void sheet_unload(Sheet *s) {
	if (!s->loaded) return;
	// the textures are freed with the sheet: copy them out for the deferred delete
	for (int i = 0; i < s->def->nTex; i++) {
		C3D_Tex *t = malloc(sizeof(C3D_Tex));
		if (t) { *t = s->tex[i]; tex_delete(t); }
	}
	free(s->tex);
	s->tex = NULL;
	s->loaded = false;
}

static inline void part_uv(const Sheet *s, const PartDef *p, float *u0, float *v0, float *u1, float *v1) {
	const C3D_Tex *t = &s->tex[p->tex];
	float iw = 1.0f / t->width, ih = 1.0f / t->height;
	*u0 = p->u * iw;
	*u1 = (p->u + p->w) * iw;
	*v0 = p->v * ih;
	*v1 = (p->v + p->h) * ih;
}

void sheet_draw_ex(Sheet *s, int frame, float x, float y, float sx, float sy, u32 color, int alpha) {
	if (!s->loaded || alpha <= 0 || frame < 0 || frame >= s->def->nFrames) return;
	const SheetDef *d = s->def;
	const FrameDef *f = &d->frames[frame];
	float ks = sx / d->scale, kt = sy / d->scale;
	for (int i = 0; i < f->count; i++) {
		const PartDef *p = &d->parts[f->first + i];
		float px = x + p->dx * ks, py = y + p->dy * kt;
		float u0, v0, u1, v1;
		part_uv(s, p, &u0, &v0, &u1, &v1);
		gfx_quad(&s->tex[p->tex], px, py, px + p->w * ks, py + p->h * kt, u0, v0, u1, v1, color, alpha);
	}
}

void sheet_draw_flip(Sheet *s, int frame, float x, float y, float sx, float sy, int flip, u32 color, int alpha) {
	if (!s->loaded || alpha <= 0 || frame < 0 || frame >= s->def->nFrames) return;
	const SheetDef *d = s->def;
	const FrameDef *f = &d->frames[frame];
	float ks = sx / d->scale, kt = sy / d->scale;
	for (int i = 0; i < f->count; i++) {
		const PartDef *p = &d->parts[f->first + i];
		float px0 = x + p->dx * ks, py0 = y + p->dy * kt;
		float px1 = px0 + p->w * ks, py1 = py0 + p->h * kt;
		float u0, v0, u1, v1;
		part_uv(s, p, &u0, &v0, &u1, &v1);
		if (flip & 1) { float a = 2 * x - px1, b = 2 * x - px0; px0 = a; px1 = b; float t = u0; u0 = u1; u1 = t; }
		if (flip & 2) { float a = 2 * y - py1, b = 2 * y - py0; py0 = a; py1 = b; float t = v0; v0 = v1; v1 = t; }
		gfx_quad(&s->tex[p->tex], px0, py0, px1, py1, u0, v0, u1, v1, color, alpha);
	}
}

void sheet_draw_rot(Sheet *s, int frame, float x, float y, float sx, float sy, float cx, float cy, float ang, u32 color, int alpha) {
	if (!s->loaded || alpha <= 0 || frame < 0 || frame >= s->def->nFrames) return;
	const SheetDef *d = s->def;
	const FrameDef *f = &d->frames[frame];
	float ks = sx / d->scale, kt = sy / d->scale;
	float cs = cosf(ang), sn = sinf(ang);
	for (int i = 0; i < f->count; i++) {
		const PartDef *p = &d->parts[f->first + i];
		float px0 = x + p->dx * ks, py0 = y + p->dy * kt;
		float px1 = px0 + p->w * ks, py1 = py0 + p->h * kt;
		float c[8] = { px0, py0, px1, py0, px1, py1, px0, py1 };
		for (int k = 0; k < 8; k += 2) {
			float dx = c[k] - cx, dy = c[k + 1] - cy;
			c[k] = cx + dx * cs - dy * sn;
			c[k + 1] = cy + dx * sn + dy * cs;
		}
		float u0, v0, u1, v1;
		part_uv(s, p, &u0, &v0, &u1, &v1);
		gfx_quad4(&s->tex[p->tex], c, u0, v0, u1, v1, color, alpha);
	}
}

void sheet_draw_sub(Sheet *s, int frame, float x0, float y0, float x1, float y1, float fu0, float fv0, float fu1, float fv1, u32 color, int alpha) {
	if (!s->loaded || alpha <= 0 || frame < 0 || frame >= s->def->nFrames) return;
	const SheetDef *d = s->def;
	const PartDef *p = &d->parts[d->frames[frame].first];
	float u0, v0, u1, v1;
	part_uv(s, p, &u0, &v0, &u1, &v1);
	gfx_quad(&s->tex[p->tex], x0, y0, x1, y1, lerpf(u0, u1, fu0), lerpf(v0, v1, fv0), lerpf(u0, u1, fu1), lerpf(v0, v1, fv1), color, alpha);
}

void sheet_draw(Sheet *s, int frame, float x, float y, float scale, u32 color, int alpha) {
	sheet_draw_ex(s, frame, x, y, scale, scale, color, alpha);
}

void sheet_draw_rect(Sheet *s, int frame, float x0, float y0, float x1, float y1, float fv0, float fv1, u32 color, int alpha) {
	sheet_draw_sub(s, frame, x0, y0, x1, y1, 0, fv0, 1, fv1, color, alpha);
}

// bounds of a frame in drawn units at scale 1 (texels / sheet scale)
void sheet_frame_bounds(const Sheet *s, int frame, float *x0, float *y0, float *x1, float *y1) {
	const SheetDef *d = s->def;
	if (!d || frame < 0 || frame >= d->nFrames) { *x0 = *y0 = *x1 = *y1 = 0; return; }
	const FrameDef *f = &d->frames[frame];
	float a = 1e9f, b = 1e9f, c = -1e9f, e = -1e9f;
	for (int i = 0; i < f->count; i++) {
		const PartDef *p = &d->parts[f->first + i];
		if (p->dx < a) a = p->dx;
		if (p->dy < b) b = p->dy;
		if (p->dx + p->w > c) c = p->dx + p->w;
		if (p->dy + p->h > e) e = p->dy + p->h;
	}
	*x0 = a / d->scale; *y0 = b / d->scale; *x1 = c / d->scale; *y1 = e / d->scale;
}

// ---------------------------------------------------------------------------
// fonts (one texture each, glyphs at 1 texel per screen pixel)
// ---------------------------------------------------------------------------
static Sheet fonts[FONT_COUNT];

void font_load(int id) {
	if (!fonts[id].loaded) sheet_load(&fonts[id], font_defs[id]->sheet);
}

Sheet *font_sheet(int id) { return &fonts[id]; }

int font_width(int id, const char *s) {
	const FontDef *d = font_defs[id];
	int w = 0;
	for (; *s; s++) {
		int c = (u8)*s;
		if (c < 32 || c > 126) c = '?';
		w += d->glyphs[c - 32].adv;
	}
	return w;
}

int font_height(int id) { return font_defs[id]->lineh; }

void font_draw_scaled(int id, float x, float y, float sc, const char *s, u32 color, int alpha) {
	Sheet *f = &fonts[id];
	if (!f->loaded) font_load(id);
	if (!f->loaded || alpha <= 0) return;
	const FontDef *d = font_defs[id];
	C3D_Tex *t = &f->tex[0];
	float iw = 1.0f / t->width, ih = 1.0f / t->height;
	if (sc == 1) { x = floorf(x + 0.5f); y = floorf(y + 0.5f); }
	for (; *s; s++) {
		int c = (u8)*s;
		if (c < 32 || c > 126) c = '?';
		const Glyph *g = &d->glyphs[c - 32];
		if (g->w) {
			float x0 = x + g->ox * sc, y0 = y + g->oy * sc;
			gfx_quad(t, x0, y0, x0 + g->w * sc, y0 + g->h * sc, g->x * iw, g->y * ih, (g->x + g->w) * iw, (g->y + g->h) * ih, color, alpha);
		}
		x += g->adv * sc;
	}
}

void font_draw(int id, float x, float y, const char *s, u32 color, int alpha) { font_draw_scaled(id, x, y, 1, s, color, alpha); }

void font_draw_centered(int id, float cx, float y, const char *s, u32 color, int alpha) {
	font_draw(id, cx - font_width(id, s) / 2.0f, y, s, color, alpha);
}

void font_draw_right(int id, float rx, float y, const char *s, u32 color, int alpha) {
	font_draw(id, rx - font_width(id, s), y, s, color, alpha);
}
