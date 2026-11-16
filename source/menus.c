// Menus: TitleState, MainMenuState, StoryMenuState, FreeplayState, options.
// The top screen shows the 1280x720 view letterboxed (MS = 0.2 DS px per
// unit, see mx()/my()), the bottom screen holds lists and touch buttons.
#include "engine.h"

#define MENU_BPM 102.0f
static float beatTimer;
static int lastBeat;
static bool subDirty;

static int menu_beat(void) {
	float ms = audio_music_time();
	return (int)(ms / (60000.0f / MENU_BPM));
}

static void draw_centered(Sheet *sh, int frame, float cx, float cy, float scale, u16 color, int alpha) {
	float x0, y0, x1, y1;
	sheet_frame_bounds(sh, frame, &x0, &y0, &x1, &y1);
	sheet_draw(sh, frame, cx - (x0 + x1) / 2 * scale, cy - (y0 + y1) / 2 * scale, scale, color, alpha);
}

static void sub_header(const char *title) {
	sub_clear(RGB(24, 16, 44));
	sub_fill(0, 0, 256, 24, RGB(0xF9, 0xCF, 0x51));
	sub_text_centered(FONT_VCR12O, 128, 6, title, WHITE);
}

// ===========================================================================
// Title (TitleState): the logo and Girlfriend are streamed sprites, laid
// out to fill the DS top screen (logo top-left in front, Girlfriend on the
// speakers bottom-right, as in the game but tighter).
// ===========================================================================
#include "character.h"

static Character tLogo, tGf;
static float flash;
static bool titlePressed;
static float pressTimer, titleT;
#define TK 0.22f                                   // DS px per unit
static const Cam titleCam = { 640, 360, TK / KDS, 0, 0 };
static inline float wx_of(float ds) { return (ds - 128) / TK + 640; }
static inline float wy_of(float ds) { return (ds - 96) / TK + 360; }

void title_enter(void) {
	chr_slots_reset();
	memset(&tLogo, 0, sizeof(tLogo));
	memset(&tGf, 0, sizeof(tGf));
	chr_load(&tGf, "title_gf", ROLE_OTHER);
	chr_load(&tLogo, "title_logo", ROLE_OTHER);
	// frame origins in DS pixels
	tLogo.x = wx_of(-14);
	tLogo.y = wy_of(-12);
	if (tGf.loaded) {
		tGf.x = wx_of(256 - tGf.hdr->W * TK + 8);
		tGf.y = wy_of(192 - tGf.hdr->H * TK + 6);
	}
	font_load(FONT_VCR16O, true);
	font_load(FONT_VCR12O, false);
	font_load(FONT_VCR16O, false);
	font_load(FONT_VCR8, false);
	menu_music(true);
	titlePressed = false;
	titleT = 0;
	if (!g_titleSeen) screen_flash(2);       // skipIntro: FlxG.camera.flash(WHITE, 4)
	g_titleSeen = true;
	lastBeat = -1;
	sub_clear(RGB(0, 0, 0));
	sub_fill(0, 60, 256, 70, RGB(0x1A, 0x10, 0x30));
	sub_text_centered(FONT_VCR16O, 128, 74, "Friday Night Funkin'", WHITE);
	sub_text_centered(FONT_VCR12O, 128, 102, "Nintendo DS", RGB(0xF9, 0xCF, 0x51));
	sub_text_centered(FONT_VCR8, 128, 170, "Unofficial fan port", RGB(120, 110, 140));
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
	chr_draw(&tGf, &titleCam);
	chr_draw(&tLogo, &titleCam);
	bool on = !titlePressed || ((int)(pressTimer * 16) & 1);
	float pulse = titlePressed ? 1 : 0.6f + 0.4f * sinf(titleT * 5);
	if (on) font_draw(FONT_VCR16O, 6, 172, "PRESS START", titlePressed ? WHITE : RGB(0x33, 0xFF, 0xFF), A31(pulse));
	(void)flash;
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
			"evilsk8r - art", "Kawai Sprite - music", "", "Nintendo DS port: unofficial", "fan project, not affiliated", "with The Funkin' Crew.", "",
			"B: back" };
		for (int i = 0; i < 12; i++) sub_text_centered(FONT_VCR8, 128, 32 + i * 12, lines[i], WHITE);
		return;
	}
	for (int i = 0; i < 4; i++) {
		int y = 34 + i * 38;
		sub_fill(28, y, 200, 32, i == mmSel ? RGB(0xF9, 0xCF, 0x51) : RGB(60, 40, 90));
		sub_text_centered(FONT_VCR12O, 128, y + 10, mmNames[i], i == mmSel ? RGB(30, 20, 10) : WHITE);
	}
}

void mainmenu_enter(void) {
	sheet_load(&shBG, &sheet_menubg, false);
	sheet_load(&shMain, &sheet_mainmenu, false);
	font_load(FONT_VCR12O, false);
	font_load(FONT_VCR8, false);
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
			{
				if (mmSel == 0) state_switch(ST_STORY);
				else if (mmSel == 1) state_switch(ST_FREEPLAY);
				else { g_optionsReturn = ST_MAINMENU; state_switch(ST_OPTIONS); }
			}
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
	if (kDown & KEY_TOUCH) {
		for (int i = 0; i < 4; i++) {
			int y = 34 + i * 38;
			if (touch.py >= y && touch.py < y + 32 && touch.px >= 28 && touch.px < 228) {
				if (i != mmSel) { mmSel = i; mm_sub(); }
				mm_accept();
			}
		}
	}
}

void mainmenu_draw(void) {
	// menuBG (scrollFactor 0.17 on the camera following the selected item)
	float sc = 0.28f;
	float off = (mmCamY - 360) * 0.17f * sc * 0.5f;
	sheet_draw(&shBG, 0, 128 - 1280 * sc / 2, 96 - 720 * sc / 2 - off - 4, sc, WHITE, 31);
	// the 4 items always fit the DS screen (spacing 160, centered)
	float k = 0.25f;
	for (int i = 0; i < 4; i++) {
		if (mmChosen && i != mmSel) continue;
		if (mmChosen && ((int)(mmFlick * 12) & 1)) continue;
		bool s = i == mmSel;
		const u8 *seq = s ? mmSelSeq[i] : mmIdle[i];
		int len = s ? mmSelLen[i] : mmIdleLen[i];
		int f = ((int)(mmT * 24)) % len;
		float y = 96 + (i - 1.5f) * 160 * k * 1.12f;
		draw_centered(&shMain, seq[f], 128, y, k, WHITE, 31);
	}
}

// ===========================================================================
// Story mode
// ===========================================================================
static Sheet shStory, shProps[3];
static int smWeek, smDiff = 1;
static float smT, smPropT, smChosenT;
static bool smChosen, smPropRight;

static int weektitle_frame(int w) { return STORYMENU_TITLE_TUTORIAL + w; }
static int diff_frame(int d) { return STORYMENU_DIFF_EASY + d; }

static void sm_sub(void) {
	sub_header("STORY MODE");
	char b[64], sc[24];
	for (int i = -2; i <= 2; i++) {
		int w = smWeek + i;
		if (w < 0 || w >= WEEK_COUNT) continue;
		int y = 74 + i * 20;
		bool s = i == 0;
		if (s) sub_fill(8, y - 3, 240, 18, RGB(0xF9, 0xCF, 0x51));
		snprintf(b, sizeof(b), "%s", week_info[w].name);
		sub_text_centered(FONT_VCR8O, 128, y, b, s ? RGB(30, 20, 10) : WHITE);
	}
	sub_fill(0, 124, 256, 1, RGB(120, 100, 160));
	snprintf(b, sizeof(b), "< %s >", diff_name(smDiff));
	sub_text_centered(FONT_VCR12O, 128, 130, b, smDiff == 0 ? RGB(120, 255, 120) : smDiff == 1 ? RGB(255, 230, 100) : RGB(255, 100, 100));
	format_score(sc, save.weekScore[smWeek][smDiff]);
	snprintf(b, sizeof(b), "WEEK SCORE: %s", sc);
	sub_text_centered(FONT_VCR8, 128, 150, b, WHITE);
	b[0] = 0;
	const WeekInfo *wi = &week_info[smWeek];
	for (int i = 0; i < wi->nSongs; i++) {
		if (i) strcat(b, ", ");
		strcat(b, song_info[wi->songs[i]].name);
	}
	sub_text_centered(FONT_VCR8, 128, 166, b, RGB(0xE5, 0x57, 0x77));
	sub_text_centered(FONT_VCR8, 128, 180, "A: play  B: back  L/R: difficulty", RGB(150, 140, 170));
}

static void sm_load_props(void) {
	for (int i = 0; i < 3; i++) sheet_unload(&shProps[i]);
	const WeekProps *wp = &week_props[smWeek];
	for (int i = 0; i < wp->n; i++) sheet_load(&shProps[i], wp->p[i].sheet, false);
	smPropT = 0;
}

void story_enter(void) {
	sheet_load(&shStory, &sheet_storymenu, false);
	font_load(FONT_VCR8O, true);
	font_load(FONT_VCR12O, false);
	font_load(FONT_VCR8, false);
	font_load(FONT_VCR8O, false);
	menu_music(false);
	smChosen = false;
	smT = 0;
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

void story_update(float dt) {
	smT += dt;
	smPropT += dt;
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
	if (kDown & KEY_ACCEPT) { smChosen = true; smChosenT = 0; audio_sfx(SFX_CONFIRM, 0.7f); smPropT = 0; }
	if (kDown & KEY_B) { audio_sfx(SFX_CANCEL, 0.6f); state_switch(ST_MAINMENU); }
	if (kDown & KEY_TOUCH) {
		if (touch.py >= 40 && touch.py < 64) sm_change_week(-1);
		else if (touch.py >= 84 && touch.py < 110) sm_change_week(1);
		else if (touch.py >= 126 && touch.py < 146) sm_change_diff(touch.px < 128 ? -1 : 1);
		else if (touch.py >= 64 && touch.py < 84) { smChosen = true; smChosenT = 0; audio_sfx(SFX_CONFIRM, 0.7f); }
	}
}

// StoryMenuState layout: black bar (score, level name), yellow band 56..456
// with the props, level titles from y 480, tracks at y 556, difficulty at 870,480.
// Horizontally the 1280 view is 256 px wide; vertically the band starts at 14.
static inline float sy(float v) { return (v - 56) * MS + 14; }

void story_draw(void) {
	const WeekInfo *wi = &week_info[smWeek];
	gfx_rect(0, 0, 256, 192, RGB(0, 0, 0), 31);
	gfx_rect(0, sy(56), 256, sy(456), HEXC(wi->bg), 31);
	char b[48], sc[24];
	format_score(sc, save.weekScore[smWeek][smDiff]);
	snprintf(b, sizeof(b), "LEVEL SCORE: %s", sc);
	font_draw(FONT_VCR8O, 3, 3, b, WHITE, 31);
	font_draw(FONT_VCR8O, 253 - font_width(FONT_VCR8O, wi->name), 3, wi->name, RGB(0xB0, 0xB0, 0xB0), 31);
	const WeekProps *wp = &week_props[smWeek];
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
		sheet_draw(&shProps[i], seq[f], mx(p->offx + 320 * i), sy(p->offy), MS, WHITE, 31);
	}
	// level titles: the selected one at y 480, the next ones below
	for (int k = 0; k < 3 && smWeek + k < WEEK_COUNT; k++) {
		u16 col = k == 0 && smChosen && ((int)(smChosenT * 20) & 1) ? RGB(0x33, 0xFF, 0xFF) : WHITE;
		float y = sy(480 + k * 125) + 6;
		if (y > 200) break;
		draw_centered(&shStory, weektitle_frame(smWeek + k), mx(560), y, MS, col, k == 0 ? 31 : 12);
	}
	// tracks (pink, VCR) and the difficulty selector
	font_draw_centered(FONT_VCR8O, mx(640 - 1280 * 0.33f), sy(556), "TRACKS", HEXC(0xE55777), 31);
	for (int i = 0; i < wi->nSongs; i++)
		font_draw_centered(FONT_VCR8O, mx(640 - 1280 * 0.33f), sy(556) + 11 + i * 10, song_info[wi->songs[i]].name, HEXC(0xE55777), 31);
	float ax = mx(850), ay = sy(480);
	sheet_draw(&shStory, sm_arrows[0], ax, ay, MS, WHITE, 31);
	draw_centered(&shStory, diff_frame(smDiff), mx(1068), ay + 9, MS, WHITE, 31);
	sheet_draw(&shStory, sm_arrows[1], 243, ay, MS, WHITE, 31);
}

// ===========================================================================
// Freeplay (FreeplayState): DJ on the left, song capsules on the right
// ===========================================================================
#include "character.h"

static Sheet shFpBg, shFpDiff;
static Character dj;
static int fpList[FREEPLAY_COUNT], fpCount;
static int fpSel, fpOpt;
static float fpScroll, fpT;
static bool fpChosen;
static int fpDiffLoaded = -1;
static const Cam menuCam = { 640, 360, 0.75f, 0, 0 };

static const FreeplaySong *fp_cur(void) { return &freeplay_songs[fpList[fpSel]]; }
static int fp_opt_count(void) { return fp_cur()->n[save.player]; }
static int fp_opt(void) { return fp_cur()->opt[save.player][fpOpt]; }

static void fp_load_diff(void) {
	int d = fp_opt() & 7;
	if (d == fpDiffLoaded) return;
	sheet_unload(&shFpDiff);
	if (fpdiff_sheets[d]) sheet_load(&shFpDiff, fpdiff_sheets[d], false);
	fpDiffLoaded = d;
}

static void fp_pick_opt(int wantDiff) {
	fpOpt = 0;
	for (int i = 0; i < fp_opt_count(); i++) if ((fp_cur()->opt[save.player][i] & 7) == wantDiff) fpOpt = i;
}

static void fp_sub(void) {
	sub_header(save.player ? "FREEPLAY - PICO" : "FREEPLAY - BOYFRIEND");
	char b[64], sc[24];
	for (int i = -3; i <= 3; i++) {
		int k = fpSel + i;
		if (k < 0 || k >= fpCount) continue;
		int y = 72 + i * 14;
		if (i == 0) sub_fill(8, y - 2, 240, 14, RGB(0xF9, 0xCF, 0x51));
		sub_text_centered(FONT_VCR8O, 128, y, freeplay_songs[fpList[k]].name, i == 0 ? RGB(30, 20, 10) : WHITE);
	}
	sub_fill(0, 122, 256, 1, RGB(120, 100, 160));
	int o = fp_opt(), si = o >> 3, d = o & 7;
	const char *var = strchr(song_info[si].tag, '-') && strcmp(song_info[si].tag, song_info[si].id) ? song_info[si].tag + strlen(song_info[si].id) + 1 : "";
	snprintf(b, sizeof(b), "< %s%s%s >", diff_name(d), *var && d < 3 ? " - " : "", d < 3 ? var : "");
	static const u16 dc[5] = { RGB(120, 255, 120), RGB(255, 230, 100), RGB(255, 100, 100), RGB(0xC0, 0x60, 0xFF), RGB(0xFF, 0x40, 0xA0) };
	sub_text_centered(FONT_VCR12O, 128, 128, b, dc[d]);
	format_score(sc, save.songScore[si][d]);
	snprintf(b, sizeof(b), "PERSONAL BEST: %s", sc);
	sub_text_centered(FONT_VCR8, 128, 148, b, WHITE);
	snprintf(b, sizeof(b), "%s", song_info[si].artist);
	sub_text_centered(FONT_VCR8, 128, 160, b, RGB(180, 170, 200));
	sub_fill(70, 172, 116, 16, RGB(60, 40, 90));
	sub_text_centered(FONT_VCR8, 128, 176, "SELECT: character", WHITE);
}

void freeplay_enter(void) {
	chr_slots_reset();
	sheet_load(&shFpBg, save.player ? &sheet_fpbg_pico : &sheet_fpbg_bf, false);
	fpDiffLoaded = -1;
	chr_load(&dj, save.player ? "dj_pico" : "dj_bf", ROLE_OTHER);
	dj.x = 640 + dj.hdr->gx;
	dj.y = 366 + dj.hdr->gy;
	chr_play(&dj, "intro", true);
	font_load(FONT_VCR12O, false);
	font_load(FONT_VCR12O, true);
	font_load(FONT_VCR16O, true);
	font_load(FONT_VCR8O, true);
	font_load(FONT_VCR8, false);
	font_load(FONT_VCR8O, false);
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
		if (touch.py >= 26 && touch.py < 120) {
			int i = (touch.py - 72 + 7 + 140) / 14 - 10;
			if (i == 0) fp_accept();
			else if (i >= -3 && i <= 3) fp_change(i);
		} else if (touch.py >= 124 && touch.py < 144) fp_change_diff(touch.px < 128 ? -1 : 1);
		else if (touch.py >= 170) state_switch(ST_CHARSELECT);
	}
}

// FreeplayState layout for the DS: backing card + DJ (bottom left, DJ at
// 640,366 + its atlas position, as in the game), week background, the
// difficulty and best score above the song capsules.
static const Cam fpCam = { 505, 320, 0.9f, 0, 0 };

void freeplay_draw(void) {
	u16 card = save.player ? RGB(0xFF, 0x9C, 0xC8) : RGB(0xFF, 0xD8, 0x63);
	gfx_rect(0, 0, 256, 192, RGB(0, 0, 0), 31);
	sheet_draw(&shFpBg, 0, 104, 16, 176.0f / 720.0f, WHITE, 31);
	gfx_rect(0, 16, 126, 192, card, 31);
	// backing text (BackingCard: scrolling character names)
	const char *t1 = save.player ? "PICO  PICO  PICO  PICO" : "BOYFRIEND  BOYFRIEND";
	for (int r = 0; r < 6; r++) {
		float off = fmodf(fpT * (r & 1 ? 18 : -18), 120);
		font_draw(FONT_VCR16O, -120 + off + (r & 1) * 30, 22 + r * 26, t1, save.player ? RGB(0xE0, 0x60, 0xA0) : RGB(0xFF, 0x99, 0x63), 10);
	}
	chr_draw(&dj, &fpCam);
	gfx_rect(0, 0, 256, 16, RGB(0, 0, 0), 31);
	font_draw(FONT_VCR12O, 4, 2, "FREEPLAY", WHITE, 31);
	// difficulty + personal best (right column)
	if (shFpDiff.loaded) draw_centered(&shFpDiff, 0, 191, 31, MS * 0.85f, WHITE, 31);
	char b[32], sc[24];
	int o = fp_opt();
	format_score(sc, save.songScore[o >> 3][o & 7]);
	snprintf(b, sizeof(b), "BEST: %s", sc);
	font_draw(FONT_VCR8O, 252 - font_width(FONT_VCR8O, b), 4, b, WHITE, 31);
	// capsules
	for (int i = -3; i <= 4; i++) {
		int k = fpSel + i;
		if (k < 0 || k >= fpCount) continue;
		float d = k - fpScroll;
		float y = 112 + d * 21 - 8;
		if (y < 46 || y > 192) continue;
		float x = 132 + fabsf(d) * 5;
		bool sel = i == 0;
		gfx_rect(x, y, 258, y + 18, sel ? RGB(0x30, 0x40, 0x80) : RGB(0x12, 0x12, 0x2C), sel ? 31 : 24);
		gfx_rect(x, y, x + 3, y + 18, sel ? RGB(0x33, 0xFF, 0xFF) : RGB(0x60, 0x60, 0x90), 31);
		const char *nm = freeplay_songs[fpList[k]].name;
		int fid = font_width(FONT_VCR12O, nm) > 252 - x - 8 ? FONT_VCR8O : FONT_VCR12O;
		font_draw(fid, x + 7, y + (fid == FONT_VCR12O ? 3 : 5), nm, sel ? WHITE : RGB(170, 170, 200), sel ? 31 : 24);
	}
	if (fpChosen && ((int)(fpT * 16) & 1)) gfx_rect(132, 104, 256, 122, WHITE, 8);
}

// ===========================================================================
// Character select (CharSelectSubState, simplified): Boyfriend / Pico
// ===========================================================================
static Sheet shCs, shCsName;
static Character chill;
static int csSel;
static float csT;
static bool csChosen;

static void cs_load(void) {
	chr_unload(&chill);
	chr_slots_reset();
	chr_load(&chill, csSel ? "chill_pico" : "chill_bf", ROLE_OTHER);
	chill.x = 640 - chill.hdr->W / 2 + 120;
	chill.y = 140;
	chr_play(&chill, "slidein", true);
	sheet_unload(&shCsName);
	sheet_load(&shCsName, csSel ? &sheet_csname_pico : &sheet_csname_bf, false);
}

static void cs_sub(void) {
	sub_header("CHOOSE YOUR CHARACTER");
	for (int i = 0; i < 2; i++) {
		int x = 24 + i * 112;
		sub_fill(x, 48, 96, 80, i == csSel ? RGB(0xF9, 0xCF, 0x51) : RGB(60, 40, 90));
		sub_text_centered(FONT_VCR12O, x + 48, 82, i ? "PICO" : "BOYFRIEND", i == csSel ? RGB(30, 20, 10) : WHITE);
	}
	sub_text_centered(FONT_VCR8, 128, 150, "LEFT/RIGHT: choose   A: confirm", RGB(200, 200, 220));
	sub_text_centered(FONT_VCR8, 128, 164, "B: back", RGB(200, 200, 220));
}

void charselect_enter(void) {
	sheet_load(&shCs, &sheet_charselect, false);
	font_load(FONT_VCR12O, false);
	font_load(FONT_VCR8, false);
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
	if (kDown & KEY_TOUCH && touch.py >= 48 && touch.py < 128) {
		int i = touch.px < 128 ? 0 : 1;
		if (i == csSel) { csChosen = true; csT = 0; chr_play(&chill, "select", true); audio_sfx(SFX_CONFIRM, 0.7f); }
		csSel = i;
	}
	if (old != csSel && !csChosen) { audio_sfx(SFX_SCROLL, 0.5f); cs_load(); cs_sub(); }
	if (kDown & KEY_ACCEPT) { csChosen = true; csT = 0; chr_play(&chill, "select", true); audio_sfx(SFX_CONFIRM, 0.7f); }
	if (kDown & KEY_B) { audio_sfx(SFX_CANCEL, 0.6f); state_switch(ST_FREEPLAY); }
}

void charselect_draw(void) {
	gfx_rect(0, 0, 256, 192, RGB(0, 0, 0), 31);
	sheet_draw(&shCs, CHARSELECT_BG, mx(-150), my(-10), MS, WHITE, 31);
	sheet_draw(&shCs, CHARSELECT_STAGE, mx(-40), my(390), MS, WHITE, 31);
	chr_draw(&chill, &menuCam);
	sheet_draw(&shCs, CHARSELECT_CURTAINS, mx(-47), my(-49), MS, WHITE, 31);
	if (shCsName.loaded) draw_centered(&shCsName, 0, mx(980), my(100), MS, WHITE, 31);
}

// ===========================================================================
// Results (ResultState, simplified)
// ===========================================================================
static float rsT;
static int rsRank;
static const char *rankNames[6] = { "PERFECT", "PERFECT", "EXCELLENT", "GREAT", "GOOD", "LOSS" };
static const u16 rankCols[6] = { RGB(255, 220, 80), RGB(255, 180, 255), RGB(120, 220, 255), RGB(120, 255, 140), RGB(255, 230, 120), RGB(160, 120, 255) };

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
	font_load(FONT_VCR16O, true);
	font_load(FONT_VCR12O, true);
	font_load(FONT_VCR12O, false);
	font_load(FONT_VCR8, false);
	rsT = 0;
	rsRank = calc_rank(&g_results);
	audio_play_music("nitro:/snd/results.adp", true, 1);
	Results *r = &g_results;
	char b[64], sc[24];
	sub_header("RESULTS");
	sub_text_centered(FONT_VCR12O, 128, 32, r->title, WHITE);
	static const char *jn[4] = { "SICK", "GOOD", "BAD", "SHIT" };
	static const u16 jc[4] = { RGB(80, 220, 255), RGB(120, 255, 120), RGB(255, 200, 80), RGB(255, 100, 100) };
	snprintf(b, sizeof(b), "TOTAL NOTES: %d", r->notes);
	sub_text(FONT_VCR8, 40, 56, b, WHITE);
	snprintf(b, sizeof(b), "MAX COMBO:   %d", r->maxCombo);
	sub_text(FONT_VCR8, 40, 68, b, WHITE);
	for (int i = 0; i < 4; i++) {
		snprintf(b, sizeof(b), "%-5s        %d", jn[i], r->judged[i]);
		sub_text(FONT_VCR8, 40, 84 + i * 12, b, jc[i]);
	}
	snprintf(b, sizeof(b), "MISSED       %d", r->misses);
	sub_text(FONT_VCR8, 40, 132, b, RGB(200, 120, 255));
	format_score(sc, r->score);
	snprintf(b, sizeof(b), "SCORE: %s%s", sc, r->newBest ? "  NEW BEST!" : "");
	sub_text_centered(FONT_VCR12O, 128, 152, b, WHITE);
	sub_text_centered(FONT_VCR8, 128, 178, "A / START: continue", RGB(150, 140, 170));
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
	gfx_gradient(0, 0, 256, 192, RGB(0xFE, 0xC8, 0x5C), RGB(0xFD, 0xD6, 0x43), 31);
	for (int i = 0; i < 6; i++) {
		float y = fmodf(rsT * 30 + i * 40, 240) - 40;
		font_draw(FONT_VCR12O, 8, y, rankNames[rsRank], RGB(0xFF, 0xB0, 0x30), 12);
	}
	float k = clampf(rsT * 3, 0, 1);
	float sc = 1 + (1 - k) * 2;
	const char *rn = rankNames[rsRank];
	float w = font_width(FONT_VCR16O, rn) * sc;
	font_draw_scaled(FONT_VCR16O, 128 - w / 2, 70 - 8 * sc, sc, rn, rankCols[rsRank], A31(k));
	font_draw_centered(FONT_VCR12O, 128, 110, g_results.title, WHITE, 31);
}

// ===========================================================================
// Options
// ===========================================================================
static int opSel;
#define OP_COUNT 7
static const char *opNames[OP_COUNT] = { "Downscroll", "Ghost tapping", "Botplay", "Camera zoom", "Flashing lights", "Note offset", "Back" };

static void op_sub(void) {
	sub_header("OPTIONS");
	char b[48];
	for (int i = 0; i < OP_COUNT; i++) {
		int y = 32 + i * 21;
		bool s = i == opSel;
		sub_fill(12, y, 232, 18, s ? RGB(0xF9, 0xCF, 0x51) : RGB(60, 40, 90));
		sub_text(FONT_VCR8O, 20, y + 4, opNames[i], s ? RGB(30, 20, 10) : WHITE);
		const char *v = NULL;
		switch (i) {
		case 0: v = save.downscroll ? "ON" : "OFF"; break;
		case 1: v = save.ghostTapping ? "ON" : "OFF"; break;
		case 2: v = save.botplay ? "ON" : "OFF"; break;
		case 3: v = save.camZoom ? "ON" : "OFF"; break;
		case 4: v = save.flashing ? "ON" : "OFF"; break;
		case 5: snprintf(b, sizeof(b), "< %d ms >", save.noteOffset); v = b; break;
		}
		if (v) sub_text(FONT_VCR8O, 236 - font_width(FONT_VCR8O, v), y + 4, v, s ? RGB(30, 20, 10) : RGB(0x33, 0xFF, 0xFF));
	}
}

void options_enter(void) {
	font_load(FONT_VCR16O, true);
	font_load(FONT_VCR12O, false);
	font_load(FONT_VCR8O, false);
	menu_music(false);
	opSel = 0;
	op_sub();
	g_fade = 0;
}

void options_leave(void) { save_write(); }

static void op_toggle(int d) {
	switch (opSel) {
	case 0: save.downscroll = !save.downscroll; break;
	case 1: save.ghostTapping = !save.ghostTapping; break;
	case 2: save.botplay = !save.botplay; break;
	case 3: save.camZoom = !save.camZoom; break;
	case 4: save.flashing = !save.flashing; break;
	case 5: save.noteOffset = (int)clampf(save.noteOffset + d * 5, -500, 500); break;
	case 6: state_switch(g_optionsReturn); return;
	}
	audio_sfx(SFX_SCROLL, 0.5f);
	op_sub();
}

void options_update(float dt) {
	if (kDown & KEY_UP) { opSel = (opSel + OP_COUNT - 1) % OP_COUNT; audio_sfx(SFX_SCROLL, 0.5f); op_sub(); }
	if (kDown & KEY_DOWN) { opSel = (opSel + 1) % OP_COUNT; audio_sfx(SFX_SCROLL, 0.5f); op_sub(); }
	if (kDown & KEY_A) op_toggle(1);
	if (opSel == 5 && (kDown & KEY_LEFT)) op_toggle(-1);
	if (opSel == 5 && (kDown & KEY_RIGHT)) op_toggle(1);
	if (kDown & KEY_B) { audio_sfx(SFX_CANCEL, 0.6f); state_switch(g_optionsReturn); }
	if (kDown & KEY_TOUCH) {
		for (int i = 0; i < OP_COUNT; i++) {
			int y = 32 + i * 21;
			if (touch.py >= y && touch.py < y + 18) {
				opSel = i;
				op_toggle(i == 5 ? (touch.px < 128 ? -1 : 1) : 1);
			}
		}
	}
}

void options_draw(void) {
	gfx_gradient(0, 0, 256, 192, RGB(0x50, 0x30, 0x90), RGB(0x10, 0x08, 0x20), 31);
	font_draw_centered(FONT_VCR16O, 128, 80, "OPTIONS", WHITE, 31);
	(void)beatTimer;
	(void)subDirty;
}
