// Stage: props from data/stages/*.json (converted to PropDef tables) drawn
// together with the characters, sorted by zIndex, plus the behaviour of the
// stage scripts (preload/scripts/stages/*.hxc). Every layer gets a stereo
// depth from its scroll factor, and its blend mode (add / multiply overlays).
#include "play.h"

typedef struct {
	const PropDef *def;
	Sheet *sh;
	int anim;
	float t;
	int frame;
	bool finished, hasDanced, visible;
	float x, y, alpha;
	u32 color;
} Prop;

#define MAX_PROPS 40
static Prop props[MAX_PROPS];
static int nProps;
static Sheet sheets[MAX_PROPS];
static const SheetDef *sheetDefs[MAX_PROPS];
static int nSheets;
static const StageDef *stage;
static int order[MAX_PROPS + 3];
static int nOrder;

// script state
static int lightningBeat, lightningOffset;
static float lightAlpha;
static int trainCars, trainCooldown;
static bool trainMoving, trainFinishing, trainStarted;
static float trainTime, trainFrame;
static bool carCanDrive;
static float carTime;
static u32 lightColor;

static bool is_stage(const char *id) { return stage && !strcmp(stage->id, id); }

Prop *stage_prop_ptr(const char *name) {
	for (int i = 0; i < nProps; i++) if (!strcmp(props[i].def->name, name)) return &props[i];
	return NULL;
}

static int prop_find_anim(Prop *p, const char *name) {
	for (int i = 0; i < p->def->nAnims; i++) if (!strcmp(p->def->anims[i].name, name)) return i;
	return -1;
}

void stage_prop_play(const char *prop, const char *anim, bool force) {
	Prop *p = stage_prop_ptr(prop);
	if (!p) return;
	int a = prop_find_anim(p, anim);
	if (a < 0) return;
	if (a == p->anim && !force && !p->finished) return;
	p->anim = a;
	p->t = 0;
	p->frame = 0;
	p->finished = false;
}

static void prop_play_idx(Prop *p, int a, bool force) {
	if (a < 0) return;
	if (a == p->anim && !force && !p->finished) return;
	p->anim = a;
	p->t = 0;
	p->frame = 0;
	p->finished = false;
}

// Bopper.dance(true) for props
static void prop_dance(Prop *p) {
	int l = prop_find_anim(p, "danceLeft");
	if (l >= 0) {
		int r = prop_find_anim(p, "danceRight");
		prop_play_idx(p, p->hasDanced ? r : l, true);
		p->hasDanced = !p->hasDanced;
	} else prop_play_idx(p, prop_find_anim(p, "idle"), true);
}

static void sort_order(void) {
	nOrder = 0;
	for (int i = 0; i < nProps; i++) order[nOrder++] = i;
	for (int c = 0; c < 3; c++) order[nOrder++] = 1000 + c;
	// insertion sort by z (stable)
	for (int i = 1; i < nOrder; i++) {
		int v = order[i];
		int zv = v >= 1000 ? stage->chars[v - 1000].z : props[v].def->z;
		int j = i - 1;
		while (j >= 0) {
			int o = order[j];
			int zo = o >= 1000 ? stage->chars[o - 1000].z : props[o].def->z;
			if (zo <= zv) break;
			order[j + 1] = o;
			j--;
		}
		order[j + 1] = v;
	}
}

void stage_load(const StageDef *d) {
	stage = d;
	nProps = 0;
	nSheets = 0;
	for (int i = 0; i < d->nProps && nProps < MAX_PROPS; i++) {
		const PropDef *pd = &d->props[i];
		Prop *p = &props[nProps++];
		memset(p, 0, sizeof(*p));
		p->def = pd;
		p->x = pd->x;
		p->y = pd->y;
		p->alpha = pd->alpha;
		p->color = pd->color;
		p->visible = true;
		p->anim = -1;
		if (pd->sheet) {
			int k;
			for (k = 0; k < nSheets; k++) if (sheetDefs[k] == pd->sheet) break;
			if (k == nSheets) {
				sheetDefs[k] = pd->sheet;
				sheet_load(&sheets[k], pd->sheet);
				nSheets++;
			}
			p->sh = &sheets[k];
		}
		if (pd->kind == PROP_ANIM) prop_play_idx(p, pd->startAnim, true);
	}
	sort_order();
	lightningBeat = 0;
	lightningOffset = 8;
	lightAlpha = 0;
	trainCars = 8;
	trainCooldown = 0;
	trainMoving = trainFinishing = trainStarted = false;
	carCanDrive = true;
	lightColor = WHITE;
	Prop *t = stage_prop_ptr("train");
	if (t && is_stage("phillyTrain")) t->x = 1280 + 200;
	Prop *car = stage_prop_ptr("fastCar");
	if (car) car->x = -12600;
}

void stage_unload(void) {
	for (int i = 0; i < nSheets; i++) sheet_unload(&sheets[i]);
	nSheets = 0;
	nProps = 0;
	stage = NULL;
}

// ---------------------------------------------------------------------------
// scripts
// ---------------------------------------------------------------------------
static void lightning(bool sound, int beat) {
	if (sound) audio_sfx(rand() & 1 ? SFX_THUNDER1 : SFX_THUNDER2, 1);
	stage_prop_play("halloweenBG", "lightning", true);
	play_flash(WHITE, 0.35f);
	lightningBeat = beat;
	lightningOffset = irand_range(8, 24);
	Character *bf = P.chars[ROLE_BF], *gf = P.chars[ROLE_GF];
	if (bf && chr_has_anim(bf, "scared") && strcmp(chr_anim_name(bf), "cheer")) chr_play(bf, "scared", true);
	if (gf && chr_has_anim(gf, "scared")) chr_play(gf, "scared", true);
}

static void train_reset(void) {
	if (trainStarted && P.chars[ROLE_GF]) chr_play(P.chars[ROLE_GF], "hairFall", false);
	Prop *t = stage_prop_ptr("train");
	if (t) t->x = 1280 + 200;
	trainMoving = false;
	trainCars = 8;
	trainFinishing = false;
	trainStarted = false;
}

static void car_reset(void) {
	Prop *car = stage_prop_ptr("fastCar");
	if (!car) return;
	car->x = -12600;
	car->y = irand_range(140, 250);
	carCanDrive = true;
	carTime = -1;
}

static float carVel;

void stage_beat(int beat) {
	if (!stage) return;
	if (is_stage("spookyMansion")) {
		if (beat == 4 && !strcmp(song_info[g_song].id, "spookeez")) lightning(false, beat);
		if ((rand() % 100) < 10 && beat > lightningBeat + lightningOffset) lightning(true, beat);
	} else if (is_stage("phillyTrain")) {
		if (!trainMoving) trainCooldown++;
		if (beat % 8 == 4 && (rand() % 100) < 30 && !trainMoving && trainCooldown > 8) {
			trainCooldown = irand_range(-4, 0);
			trainMoving = true;
			trainTime = 0;
			trainFrame = 0;
			audio_sfx(SFX_TRAIN, 1);
		}
		if (beat % 4 == 0) {
			static const u32 cols[5] = { 0x31A2FD, 0x31FD8C, 0xFB33F5, 0xFBA633, 0xFD4531 };
			lightColor = HEXC(cols[rand() % 5]);
			lightAlpha = 1;
		}
	} else if (is_stage("limoRide")) {
		if ((rand() % 100) < 10 && carCanDrive) {
			Prop *car = stage_prop_ptr("fastCar");
			if (car) {
				audio_sfx(SFX_CARPASS, 0.7f);
				carVel = irand_range(170, 220) * 60.0f * 3;
				carCanDrive = false;
				carTime = 2;
			}
		}
	}
}

void stage_step(int step) {
	if (!stage || step < 0) return;
	for (int i = 0; i < nProps; i++) {
		Prop *p = &props[i];
		float de = p->def->danceEvery;
		if (de <= 0 || p->def->kind != PROP_ANIM) continue;
		int every = (int)(de * 4 + 0.5f);
		if (every > 0 && step % every == 0) prop_dance(p);
	}
}

void stage_update(float dt) {
	if (!stage) return;
	for (int i = 0; i < nProps; i++) {
		Prop *p = &props[i];
		if (p->anim < 0 || p->finished) continue;
		const PropAnim *a = &p->def->anims[p->anim];
		p->t += dt;
		int f = (int)(p->t * a->fps);
		if (f >= a->n) {
			if (a->loop) f %= a->n;
			else { f = a->n - 1; p->finished = true; }
		}
		p->frame = f;
	}
	if (is_stage("phillyTrain")) {
		lightAlpha -= (P.crochet / 1000) * dt * 1.5f;
		if (lightAlpha < 0) lightAlpha = 0;
		Prop *l = stage_prop_ptr("lights");
		if (l) { l->color = lightColor; l->alpha = lightAlpha; }
		if (trainMoving) {
			trainTime += dt;
			trainFrame += dt;
			if (trainFrame >= 1.0f / 24) {
				trainFrame = 0;
				if (trainTime >= 4.7f) {
					if (!trainStarted && P.chars[ROLE_GF]) chr_play(P.chars[ROLE_GF], "hairBlow", false);
					trainStarted = true;
				}
				if (trainStarted) {
					Prop *t = stage_prop_ptr("train");
					if (t) {
						t->x -= 400;
						if (t->x < -2000 && !trainFinishing) {
							t->x = -1150;
							if (--trainCars <= 0) trainFinishing = true;
						}
						if (t->x < -4000 && trainFinishing) train_reset();
					}
				}
			}
		}
	} else if (is_stage("limoRide")) {
		Prop *car = stage_prop_ptr("fastCar");
		if (car && !carCanDrive) {
			car->x += carVel * dt;
			carTime -= dt;
			if (carTime <= 0) car_reset();
		}
	}
}

void stage_draw(const Cam *cam) {
	if (!stage) return;
	float k = cam->zoom * KH;
	for (int i = 0; i < nOrder; i++) {
		int o = order[i];
		if (o >= 1000) {
			Character *c = P.chars[o - 1000];
			gfx_blend(BLEND_NORMAL);
			if (o - 1000 == ROLE_GF && P.abot.loaded) { gfx_depth(depth_of(P.abot.sfx)); chr_draw(&P.abot, cam); }
			if (c && c->loaded) { gfx_depth(depth_of(c->sfx)); chr_draw(c, cam); }
			continue;
		}
		Prop *p = &props[o];
		const PropDef *d = p->def;
		if (!p->visible || p->alpha <= 0.01f) continue;
		gfx_depth(depth_of(d->sfx));
		gfx_blend(d->blend);
		float sx = cam_sx(cam, p->x, d->sfx), sy = cam_sy(cam, p->y, d->sfy);
		if (d->kind == PROP_SOLID) {
			gfx_rect(sx, sy, cam_sx(cam, p->x + d->w, d->sfx), cam_sy(cam, p->y + d->h, d->sfy), d->color, AL(p->alpha));
			continue;
		}
		if (!p->sh || !p->sh->loaded) continue;
		int frame = 0;
		float ox = 0, oy = 0;
		if (d->kind == PROP_ANIM && p->anim >= 0) {
			const PropAnim *a = &d->anims[p->anim];
			frame = a->seq[p->frame];
			ox = a->offx;
			oy = a->offy;
		}
		sheet_draw(p->sh, frame, cam_sx(cam, p->x + ox, d->sfx), cam_sy(cam, p->y + oy, d->sfy), k, p->color, AL(p->alpha));
	}
	gfx_blend(BLEND_NORMAL);
	gfx_depth(0);
}
