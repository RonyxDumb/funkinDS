#pragma once
#include "engine.h"

typedef struct {
	float camX, camY;      // camera center (world) = FlxCamera.scroll + half size
	float zoom;            // effective zoom
	float shakeX, shakeY;  // DS px
} Cam;

// world -> DS screen (flixel camera, zoom around the screen center)
static inline float cam_sx(const Cam *c, float wx, float sf) {
	return (wx - c->camX * sf + 640.0f * sf - 640.0f) * c->zoom * KDS + 128.0f + c->shakeX;
}
static inline float cam_sy(const Cam *c, float wy, float sf) {
	return (wy - c->camY * sf + 360.0f * sf - 360.0f) * c->zoom * KDS + 96.0f + c->shakeY;
}

typedef struct {
	u32 off, size;
	u8 wlog, hlog;
	const void *owner;
	int frame;
	u8 *buf;               // decompressed frame
} ChrSlot;

enum { ROLE_BF, ROLE_GF, ROLE_DAD, ROLE_OTHER };

typedef struct {
	char id[32];            // character id (without the _p / _dead suffix)
	u8 *data;               // whole .chr file
	const ChrHeader *hdr;
	const ChrAnimRec *anims;
	const u16 *seq;
	const ChrFrameRec *frames;
	u32 palAddr, palParam;
	ChrSlot *slot;
	int role;
	float x, y;             // sprite position (top-left of the hitbox)
	float sfx, sfy;         // scroll factors
	float alpha;
	u16 color;
	bool visible;
	int anim;
	float animTime;
	int frameIdx;
	bool finished;
	float holdTimer;
	bool hasDanced;
	char idleSuffix[8];
	float camFocusX, camFocusY;     // cameraFocusPoint
	bool dead;
	bool loaded;
} Character;

void chr_slots_reset(void);
ChrSlot *chr_slot_new(int wlog, int hlog);
bool chr_load(Character *c, const char *tag, int role);
void chr_unload(Character *c);
void chr_place(Character *c, const StageChar *sc);
int chr_find_anim(Character *c, const char *name);
bool chr_has_anim(Character *c, const char *name);
const char *chr_anim_name(Character *c);
void chr_play(Character *c, const char *name, bool force);
void chr_play_any(Character *c, const char *name);   // name, or name1 / name2 alternating
void chr_dance(Character *c, bool force);
bool chr_is_singing(Character *c);
void chr_sing(Character *c, int dir, bool miss, const char *suffix);
void chr_update(Character *c, float dt, float stepMs, bool holding);
void chr_step(Character *c, int step);
void chr_draw(Character *c, const Cam *cam);
