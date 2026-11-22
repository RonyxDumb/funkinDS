// Menus: TitleState, MainMenuState, StoryMenuState, FreeplayState, options.
// The top screen shows the 1280x720 view (MS = 0.3125 px per unit, see
// mx()/my()), the bottom screen holds lists and touch buttons.
#include "engine.h"
#include "character.h"

#define MENU_BPM 102.0f
static int lastBeat;

static int menu_beat(void) {
	float ms = audio_music_time();
	return (int)(ms / (60000.0f / MENU_BPM));
}

static void draw_centered(Sheet *sh, int frame, float cx, float cy, float scale, u32 color, int alpha) {
	float x0, y0, x1, y1;
	sheet_frame_bounds(sh, frame, &x0, &y0, &x1, &y1);
	sheet_draw(sh, frame, cx - (x0 + x1) / 2 * scale, cy - (y0 + y1) / 2 * scale, scale, color, alpha);
}

#define YELLOW RGB(0xF9, 0xCF, 0x51)
#define DARKTXT RGB(40, 24, 10)

static void sub_header(const char *title) {
	sub_gradient(0, 0, BW, BH, RGB(36, 22, 66), RGB(12, 8, 26));
	sub_gradient(0, 0, BW, 30, RGB(0xFF, 0xE0, 0x70), RGB(0xE8, 0xB0, 0x30));
	sub_fill(0, 30, BW, 2, RGB(0x8A, 0x5A, 0x10));
	sub_text_centered(FONT_VCR16O, BW / 2, 4, title, WHITE);
}

// ===========================================================================
// Title (TitleState): the logo and Girlfriend are streamed sprites at their
// V-Slice positions (logo at -150,-100, Girlfriend at 512,50)
// ===========================================================================
static Character tLogo, tGf;
static bool titlePressed;
static float pressTimer, titleT;
static const Cam titleCam = { 640, 360, 1, 0, 0 };

void title_enter(void) {
	chr_slots_reset();
	memset(&tLogo, 0, sizeof(tLogo));
	memset(&tGf, 0, sizeof(tGf));
	chr_load(&tGf, "title_gf", ROLE_OTHER);
	chr_load(&tLogo, "title_logo", ROLE_OTHER);
	tLogo.x = -150 + 40;
	tLogo.y = -100 + 30;
	tGf.x = 1280 * 0.4f;
	tGf.y = 720 * 0.07f;
	menu_music(true);
	titlePressed = false;
	titleT = 0;
	if (!g_titleSeen) screen_flash(2);       // skipIntro: FlxG.camera.flash(WHITE, 4)
	g_titleSeen = true;
	lastBeat = -1;
	sub_gradient(0, 0, BW, BH, RGB(0x1A, 0x10, 0x30), RGB(0, 0, 0));
	sub_fill(0, 70, BW, 84, RGB(0x2A, 0x18, 0x50));
	sub_fill(0, 70, BW, 2, YELLOW);
	sub_fill(0, 152, BW, 2, YELLOW);
	sub_text_centered(FONT_VCR16O, BW / 2, 84, "Friday Night Funkin'", WHITE);
	sub_text_centered(FONT_VCR16O, BW / 2, 116, "Nintendo 3DS", YELLOW);
	sub_text_centered(FONT_VCR8, BW / 2, 200, "Unofficial fan port", RGB(120, 110, 140));
	if (!audio_ok()) {
		sub_fill(0, 164, BW, 30, RGB(90, 10, 10));
		sub_text_centered(FONT_VCR8O, BW / 2, 166, "NO SOUND: /3ds/dspfirm.cdc missing", WHITE);
		sub_text_centered(FONT_VCR8, BW / 2, 180, "run DSP1 once to dump the DSP firmware", RGB(255, 200, 200));
	}
	sub_text_centered(FONT_VCR8, BW / 2, 216, "Touch or press START", RGB(170, 160, 200));
}

void title_leave(void) {
	chr_unload(&tLogo);
	chr_unload(&tGf);
	chr_slots_reset();
}

void title_update(float dt) {
	int b = menu_beat();
	if (b != lastBeat) {
		lastBeat = b;
		chr_play(&tLogo, "idle", true);
		chr_dance(&tGf, true);
	}
	titleT += dt;
	chr_update(&tLogo, dt, 100, false);
	chr_update(&tGf, dt, 100, false);
	if (!titlePressed && (kDown & (KEY_START | KEY_A | KEY_TOUCH))) {
		titlePressed = true;
		pressTimer = 0;
		screen_flash(1);
		audio_sfx(SFX_CONFIRM, 0.7f);
	}
	if (titlePressed) {
		pressTimer += dt;
		if (pressTimer > 1.4f) state_switch(ST_MAINMENU);
	}
}

void title_draw(void) {
	gfx_depth(2);
	chr_draw(&tGf, &titleCam);
	gfx_depth(-2);
	chr_draw(&tLogo, &titleCam);
	gfx_depth(-1);
	bool on = !titlePressed || ((int)(pressTimer * 16) & 1);
	float pulse = titlePressed ? 1 : 0.6f + 0.4f * sinf(titleT * 5);
	if (on) font_draw(FONT_VCR24O, mx(100), my(590), "PRESS START", titlePressed ? WHITE : RGB(0x33, 0xFF, 0xFF), AL(pulse));
	gfx_depth(0);
}

// ===========================================================================
// Main menu
// ===========================================================================
static Sheet shBG, shMain;
static int mmSel;
static float mmT, mmCamY, mmFlick;
static bool mmChosen;
static const char *mmNames[4] = { "Story Mode", "Freeplay", "Options", "Credits" };
static const u8 *const mmIdle[4] = { mm_storymode_idle, mm_freeplay_idle, mm_options_idle, mm_credits_idle };
static const u8 *const mmSelSeq[4] = { mm_storymode_sel, mm_freeplay_sel, mm_options_sel, mm_credits_sel };
static const int mmIdleLen[4] = { MM_STORYMODE_IDLE_LEN, MM_FREEPLAY_IDLE_LEN, MM_OPTIONS_IDLE_LEN, MM_CREDITS_IDLE_LEN };
static const int mmSelLen[4] = { MM_STORYMODE_SEL_LEN, MM_FREEPLAY_SEL_LEN, MM_OPTIONS_SEL_LEN, MM_CREDITS_SEL_LEN };
static bool showCredits;

static void mm_sub(void) {
	sub_header("MAIN MENU");
	if (showCredits) {
		static const char *lines[] = { "Friday Night Funkin'", "by The Funkin' Crew:", "ninjamuffin99 - programming", "PhantomArcade - art",
			"evilsk8r - art", "Kawai Sprite - music", "", "Nintendo 3DS port: unofficial", "fan project, not affiliated", "with The Funkin' Crew.", "",
			"B: back" };
		for (int i = 0; i < 12; i++) sub_text_centered(FONT_VCR8O, BW / 2, 42 + i * 16, lines[i], i == 0 ? YELLOW : WHITE);
		return;
	}
	for (int i = 0; i < 4; i++) {
		int y = 44 + i * 48;
		sub_button(40, y, 240, 40, i == mmSel);
		sub_text_centered(FONT_VCR16O, BW / 2, y + 9, mmNames[i], i == mmSel ? DARKTXT : WHITE);
	}
}

void mainmenu_enter(void) {
	sheet_load(&shBG, &sheet_menubg);
	sheet_load(&shMain, &sheet_mainmenu);
	menu_music(false);
	mmT = 0;
	mmChosen = false;
	showCredits = false;
	mmCamY = 120 + mmSel * 160;
	mm_sub();
	g_fade = 0;
}

void mainmenu_leave(void) { sheet_unload(&shBG); sheet_unload(&shMain); }

static void mm_change(int d) {
	mmSel = (mmSel + d + 4) % 4;
	mmT = 0;
	audio_sfx(SFX_SCROLL, 0.5f);
	mm_sub();
}

static void mm_accept(void) {
	if (mmSel == 3) { showCredits = true; audio_sfx(SFX_CONFIRM, 0.7f); mm_sub(); return; }
	mmChosen = true;
	mmFlick = 0;
	audio_sfx(SFX_CONFIRM, 0.7f);
}

void mainmenu_update(float dt) {
	mmT += dt;
	float target = 120 + mmSel * 160;
	mmCamY += (target - mmCamY) * fps_ratio(0.06f, dt);
	if (mmChosen) {
		mmFlick += dt;
		if (mmFlick > 1.0f) {
			if (mmSel == 0) state_switch(ST_STORY);
			else if (mmSel == 1) state_switch(ST_FREEPLAY);
			else { g_optionsReturn = ST_MAINMENU; state_switch(ST_OPTIONS); }
		}
		return;
	}
	if (showCredits) {
		if (kDown & (KEY_B | KEY_A | KEY_TOUCH)) { showCredits = false; audio_sfx(SFX_CANCEL, 0.6f); mm_sub(); }
		return;
	}
	if (kDown & KEY_UP) mm_change(-1);
	if (kDown & KEY_DOWN) mm_change(1);
	if (kDown & KEY_ACCEPT) mm_accept();
	if (kDown & KEY_B) { audio_sfx(SFX_CANCEL, 0.6f); state_switch(ST_TITLE); }
	for (int i = 0; i < 4; i++) {
		if (touch_in(40, 44 + i * 48, 240, 40)) {
			if (i != mmSel) { mmSel = i; mm_sub(); }
			mm_accept();
		}
	}
}

void mainmenu_draw(void) {
	// menuBG (1.2x, scrollFactor 0.17 on the camera following the selected item)
	float sc = MS * 1.2f;
	float off = (mmCamY - 360) * 0.17f * MS;
	gfx_depth(5);
	sheet_draw(&shBG, 0, 200 - 1280 * sc / 2, 120 - 720 * sc / 2 - off + 10, sc, WHITE, 255);
	// the 4 items fit the screen (spacing 160, centered)
	gfx_depth(-1);
	for (int i = 0; i < 4; i++) {
		if (mmChosen && i != mmSel) continue;
		if (mmChosen && ((int)(mmFlick * 12) & 1)) continue;
		bool s = i == mmSel;
		const u8 *seq = s ? mmSelSeq[i] : mmIdle[i];
		int len = s ? mmSelLen[i] : mmIdleLen[i];
		int f = ((int)(mmT * 24)) % len;
		float y = 120 + (i - 1.5f) * 160 * MS * 0.92f;
		draw_centered(&shMain, seq[f], 200, y, MS * 0.92f, WHITE, 255);
	}
	gfx_depth(0);
}

// ===========================================================================
// Story mode (StoryMenuState layout: black bar, yellow band 56..456 with the
// props, level titles from y 480, tracks at y 556, difficulty at 870,480)
// ===========================================================================
static Sheet shStory, shProps[3];
static int smWeek, smDiff = 1;
static float smT, smPropT, smChosenT, smScroll;
static bool smChosen, smPropRight;

static int weektitle_frame(int w) { return STORYMENU_TITLE_TUTORIAL + w; }
static int diff_frame(int d) { return STORYMENU_DIFF_EASY + d; }

static void sm_sub(void) {
	sub_header("STORY MODE");
	char b[96], sc[24];
	for (int i = -2; i <= 2; i++) {
		int w = smWeek + i;
		if (w < 0 || w >= WEEK_COUNT) continue;
		int y = 84 + i * 24;
		bool s = i == 0;
		if (s) sub_button(10, y - 4, 300, 24, true);
		snprintf(b, sizeof(b), "%s", week_info[w].name);
		sub_text_centered(FONT_VCR12O, BW / 2, y, b, s ? DARKTXT : (i == -2 || i == 2) ? RGB(150, 140, 170) : WHITE);
	}
	sub_fill(0, 146, BW, 1, RGB(120, 100, 160));
	sub_button(10, 152, 40, 26, false);
	sub_text_centered(FONT_VCR12O, 30, 157, "<", WHITE);
	sub_button(270, 152, 40, 26, false);
	sub_text_centered(FONT_VCR12O, 290, 157, ">", WHITE);
	snprintf(b, sizeof(b), "%s", diff_name(smDiff));
	for (char *p = b; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
	sub_text_centered(FONT_VCR16O, BW / 2, 154, b, smDiff == 0 ? RGB(120, 255, 120) : smDiff == 1 ? RGB(255, 230, 100) : RGB(255, 100, 100));
	format_score(sc, save.weekScore[smWeek][smDiff]);
	snprintf(b, sizeof(b), "WEEK SCORE: %s", sc);
	sub_text_centered(FONT_VCR12O, BW / 2, 184, b, WHITE);
	b[0] = 0;
	const WeekInfo *wi = &week_info[smWeek];
	for (int i = 0; i < wi->nSongs; i++) {
		if (i) strcat(b, ", ");
		strcat(b, song_info[wi->songs[i]].name);
	}
	sub_text_centered(font_width(FONT_VCR8O, b) > 312 ? FONT_VCR8 : FONT_VCR8O, BW / 2, 204, b, RGB(0xE5, 0x57, 0x77));
	sub_text_centered(FONT_VCR8, BW / 2, 224, "A: play  B: back  L/R: difficulty", RGB(150, 140, 170));
}

static void sm_load_props(void) {
	for (int i = 0; i < 3; i++) sheet_unload(&shProps[i]);
	const WeekProps *wp = &week_props[smWeek];
	for (int i = 0; i < wp->n; i++) sheet_load(&shProps[i], wp->p[i].sheet);
	smPropT = 0;
}

void story_enter(void) {
	sheet_load(&shStory, &sheet_storymenu);
	menu_music(false);
	smChosen = false;
	smT = 0;
	smScroll = smWeek;
	if (save.lastDiff >= 0 && save.lastDiff < 3) smDiff = save.lastDiff;
	sm_load_props();
	sm_sub();
	lastBeat = -1;
	g_fade = 0;
}

void story_leave(void) {
	sheet_unload(&shStory);
	for (int i = 0; i < 3; i++) sheet_unload(&shProps[i]);
}

static void sm_change_week(int d) {
	smWeek = (smWeek + d + WEEK_COUNT) % WEEK_COUNT;
	audio_sfx(SFX_SCROLL, 0.5f);
	sm_load_props();
	sm_sub();
}

static void sm_change_diff(int d) {
	smDiff = (smDiff + d + 3) % 3;
	audio_sfx(SFX_SCROLL, 0.5f);
	sm_sub();
}

static void sm_accept(void) { smChosen = true; smChosenT = 0; audio_sfx(SFX_CONFIRM, 0.7f); smPropT = 0; }

void story_update(float dt) {
	smT += dt;
	smPropT += dt;
	smScroll += (smWeek - smScroll) * fps_ratio(0.3f, dt);
	if (fabsf(smWeek - smScroll) > 3) smScroll = smWeek;
	int b = menu_beat();
	if (b != lastBeat) { lastBeat = b; smPropT = 0; smPropRight = !smPropRight; }
	if (smChosen) {
		smChosenT += dt;
		if (smChosenT > 1.1f) {
			g_storyMode = true;
			g_week = smWeek;
			g_weekPos = 0;
			g_weekScore = 0;
			g_diff = smDiff;
			g_song = week_info[smWeek].songs[0];
			g_cutsceneDone = false;
			save.lastDiff = smDiff;
			state_switch(ST_PLAY);
		}
		return;
	}
	if (kDown & KEY_UP) sm_change_week(-1);
	if (kDown & KEY_DOWN) sm_change_week(1);
	if (kDown & (KEY_LEFT | KEY_L)) sm_change_diff(-1);
	if (kDown & (KEY_RIGHT | KEY_R)) sm_change_diff(1);
	if (kDown & KEY_ACCEPT) sm_accept();
	if (kDown & KEY_B) { audio_sfx(SFX_CANCEL, 0.6f); state_switch(ST_MAINMENU); }
	if (kDown & KEY_TOUCH) {
		if (touch.py >= 32 && touch.py < 78) sm_change_week(touch.py < 56 ? -2 : -1);
		else if (touch.py >= 104 && touch.py < 146) sm_change_week(touch.py < 126 ? 1 : 2);
		else if (touch.py >= 78 && touch.py < 104) sm_accept();
		else if (touch.py >= 150 && touch.py < 180) sm_change_diff(touch.px < BW / 2 ? -1 : 1);
	}
}

void story_draw(void) {
	const WeekInfo *wi = &week_info[smWeek];
	gfx_rect(0, 0, SW, SH, BLACK, 255);
	gfx_depth(1.5f);
	gfx_rect(0, my(56), SW, my(456), HEXC(wi->bg), 255);
	char b[48], sc[24];
	gfx_depth(-1);
	format_score(sc, save.weekScore[smWeek][smDiff]);
	snprintf(b, sizeof(b), "LEVEL SCORE: %s", sc);
	int fid = font_width(FONT_VCR12O, b) + font_width(FONT_VCR12O, wi->name) > SW - 16 ? FONT_VCR8O : FONT_VCR12O;
	font_draw(fid, 4, fid == FONT_VCR12O ? 3 : 5, b, WHITE, 255);
	font_draw_right(fid, SW - 4, fid == FONT_VCR12O ? 3 : 5, wi->name, RGB(0xB0, 0xB0, 0xB0), 255);
	const WeekProps *wp = &week_props[smWeek];
	gfx_depth(0.5f);
	for (int i = 0; i < wp->n; i++) {
		const WeekProp *p = &wp->p[i];
		if (!shProps[i].loaded) continue;
		const u8 *seq = p->seqA;
		int len = p->lenA;
		if (p->dance && smPropRight) { seq = p->seqB; len = p->lenB; }
		if (!p->dance && smChosen && i == 1 && p->lenB) { seq = p->seqB; len = p->lenB; }
		int f = (int)(smPropT * 24 / 2);
		if (f >= len) f = len - 1;
		// LevelProp: x = offsets[0] + FlxG.width * 0.25 * index, y = offsets[1]
		sheet_draw(&shProps[i], seq[f], mx(p->offx + 320 * i), my(p->offy + 56), MS, WHITE, 255);
	}
	gfx_depth(-0.5f);
	// level titles: the selected one at y 480, the others scroll below / above
	for (int k = -1; k < 3; k++) {
		int w = smWeek + k;
		if (w < 0 || w >= WEEK_COUNT) continue;
		u32 col = k == 0 && smChosen && ((int)(smChosenT * 20) & 1) ? RGB(0x33, 0xFF, 0xFF) : WHITE;
		float y = my(480 + (w - smScroll) * 125) + 14;
		if (y > 260 || y < my(456)) continue;
		draw_centered(&shStory, weektitle_frame(w), mx(640), y, MS, col, k == 0 ? 255 : 110);
	}
	// tracks (pink, VCR) and the difficulty selector
	gfx_depth(-1);
	font_draw_centered(FONT_VCR12O, mx(640 - 1280 * 0.35f), my(500), "TRACKS", HEXC(0xE55777), 255);
	for (int i = 0; i < wi->nSongs; i++)
		font_draw_centered(FONT_VCR8O, mx(640 - 1280 * 0.35f), my(500) + 16 + i * 13, song_info[wi->songs[i]].name, HEXC(0xE55777), 255);
	float ax = mx(870), ay = my(480);
	sheet_draw(&shStory, sm_arrows[(kHeld & (KEY_LEFT | KEY_L)) ? 2 : 0], ax, ay, MS, WHITE, 255);
	draw_centered(&shStory, diff_frame(smDiff), mx(1088), ay + 15, MS, WHITE, 255);
	sheet_draw(&shStory, sm_arrows[(kHeld & (KEY_RIGHT | KEY_R)) ? 3 : 1], mx(1240), ay, MS, WHITE, 255);
	gfx_depth(0);
}

// ===========================================================================
// Freeplay (FreeplayState): DJ on the left, song capsules on the right
// ===========================================================================
static Sheet shFpBg, shFpDiff;
static Character dj;
static int fpList[FREEPLAY_COUNT], fpCount;
static int fpSel, fpOpt;
static float fpScroll, fpT;
static bool fpChosen;
static int fpDiffLoaded = -1;

static const FreeplaySong *fp_cur(void) { return &freeplay_songs[fpList[fpSel]]; }
static int fp_opt_count(void) { return fp_cur()->n[save.player]; }
static int fp_opt(void) { return fp_cur()->opt[save.player][fpOpt]; }

static void fp_load_diff(void) {
	int d = fp_opt() & 7;
	if (d == fpDiffLoaded) return;
	sheet_unload(&shFpDiff);
	if (fpdiff_sheets[d]) sheet_load(&shFpDiff, fpdiff_sheets[d]);
	fpDiffLoaded = d;
}

static void fp_pick_opt(int wantDiff) {
	fpOpt = 0;
	for (int i = 0; i < fp_opt_count(); i++) if ((fp_cur()->opt[save.player][i] & 7) == wantDiff) fpOpt = i;
}

static void fp_sub(void) {
	sub_header(save.player ? "FREEPLAY - PICO" : "FREEPLAY - BOYFRIEND");
	char b[64], sc[24];
	for (int i = -3; i <= 2; i++) {
		int k = fpSel + i;
		if (k < 0 || k >= fpCount) continue;
		int y = 96 + i * 19;
		if (i == 0) sub_button(8, y - 3, 304, 21, true);
		sub_text_centered(FONT_VCR12O, BW / 2, y, freeplay_songs[fpList[k]].name, i == 0 ? DARKTXT : abs(i) == 3 ? RGB(140, 130, 160) : WHITE);
	}
	sub_fill(0, 156, BW, 1, RGB(120, 100, 160));
	int o = fp_opt(), si = o >> 3, d = o & 7;
	const char *var = strchr(song_info[si].tag, '-') && strcmp(song_info[si].tag, song_info[si].id) ? song_info[si].tag + strlen(song_info[si].id) + 1 : "";
	snprintf(b, sizeof(b), "%s%s%s", diff_name(d), *var && d < 3 ? " - " : "", d < 3 ? var : "");
	for (char *p = b; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
	static const u32 dc[5] = { RGB(120, 255, 120), RGB(255, 230, 100), RGB(255, 100, 100), RGB(0xC0, 0x60, 0xFF), RGB(0xFF, 0x40, 0xA0) };
	sub_button(8, 160, 34, 24, false);
	sub_text_centered(FONT_VCR12O, 25, 164, "<", WHITE);
	sub_button(278, 160, 34, 24, false);
	sub_text_centered(FONT_VCR12O, 295, 164, ">", WHITE);
	sub_text_centered(FONT_VCR12O, BW / 2, 164, b, dc[d]);
	format_score(sc, save.songScore[si][d]);
	snprintf(b, sizeof(b), "PERSONAL BEST: %s", sc);
	sub_text_centered(FONT_VCR8O, BW / 2, 190, b, WHITE);
	snprintf(b, sizeof(b), "%s", song_info[si].artist);
	sub_text_centered(FONT_VCR8, BW / 2, 205, b, RGB(180, 170, 200));
	sub_button(90, 218, 140, 20, false);
	sub_text_centered(FONT_VCR8, BW / 2, 222, "SELECT: character", WHITE);
}

void freeplay_enter(void) {
	chr_slots_reset();
	sheet_load(&shFpBg, save.player ? &sheet_fpbg_pico : &sheet_fpbg_bf);
	fpDiffLoaded = -1;
	chr_load(&dj, save.player ? "dj_pico" : "dj_bf", ROLE_OTHER);
	if (dj.loaded) {
		dj.x = 640 + dj.hdr->gx;
		dj.y = 366 + dj.hdr->gy;
	}
	chr_play(&dj, "intro", true);
	menu_music(false);
	fpCount = 0;
	for (int i = 0; i < FREEPLAY_COUNT; i++) if (freeplay_songs[i].n[save.player]) fpList[fpCount++] = i;
	if (fpSel >= fpCount) fpSel = 0;
	fp_pick_opt(save.lastDiff);
	fp_load_diff();
	fpScroll = fpSel;
	fpChosen = false;
	fpT = 0;
	fp_sub();
	g_fade = 0;
}

void freeplay_leave(void) {
	sheet_unload(&shFpBg);
	sheet_unload(&shFpDiff);
	chr_unload(&dj);
	chr_slots_reset();
}

static void fp_change(int d) {
	int want = fp_opt() & 7;
	fpSel = (fpSel + d + fpCount) % fpCount;
	fp_pick_opt(want);
	fp_load_diff();
	audio_sfx(SFX_SCROLL, 0.5f);
	fp_sub();
}

static void fp_change_diff(int d) {
	fpOpt = (fpOpt + d + fp_opt_count()) % fp_opt_count();
	fp_load_diff();
	audio_sfx(SFX_SCROLL, 0.5f);
	fp_sub();
}

static void fp_accept(void) {
	fpChosen = true;
	fpT = 0;
	chr_play(&dj, "confirm", true);
	audio_sfx(SFX_CONFIRM, 0.7f);
}

void freeplay_update(float dt) {
	fpScroll += (fpSel - fpScroll) * fps_ratio(0.2f, dt);
	fpT += dt;
	chr_update(&dj, dt, 100, false);
	if (dj.finished && !strcmp(chr_anim_name(&dj), "intro")) chr_play(&dj, "idle", true);
	if (fpChosen) {
		if (fpT > 1.0f) {
			int o = fp_opt();
			g_storyMode = false;
			g_song = o >> 3;
			g_diff = o & 7;
			save.lastDiff = g_diff;
			state_switch(ST_PLAY);
		}
		return;
	}
	if (kDown & KEY_UP) fp_change(-1);
	if (kDown & KEY_DOWN) fp_change(1);
	if (kDown & (KEY_LEFT | KEY_L)) fp_change_diff(-1);
	if (kDown & (KEY_RIGHT | KEY_R)) fp_change_diff(1);
	if (kDown & KEY_ACCEPT) { fp_accept(); return; }
	if (kDown & KEY_SELECT) { audio_sfx(SFX_CONFIRM, 0.5f); state_switch(ST_CHARSELECT); return; }
	if (kDown & KEY_B) { audio_sfx(SFX_CANCEL, 0.6f); state_switch(ST_MAINMENU); }
	if (kDown & KEY_TOUCH) {
		if (touch.py >= 32 && touch.py < 154) {
			int i = (int)floorf((touch.py - (96 - 3)) / 19.0f);
			if (i == 0) fp_accept();
			else if (i >= -3 && i <= 2) fp_change(i);
		} else if (touch.py >= 158 && touch.py < 186) fp_change_diff(touch.px < BW / 2 ? -1 : 1);
		else if (touch.py >= 214) state_switch(ST_CHARSELECT);
	}
}

// FreeplayState layout: backing card + DJ (left, DJ at 640,366 + its atlas
// position), week background, the difficulty and best score above the
// song capsules on the right.
static const Cam fpCam = { 560, 330, 0.85f, 0, 0 };

void freeplay_draw(void) {
	u32 card = save.player ? RGB(0xFF, 0x9C, 0xC8) : RGB(0xFF, 0xD8, 0x63);
	gfx_rect(0, 0, SW, SH, BLACK, 255);
	gfx_depth(4);
	sheet_draw(&shFpBg, 0, 170, 22, 218.0f / 720.0f, WHITE, 255);
	gfx_depth(2);
	gfx_rect(0, 22, 196, SH, card, 255);
	// backing text (BackingCard: scrolling character names)
	const char *t1 = save.player ? "PICO  PICO  PICO  PICO  PICO" : "BOYFRIEND  BOYFRIEND  BOYFRIEND";
	for (int r = 0; r < 7; r++) {
		float off = fmodf(fpT * (r & 1 ? 24 : -24), 180);
		font_draw(FONT_VCR24O, -180 + off + (r & 1) * 40, 28 + r * 31, t1, save.player ? RGB(0xE0, 0x60, 0xA0) : RGB(0xFF, 0x99, 0x63), 70);
	}
	gfx_rect(0, 22, 196, 24, WHITE, 120);
	gfx_depth(0);
	chr_draw(&dj, &fpCam);
	gfx_depth(-1);
	gfx_gradient(0, 0, SW, 22, RGB(0, 0, 0), RGB(20, 20, 30), 255);
	font_draw(FONT_VCR16O, 6, 0, "FREEPLAY", WHITE, 255);
	// difficulty + personal best (right column)
	if (shFpDiff.loaded) draw_centered(&shFpDiff, 0, 296, 46, MS * 0.85f, WHITE, 255);
	char b[32], sc[24];
	int o = fp_opt();
	format_score(sc, save.songScore[o >> 3][o & 7]);
	snprintf(b, sizeof(b), "BEST: %s", sc);
	font_draw_right(FONT_VCR12O, SW - 6, 3, b, WHITE, 255);
	// capsules
	for (int i = -4; i <= 5; i++) {
		int k = fpSel + i;
		if (k < 0 || k >= fpCount) continue;
		float d = k - fpScroll;
		float y = 140 + d * 27 - 11;
		if (y < 66 || y > SH) continue;
		float x = 206 + fabsf(d) * 7;
		bool sel = i == 0;
		gfx_depth(sel ? -1.5f : -0.8f);
		gfx_gradient_h(x, y, SW + 2, y + 23, sel ? RGB(0x30, 0x48, 0x98) : RGB(0x16, 0x16, 0x34), sel ? RGB(0x18, 0x20, 0x50) : RGB(0x0C, 0x0C, 0x20), sel ? 255 : 215);
		gfx_rect(x, y, x + 4, y + 23, sel ? RGB(0x33, 0xFF, 0xFF) : RGB(0x60, 0x60, 0x90), 255);
		const char *nm = freeplay_songs[fpList[k]].name;
		int fid = font_width(FONT_VCR12O, nm) > SW - x - 12 ? FONT_VCR8O : FONT_VCR12O;
		font_draw(fid, x + 9, y + (fid == FONT_VCR12O ? 3 : 5), nm, sel ? WHITE : RGB(170, 170, 200), sel ? 255 : 200);
	}
	if (fpChosen && ((int)(fpT * 16) & 1)) gfx_rect(206, 129, SW, 152, WHITE, 70);
	gfx_depth(0);
}

// ===========================================================================
// Character select (CharSelectSubState, simplified): Boyfriend / Pico
// ===========================================================================
static Sheet shCs, shCsName;
static Character chill;
static int csSel;
static float csT;
static bool csChosen;
static const Cam menuCam = { 640, 360, 1.0f, 0, 0 };

static void cs_load(void) {
	chr_unload(&chill);
	chr_slots_reset();
	chr_load(&chill, csSel ? "chill_pico" : "chill_bf", ROLE_OTHER);
	if (chill.loaded) {
		chill.x = 640 - chill.hdr->W / 2 + 120;
		chill.y = 140;
	}
	chr_play(&chill, "slidein", true);
	sheet_unload(&shCsName);
	sheet_load(&shCsName, csSel ? &sheet_csname_pico : &sheet_csname_bf);
}

static void cs_sub(void) {
	sub_header("CHOOSE YOUR CHARACTER");
	for (int i = 0; i < 2; i++) {
		int x = 20 + i * 150;
		sub_button(x, 52, 130, 100, i == csSel);
		sub_text_centered(FONT_VCR16O, x + 65, 92, i ? "PICO" : "BOYFRIEND", i == csSel ? DARKTXT : WHITE);
	}
	sub_text_centered(FONT_VCR8O, BW / 2, 176, "LEFT/RIGHT: choose   A: confirm", RGB(200, 200, 220));
	sub_text_centered(FONT_VCR8O, BW / 2, 194, "B: back", RGB(200, 200, 220));
}

void charselect_enter(void) {
	sheet_load(&shCs, &sheet_charselect);
	memset(&chill, 0, sizeof(chill));
	csSel = save.player;
	csChosen = false;
	cs_load();
	cs_sub();
	g_fade = 0;
}

void charselect_leave(void) {
	sheet_unload(&shCs);
	sheet_unload(&shCsName);
	chr_unload(&chill);
	chr_slots_reset();
}

void charselect_update(float dt) {
	csT += dt;
	chr_update(&chill, dt, 100, false);
	if (chill.finished && !strcmp(chr_anim_name(&chill), "slidein")) chr_play(&chill, "idle", true);
	if (csChosen) {
		if (csT > 0.9f) {
			save.player = csSel;
			save_write();
			state_switch(ST_FREEPLAY);
		}
		return;
	}
	int old = csSel;
	if (kDown & (KEY_LEFT | KEY_RIGHT)) csSel ^= 1;
	if ((kDown & KEY_TOUCH) && touch.py >= 52 && touch.py < 152) {
		int i = touch.px < BW / 2 ? 0 : 1;
		if (i == csSel) { csChosen = true; csT = 0; chr_play(&chill, "select", true); audio_sfx(SFX_CONFIRM, 0.7f); }
		csSel = i;
	}
	if (old != csSel && !csChosen) { audio_sfx(SFX_SCROLL, 0.5f); cs_load(); cs_sub(); }
	if (kDown & KEY_ACCEPT) { csChosen = true; csT = 0; chr_play(&chill, "select", true); audio_sfx(SFX_CONFIRM, 0.7f); }
	if (kDown & KEY_B) { audio_sfx(SFX_CANCEL, 0.6f); state_switch(ST_FREEPLAY); }
}

void charselect_draw(void) {
	gfx_rect(0, 0, SW, SH, BLACK, 255);
	gfx_depth(5);
	sheet_draw(&shCs, CHARSELECT_BG, mx(-150), my(-10), MS, WHITE, 255);
	gfx_depth(2);
	sheet_draw(&shCs, CHARSELECT_STAGE, mx(-40), my(390), MS, WHITE, 255);
	gfx_depth(0);
	chr_draw(&chill, &menuCam);
	gfx_depth(-2);
	sheet_draw(&shCs, CHARSELECT_CURTAINS, mx(-47), my(-49), MS, WHITE, 255);
	if (shCsName.loaded) draw_centered(&shCsName, 0, mx(980), my(100), MS, WHITE, 255);
	gfx_depth(0);
}

// ===========================================================================
// Results (ResultState, simplified)
// ===========================================================================
static float rsT;
static int rsRank;
static const char *rankNames[6] = { "PERFECT", "PERFECT", "EXCELLENT", "GREAT", "GOOD", "LOSS" };
static const u32 rankCols[6] = { RGB(255, 220, 80), RGB(255, 180, 255), RGB(120, 220, 255), RGB(120, 255, 140), RGB(255, 230, 120), RGB(160, 120, 255) };

// Scoring.calculateRank
static int calc_rank(const Results *r) {
	if (r->notes <= 0) return 4;
	int hit = r->judged[0] + r->judged[1];
	float comp = (float)hit / r->notes;
	if (r->judged[0] == r->notes && r->misses == 0) return 0;
	if (comp >= 1.0f && r->misses == 0) return 1;
	if (comp >= 0.9f) return 2;
	if (comp >= 0.8f) return 3;
	if (comp >= 0.6f) return 4;
	return 5;
}

void results_enter(void) {
	rsT = 0;
	rsRank = calc_rank(&g_results);
	audio_play_music("romfs:/snd/results.adp", true, 1);
	Results *r = &g_results;
	char b[64], sc[24];
	sub_header("RESULTS");
	sub_text_centered(FONT_VCR12O, BW / 2, 38, r->title, WHITE);
	static const char *jn[4] = { "SICK", "GOOD", "BAD", "SHIT" };
	static const u32 jc[4] = { RGB(80, 220, 255), RGB(120, 255, 120), RGB(255, 200, 80), RGB(255, 100, 100) };
	snprintf(b, sizeof(b), "TOTAL NOTES: %d", r->notes);
	sub_text(FONT_VCR12O, 50, 62, b, WHITE);
	snprintf(b, sizeof(b), "MAX COMBO:   %d", r->maxCombo);
	sub_text(FONT_VCR12O, 50, 80, b, WHITE);
	for (int i = 0; i < 4; i++) {
		snprintf(b, sizeof(b), "%-5s        %d", jn[i], r->judged[i]);
		sub_text(FONT_VCR12O, 50, 102 + i * 17, b, jc[i]);
	}
	snprintf(b, sizeof(b), "MISSED       %d", r->misses);
	sub_text(FONT_VCR12O, 50, 170, b, RGB(200, 120, 255));
	format_score(sc, r->score);
	snprintf(b, sizeof(b), "SCORE: %s%s", sc, r->newBest ? "  NEW BEST!" : "");
	sub_text_centered(FONT_VCR16O, BW / 2, 194, b, r->newBest ? YELLOW : WHITE);
	sub_text_centered(FONT_VCR8, BW / 2, 224, "A / START: continue", RGB(150, 140, 170));
	g_fade = 0;
}

void results_leave(void) { audio_stop(); }

void results_update(float dt) {
	rsT += dt;
	if (rsT > 0.5f && (kDown & (KEY_ACCEPT | KEY_TOUCH | KEY_B))) {
		audio_sfx(SFX_CONFIRM, 0.6f);
		menu_music(false);
		state_switch(g_storyMode ? ST_STORY : ST_FREEPLAY);
	}
}

void results_draw(void) {
	gfx_depth(3);
	gfx_gradient(0, 0, SW, SH, RGB(0xFE, 0xC8, 0x5C), RGB(0xFD, 0xD6, 0x43), 255);
	for (int i = 0; i < 7; i++) {
		float y = fmodf(rsT * 30 + i * 40, 280) - 40;
		float x = fmodf(rsT * 20 + i * 70, 200) - 60;
		font_draw(FONT_VCR24O, x, y, rankNames[rsRank], RGB(0xFF, 0xB0, 0x30), 110);
	}
	gfx_depth(-2);
	float k = clampf(rsT * 3, 0, 1);
	float sc = 1 + (1 - k) * 2;
	const char *rn = rankNames[rsRank];
	float w = font_width(FONT_VCR24O, rn) * sc;
	font_draw_scaled(FONT_VCR24O, 200 - w / 2, 86 - 16 * sc, sc, rn, rankCols[rsRank], AL(k));
	gfx_depth(-1);
	font_draw_centered(FONT_VCR16O, 200, 140, g_results.title, WHITE, 255);
	gfx_depth(0);
}

// ===========================================================================
// Options
// ===========================================================================
static int opSel;
#define OP_COUNT 9
static const char *opNames[OP_COUNT] = { "Downscroll", "Middlescroll", "Ghost tapping", "Botplay", "Camera zoom", "Flashing lights",
	"Note offset", "3D depth", "Back" };

static void op_sub(void) {
	sub_header("OPTIONS");
	char b[48];
	for (int i = 0; i < OP_COUNT; i++) {
		int y = 36 + i * 22;
		bool s = i == opSel;
		sub_button(10, y, 300, 20, s);
		sub_text(FONT_VCR12O, 20, y + 2, opNames[i], s ? DARKTXT : WHITE);
		const char *v = NULL;
		switch (i) {
		case 0: v = save.downscroll ? "ON" : "OFF"; break;
		case 1: v = save.middlescroll ? "ON" : "OFF"; break;
		case 2: v = save.ghostTapping ? "ON" : "OFF"; break;
		case 3: v = save.botplay ? "ON" : "OFF"; break;
		case 4: v = save.camZoom ? "ON" : "OFF"; break;
		case 5: v = save.flashing ? "ON" : "OFF"; break;
		case 6: snprintf(b, sizeof(b), "< %d ms >", save.noteOffset); v = b; break;
		case 7: snprintf(b, sizeof(b), "< %d%% >", save.depth3d); v = b; break;
		}
		if (v) sub_text_right(FONT_VCR12O, 300, y + 2, v, s ? DARKTXT : RGB(0x33, 0xFF, 0xFF));
	}
}

void options_enter(void) {
	menu_music(false);
	opSel = 0;
	op_sub();
	g_fade = 0;
}

void options_leave(void) { save_write(); }

static void op_toggle(int d) {
	switch (opSel) {
	case 0: save.downscroll = !save.downscroll; break;
	case 1: save.middlescroll = !save.middlescroll; break;
	case 2: save.ghostTapping = !save.ghostTapping; break;
	case 3: save.botplay = !save.botplay; break;
	case 4: save.camZoom = !save.camZoom; break;
	case 5: save.flashing = !save.flashing; break;
	case 6: save.noteOffset = (int)clampf(save.noteOffset + d * 5, -500, 500); break;
	case 7: save.depth3d = (int)clampf(save.depth3d + d * 10, 0, 200); break;
	case 8: state_switch(g_optionsReturn); return;
	}
	audio_sfx(SFX_SCROLL, 0.5f);
	op_sub();
}

void options_update(float dt) {
	if (kDown & KEY_UP) { opSel = (opSel + OP_COUNT - 1) % OP_COUNT; audio_sfx(SFX_SCROLL, 0.5f); op_sub(); }
	if (kDown & KEY_DOWN) { opSel = (opSel + 1) % OP_COUNT; audio_sfx(SFX_SCROLL, 0.5f); op_sub(); }
	if (kDown & KEY_A) op_toggle(1);
	if ((opSel == 6 || opSel == 7) && (kDown & KEY_LEFT)) op_toggle(-1);
	if ((opSel == 6 || opSel == 7) && (kDown & KEY_RIGHT)) op_toggle(1);
	if (kDown & KEY_B) { audio_sfx(SFX_CANCEL, 0.6f); state_switch(g_optionsReturn); }
	for (int i = 0; i < OP_COUNT; i++) {
		if (touch_in(10, 36 + i * 22, 300, 20)) {
			opSel = i;
			op_toggle((i == 6 || i == 7) ? (touch.px < BW / 2 ? -1 : 1) : 1);
		}
	}
}

void options_draw(void) {
	gfx_depth(3);
	gfx_gradient(0, 0, SW, SH, RGB(0x50, 0x30, 0x90), RGB(0x10, 0x08, 0x20), 255);
	gfx_depth(-1.5f);
	font_draw_centered(FONT_VCR24O, 200, 90, "OPTIONS", WHITE, 255);
	gfx_depth(-0.5f);
	font_draw_centered(FONT_VCR12O, 200, 134, opNames[opSel], YELLOW, 255);
	if (opSel == 7) font_draw_centered(FONT_VCR8O, 200, 156, "move the 3D slider to see the depth", RGB(200, 200, 220), 255);
	gfx_depth(0);
}
