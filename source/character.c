// V-Slice BaseCharacter / Bopper animation semantics on top of the .chr files
// written by tools/convert.py: frames are 8 bpp (premultiplied RGBA palette),
// LZ77 compressed in RAM, and expanded into the character's texture slot
// when they change.
#include "character.h"

static const char *dirNames[4] = { "LEFT", "DOWN", "UP", "RIGHT" };

#define MAX_SLOTS 10
static ChrSlot slots[MAX_SLOTS];
static int nSlots;

// LZ77 (GBA BIOS type 0x10)
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

// texels expanded per frame: characters whose new frame does not fit keep
// showing their previous frame for one more frame
#define DECOMP_BUDGET (300 * 1024)
static int decompBudget = DECOMP_BUDGET;
void chr_frame_begin(void) { decompBudget = DECOMP_BUDGET; }

void chr_slots_reset(void) {
	for (int i = 0; i < nSlots; i++) {
		free(slots[i].buf);
		tex_delete(slots[i].tex);
	}
	nSlots = 0;
}

ChrSlot *chr_slot_new(int wlog, int hlog, bool nearest) {
	if (nSlots >= MAX_SLOTS) return NULL;
	int w = 8 << wlog, h = 8 << hlog;
	if (w > 1024) w = 1024;
	if (h > 1024) h = 1024;
	ChrSlot *s = &slots[nSlots];
	s->tex = tex_new(w, h, GPU_RGBA8, !nearest);
	if (!s->tex) return NULL;
	memset(s->tex->data, 0, s->tex->size);
	tex_upload_done(s->tex);
	s->w = w;
	s->h = h;
	s->owner = NULL;
	s->frame = -1;
	s->buf = malloc(w * h);
	if (!s->buf) { tex_delete(s->tex); return NULL; }
	nSlots++;
	return s;
}

bool chr_load(Character *c, const char *tag, int role) {
	memset(c, 0, sizeof(*c));
	char path[64];
	snprintf(path, sizeof(path), "/chr/%s.chr", tag);
	u32 size;
	c->data = file_load(path, &size);
	if (!c->data) return false;
	snprintf(c->id, sizeof(c->id), "%s", tag);
	char *u = strstr(c->id, "_p");
	if (u && (u[2] == 0 || u[2] == '_')) *u = 0;
	u = strstr(c->id, "_dead");
	if (u) *u = 0;
	c->hdr = (const ChrHeader *)c->data;
	if (memcmp(c->hdr->magic, "CHR3", 4)) { free(c->data); c->data = NULL; return false; }
	c->anims = (const ChrAnimRec *)(c->data + c->hdr->offAnims);
	c->seq = (const u16 *)(c->data + c->hdr->offSeq);
	c->frames = (const ChrFrameRec *)(c->data + c->hdr->offFrames);
	c->pal = (const u32 *)(c->data + c->hdr->offPal);
	c->slot = chr_slot_new(c->hdr->slotW, c->hdr->slotH, c->hdr->pixel);
	c->role = role;
	c->alpha = 1;
	c->color = WHITE;
	c->sfx = c->sfy = 1;
	c->visible = true;
	c->anim = -1;
	c->loaded = true;
	chr_dance(c, true);
	return true;
}

void chr_unload(Character *c) {
	free(c->data);
	c->data = NULL;
	c->loaded = false;
}

// Stage.addCharacter + BaseCharacter.setScale + resetCameraFocusPoint
void chr_place(Character *c, const StageChar *sc) {
	const ChrHeader *h = c->hdr;
	c->x = sc->x - h->W / 2 + h->gx;
	c->y = sc->y - h->H + h->gy;
	c->camFocusX = c->x + h->W / 2 + h->camx + sc->camx;
	c->camFocusY = c->y + h->H / 2 + h->camy + sc->camy;
	c->sfx = sc->sfx;
	c->sfy = sc->sfy;
}

int chr_find_anim(Character *c, const char *name) {
	if (!c->loaded) return -1;
	for (int i = 0; i < c->hdr->nAnims; i++)
		if (!strcmp(c->anims[i].name, name)) return i;
	return -1;
}

bool chr_has_anim(Character *c, const char *name) { return chr_find_anim(c, name) >= 0; }

const char *chr_anim_name(Character *c) { return c->anim < 0 ? "" : c->anims[c->anim].name; }

static bool ends_with(const char *s, const char *suf) {
	int a = strlen(s), b = strlen(suf);
	return a >= b && !strcmp(s + a - b, suf);
}

// FlxAnimationController.play
void chr_play(Character *c, const char *name, bool force) {
	if (!c->loaded) return;
	int a = chr_find_anim(c, name);
	if (a < 0) return;
	if (a == c->anim && !force && !c->finished) return;
	c->anim = a;
	c->animTime = 0;
	c->frameIdx = 0;
	c->finished = false;
}

void chr_play_any(Character *c, const char *name) {
	static bool alt;
	if (chr_has_anim(c, name)) { chr_play(c, name, true); return; }
	char n[40];
	snprintf(n, sizeof(n), "%s%d", name, alt ? 2 : 1);
	alt = !alt;
	chr_play(c, n, true);
}

bool chr_is_singing(Character *c) {
	const char *n = chr_anim_name(c);
	return !strncmp(n, "sing", 4) && !ends_with(n, "-end");
}

// BaseCharacter.dance -> Bopper.dance
void chr_dance(Character *c, bool force) {
	if (!c->loaded || c->dead) return;
	if (!force) {
		if (chr_is_singing(c)) return;
		const char *n = chr_anim_name(c);
		if (strncmp(n, "dance", 5) && strncmp(n, "idle", 4) && !c->finished && c->anim >= 0) return;
	}
	char n[32];
	snprintf(n, sizeof(n), "danceLeft%s", c->idleSuffix);
	if (chr_has_anim(c, n)) {
		snprintf(n, sizeof(n), c->hasDanced ? "danceRight%s" : "danceLeft%s", c->idleSuffix);
		c->hasDanced = !c->hasDanced;
	} else snprintf(n, sizeof(n), "idle%s", c->idleSuffix);
	chr_play(c, n, force);
}

void chr_sing(Character *c, int dir, bool miss, const char *suffix) {
	char n[40];
	snprintf(n, sizeof(n), "sing%s%s%s%s", dirNames[dir & 3], miss ? "miss" : "", suffix && *suffix ? "-" : "", suffix ? suffix : "");
	if (!chr_has_anim(c, n)) snprintf(n, sizeof(n), "sing%s%s", dirNames[dir & 3], miss ? "miss" : "");
	chr_play(c, n, true);
	c->holdTimer = 0;
}

static void advance(Character *c, float dt) {
	if (c->anim < 0 || c->finished) return;
	const ChrAnimRec *a = &c->anims[c->anim];
	c->animTime += dt;
	int f = (int)(c->animTime * a->fps);
	if (f >= a->n) {
		if (a->loop) f %= a->n;
		else { f = a->n - 1; c->finished = true; }
	}
	c->frameIdx = f;
}

// BaseCharacter.onUpdate
void chr_update(Character *c, float dt, float stepMs, bool holding) {
	if (!c->loaded) return;
	advance(c, dt);
	if (c->dead) return;
	const char *cur = chr_anim_name(c);
	if (c->finished && !ends_with(cur, "-hold")) {
		char n[40];
		snprintf(n, sizeof(n), "%s-hold", cur);
		if (chr_has_anim(c, n)) { chr_play(c, n, false); return; }
	}
	if (chr_is_singing(c)) {
		c->holdTimer += dt;
		float singSec = c->hdr->singTime * stepMs / 1000.0f;
		if (strstr(cur, "miss")) singSec *= 2;
		bool stop = c->role == ROLE_BF ? !holding : true;
		if (c->holdTimer > singSec && stop) {
			c->holdTimer = 0;
			char base[40], n[48];
			snprintf(base, sizeof(base), "%s", cur);
			if (ends_with(base, "-hold")) base[strlen(base) - 5] = 0;
			snprintf(n, sizeof(n), "%s-end", base);
			if (chr_has_anim(c, n)) chr_play(c, n, false);
			else chr_dance(c, true);
		}
	} else c->holdTimer = 0;
}

// Bopper.onStepHit (characters have shouldBop = false)
void chr_step(Character *c, int step) {
	if (!c->loaded || c->dead) return;
	float de = c->hdr->danceEvery;
	if (de <= 0) return;
	int every = (int)(de * 4 + 0.5f);
	if (every <= 0) return;
	if (step >= 0 && step % every == 0) chr_dance(c, false);
}

// expands a decompressed frame (tiled indices, w8 x h8) into the slot texture
static void expand(ChrSlot *s, const u32 *pal, int w8, int h8) {
	u32 *dst = s->tex->data;
	int row = w8 * 8, trow = (s->w / 8) * 64;
	if (w8 > s->w) return;
	for (int ty = 0; ty < h8 / 8 && ty * 8 < s->h; ty++) {
		const u8 *src = s->buf + ty * row;
		u32 *d = dst + ty * trow;
		for (int i = 0; i < row; i += 4) {
			d[i] = pal[src[i]];
			d[i + 1] = pal[src[i + 1]];
			d[i + 2] = pal[src[i + 2]];
			d[i + 3] = pal[src[i + 3]];
		}
	}
	GSPGPU_FlushDataCache(dst, trow * 4 * ((h8 + 7) / 8));
}

void chr_draw(Character *c, const Cam *cam) {
	if (!c->loaded || !c->visible || c->anim < 0 || c->alpha <= 0.01f || !c->slot) return;
	const ChrAnimRec *a = &c->anims[c->anim];
	int uf = c->seq[a->start + c->frameIdx];
	const ChrFrameRec *f = &c->frames[uf];
	ChrSlot *s = c->slot;
	if (s->owner == c->data && s->frame != uf && s->frame >= 0 && decompBudget < f->w8 * f->h8) {
		uf = s->frame;              // over budget: previous frame
		f = &c->frames[uf];
	}
	if (s->owner != c->data || s->frame != uf) {
		const u8 *src = c->data + c->hdr->offPix + f->off;
		u32 t0 = time_ticks();
		lz77_decode(src, s->buf, s->w * s->h);
		expand(s, c->pal, f->w8, f->h8);
		decompBudget -= f->w8 * f->h8;
		prof_add(PF_DECOMP, t0);
		s->owner = c->data;
		s->frame = uf;
		s->offx = a->offx;
		s->offy = a->offy;
	}
	float wx0 = c->x + s->offx + f->dx;
	float wy0 = c->y + s->offy + f->dy;
	float x0 = cam_sx(cam, wx0, c->sfx), y0 = cam_sy(cam, wy0, c->sfy);
	float x1 = cam_sx(cam, wx0 + f->ww, c->sfx), y1 = cam_sy(cam, wy0 + f->wh, c->sfy);
	gfx_quad(s->tex, x0, y0, x1, y1, 0, 0, (float)f->cw / s->w, (float)f->ch / s->h, c->color, AL(c->alpha));
}
