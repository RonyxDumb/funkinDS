// V-Slice BaseCharacter / Bopper animation semantics on top of the .chr files
// written by tools/convert.py (frames are LZ77 compressed in RAM and
// decompressed into a VRAM slot when they change).
#include "character.h"

static const char *dirNames[4] = { "LEFT", "DOWN", "UP", "RIGHT" };

static ChrSlot slots[8];
static int nSlots;

void chr_slots_reset(void) {
	for (int i = 0; i < nSlots; i++) free(slots[i].buf);
	nSlots = 0;
	gfx_chr_reset();
}

ChrSlot *chr_slot_new(int wlog, int hlog) {
	if (nSlots >= 8) return NULL;
	u32 size = (8 << wlog) * (8 << hlog);
	u32 off = gfx_chr_slot(size);
	if (off == 0xFFFFFFFF) return NULL;
	ChrSlot *s = &slots[nSlots];
	s->off = off;
	s->size = size;
	s->wlog = wlog;
	s->hlog = hlog;
	s->owner = NULL;
	s->frame = -1;
	s->buf = malloc(size);
	if (!s->buf) return NULL;
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
	if (memcmp(c->hdr->magic, "CHR1", 4)) { free(c->data); c->data = NULL; return false; }
	c->anims = (const ChrAnimRec *)(c->data + c->hdr->offAnims);
	c->seq = (const u16 *)(c->data + c->hdr->offSeq);
	c->frames = (const ChrFrameRec *)(c->data + c->hdr->offFrames);
	c->palAddr = pal_alloc(512);
	c->palParam = c->palAddr >> 4;
	pal_write(c->palAddr, (const u16 *)(c->data + c->hdr->offPal), 256);
	shader_register((u16 *)(c->data + c->hdr->offPal), c->palAddr, 256);
	c->slot = chr_slot_new(c->hdr->slotW, c->hdr->slotH);
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
	if (c->loaded) shader_unregister(c->palAddr);
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

void chr_draw(Character *c, const Cam *cam) {
	if (!c->loaded || !c->visible || c->anim < 0 || c->alpha <= 0.01f || !c->slot) return;
	const ChrAnimRec *a = &c->anims[c->anim];
	int uf = c->seq[a->start + c->frameIdx];
	const ChrFrameRec *f = &c->frames[uf];
	ChrSlot *s = c->slot;
	if (s->owner != c->data || s->frame != uf) {
		const u8 *src = c->data + c->hdr->offPix + f->off;
		swiDecompressLZSSWram((void *)src, s->buf);
		DC_FlushRange(s->buf, f->w4 * f->h);   // the vblank upload uses DMA
		gfx_queue_upload(s->off, s->buf, f->w4, f->h, 8 << s->wlog);
		s->owner = c->data;
		s->frame = uf;
	}
	float wx0 = c->x + a->offx + f->dx;
	float wy0 = c->y + a->offy + f->dy;
	float x0 = cam_sx(cam, wx0, c->sfx), y0 = cam_sy(cam, wy0, c->sfy);
	float x1 = cam_sx(cam, wx0 + f->ww, c->sfx), y1 = cam_sy(cam, wy0 + f->wh, c->sfy);
	u32 tp = (s->off >> 3) | (s->wlog << 20) | (s->hlog << 23) | (GL_RGB256 << 26) | GL_TEXTURE_COLOR0_TRANSPARENT;
	gfx_quad_raw((int)(x0 * 16), (int)(y0 * 16), (int)(x1 * 16), (int)(y1 * 16), 0, 0, f->cw << 4, f->h << 4,
		tp, c->palParam, c->color, A31(c->alpha));
}
