// PlayState (V-Slice semantics)
#pragma once
#include "engine.h"
#include "character.h"

enum { NS_PENDING, NS_HELD, NS_DONE, NS_MISSED };

typedef struct {
	float t, len;
	u8 dir, kind, state, pad;
	float holdEnd;          // song position the hold was released / ended
} Note;

typedef struct {
	Note *notes;
	int n, first;
	bool cpu;
	int anim[4];            // 0 static, 1 press, 2 confirm
	float animTime[4];
	float cpuRelease[4];    // cpu: time the confirm animation ends
	float cx[4];            // lane centers (DS px)
	float scale;            // lane scale (1 = 30 DS px per lane)
	bool holding[4];
} Strum;

typedef struct {
	float x, y, vx, vy, ay;
	float alpha, fadeDelay;
	int frame;
	bool used;
} Popup;

typedef struct {
	const SongInfo *info;
	u8 *chartData;
	const ChartHeader *chart;
	const TimeChange *times;
	const ChartEvent *events;
	Character chr[3];
	Character *chars[3];    // ROLE_BF, ROLE_GF, ROLE_DAD
	float songPos;
	bool started, ended, paused, dead;
	int countdownStep;
	int curStep, curBeat;
	float stepF, beatF;
	float stepMs, crochet;
	float scrollSpeed;
	int scrollTween;
	Strum sl[2];            // 0 player, 1 opponent
	// camera
	Cam cam;
	float camX, camY, followX, followY;
	bool followEnabled;
	int camTweenId;
	float currentZoom, stageZoom, bopMult, bopIntensity, hudZoom, hudBop;
	float zoomRate, zoomOffset;
	int zoomTween;
	// score
	float health;
	int score, misses, combo, maxCombo;
	int judged[4];
	int evIdx;
	Popup pops[16];
	struct { bool used; int lane, var; float t; } splash[6];
	float iconScale[2];
	const NoteSkin *skin;
	const SplashDef *splashDef;
	Sheet shNotes, shSplash, shPopup, shCountdown, shIcon[2];
	float countdownAlpha;
	int countdownFrame;
	bool hideOpp;
	float pauseA;            // pause dim (fades in and out)
	float flashA;            // camGame flash
	u16 flashCol;
	float flashDur;
	Character abot;         // Nene's speaker
	// dialogue (week 6)
	const Conversation *dlg;
	int dlgLine;
	float dlgChars, dlgFade;
	Sheet shDlgSpk[4], shDlgBox;
	int dlgSpkId[4];
	int dlgBoxId;
} PlayState;

extern PlayState P;

// stage.c
void stage_load(const StageDef *d);
void stage_unload(void);
void stage_update(float dt);
void stage_beat(int beat);
void stage_step(int step);
void stage_draw(const Cam *cam);
void stage_prop_play(const char *prop, const char *anim, bool force);
void play_flash(u16 color, float dur);   // FlxG.camera.flash on the game camera
