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
// Title
// ===========================================================================
static Sheet shTitle;
static float logoT, gfT, flash;
static bool gfRight, titlePressed;
static float pressTimer;

void title_enter(void) {
	sheet_load(&shTitle, &sheet_title, false);
	font_load(FONT_VCR12O, false);
	font_load(FONT_VCR8, false);
	menu_music(true);
	logoT = gfT = 0;
	gfRight = false;
	titlePressed = false;
	flash = g_titleSeen ? 0 : 1;
	g_titleSeen = true;
	lastBeat = -1;
	sub_clear(RGB(0, 0, 0));
	sub_text_centered(FONT_VCR12O, 128, 70, "Friday Night Funkin'", WHITE);
	sub_text_centered(FONT_VCR8, 128, 92, "Nintendo DS port", RGB(200, 200, 200));
	sub_text_centered(FONT_VCR8, 128, 150, "Press START", RGB(0x33, 0xFF, 0xFF));
	g_fade = 0;
}

void title_leave(void) { sheet_unload(&shTitle); }

void title_update(float dt) {
	int b = menu_beat();
	if (b != lastBeat) {
		lastBeat = b;
		logoT = 0;
		gfT = 0;
		gfRight = !gfRight;
	}
	logoT += dt;
	gfT += dt;
	if (flash > 0) flash -= dt / 1.5f;
	if (!titlePressed && (kDown & (KEY_START | KEY_A | KEY_TOUCH))) {
		titlePressed = true;
		pressTimer = 0;
		flash = 1;
		audio_sfx(SFX_CONFIRM, 0.7f);
	}
	if (titlePressed) {
		pressTimer += dt;
		if (pressTimer > 1.5f) g_fade = clampf((pressTimer - 1.5f) / 0.5f, 0, 1);
		if (pressTimer > 2.0f) state_switch(ST_MAINMENU);
	}
}

void title_draw(void) {
	int f = (int)(logoT * 24 / 5);
	if (f >= TITLE_LOGO_LEN) f = TITLE_LOGO_LEN - 1;
	sheet_draw(&shTitle, title_logo[f], mx(-150), my(-100) + 10, MS, WHITE, 31);
	int g = (int)(gfT * 24 / 4);
	const u8 *seq = gfRight ? title_gf_right : title_gf_left;
	int len = gfRight ? TITLE_GF_RIGHT_LEN : TITLE_GF_LEFT_LEN;
	if (g >= len) g = len - 1;
	sheet_draw(&shTitle, seq[g], mx(1280 * 0.4f), my(720 * 0.07f) + 10, MS, WHITE, 31);
	bool on = !titlePressed || ((int)(pressTimer * 20) & 1);
	draw_centered(&shTitle, titlePressed && on ? TITLE_ENTER_PRESS : TITLE_ENTER_IDLE, 128, my(720 * 0.86f) + 8, MS, WHITE, 31);
	if (flash > 0) gfx_rect(0, 0, 256, 192, WHITE, A31(flash));
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
			g_fade = clampf((mmFlick - 1.0f) / 0.3f, 0, 1);
			if (mmFlick > 1.3f) {
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
	// menuBG: 1.2x, scroll factor 0.17 (DS: whole screen)
	float sc = 0.27f;
	float off = (mmCamY - 360) * 0.17f * sc * 0.6f;
	sheet_draw(&shBG, 0, 128 - 1280 * sc / 2, 96 - 720 * sc / 2 - off, sc, WHITE, 31);
	for (int i = 0; i < 4; i++) {
		if (mmChosen && i != mmSel) continue;
		if (mmChosen && ((int)(mmFlick * 12) & 1)) continue;
		bool s = i == mmSel;
		const u8 *seq = s ? mmSelSeq[i] : mmIdle[i];
		int len = s ? mmSelLen[i] : mmIdleLen[i];
		int f = ((int)(mmT * 24)) % len;
		float y = 96 + (120 + i * 160 - mmCamY) * 0.27f;
		draw_centered(&shMain, seq[f], 128, y, 0.27f, WHITE, 31);
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
		if (smChosenT > 1.0f) g_fade = clampf((smChosenT - 1.0f) / 0.4f, 0, 1);
		if (smChosenT > 1.4f) {
			g_storyMode = true;
			g_week = smWeek;
			g_weekPos = 0;
			g_weekScore = 0;
			g_diff = smDiff;
			g_song = week_info[smWeek].songs[0];
			save.lastDiff = smDiff;
			audio_stop();
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

void story_draw(void) {
	const WeekInfo *wi = &week_info[smWeek];
	// yellow band (y 56..456)
	gfx_rect(0, 0, 256, my(56), RGB(0, 0, 0), 31);
	gfx_rect(0, my(56), 256, my(456), HEXC(wi->bg), 31);
	gfx_rect(0, my(456), 256, 192, RGB(0, 0, 0), 31);
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
		// LevelProp: x = (FlxG.width * 0.25) * (1 + i) - 150 + offsets
		float x = 1280 * 0.25f * (1 + i) - 150 + p->offx;
		float y = 70 + p->offy;
		sheet_draw(&shProps[i], seq[f], mx(x), my(y), MS, WHITE, 31);
	}
	// week title, flashing when chosen
	u16 col = smChosen && ((int)(smChosenT * 20) & 1) ? RGB(0x33, 0xFF, 0xFF) : WHITE;
	draw_centered(&shStory, weektitle_frame(smWeek), 128, my(520), MS, col, 31);
	draw_centered(&shStory, diff_frame(smDiff), mx(1060), my(560), MS * 0.8f, WHITE, 31);
}

// ===========================================================================
// Freeplay
// ===========================================================================
static Sheet shDesat, shAlpha;
static int fpSel, fpDiff = 1;
static float fpScroll, fpLerpScore;

static int fp_song_week_color(int s) {
	static const u32 cols[8] = { 0x9271FD, 0x9271FD, 0x223344, 0x941653, 0xFC96D7, 0xA0D1FF, 0xFF78BF, 0xF6B604 };
	return cols[song_info[s].week & 7];
}

static void fp_sub(void) {
	sub_header("FREEPLAY");
	char b[64], sc[24];
	for (int i = -3; i <= 3; i++) {
		int s = fpSel + i;
		if (s < 0 || s >= SONG_COUNT) continue;
		int y = 76 + i * 14;
		if (i == 0) sub_fill(8, y - 2, 240, 14, RGB(0xF9, 0xCF, 0x51));
		sub_text_centered(FONT_VCR8O, 128, y, song_info[s].name, i == 0 ? RGB(30, 20, 10) : WHITE);
	}
	sub_fill(0, 128, 256, 1, RGB(120, 100, 160));
	snprintf(b, sizeof(b), "< %s >", diff_name(fpDiff));
	sub_text_centered(FONT_VCR12O, 128, 134, b, fpDiff == 0 ? RGB(120, 255, 120) : fpDiff == 1 ? RGB(255, 230, 100) : RGB(255, 100, 100));
	format_score(sc, save.songScore[fpSel][fpDiff]);
	snprintf(b, sizeof(b), "PERSONAL BEST: %s", sc);
	sub_text_centered(FONT_VCR8, 128, 156, b, WHITE);
	sub_text_centered(FONT_VCR8, 128, 178, "A: play  B: back  L/R: difficulty", RGB(150, 140, 170));
}

void freeplay_enter(void) {
	sheet_load(&shDesat, &sheet_menudesat, false);
	sheet_load(&shAlpha, &sheet_alphabet, false);
	font_load(FONT_VCR12O, false);
	font_load(FONT_VCR8, false);
	font_load(FONT_VCR8O, false);
	menu_music(false);
	if (save.lastDiff >= 0 && save.lastDiff < 3) fpDiff = save.lastDiff;
	fpScroll = fpSel;
	fp_sub();
	g_fade = 0;
}

void freeplay_leave(void) { sheet_unload(&shDesat); sheet_unload(&shAlpha); }

static void fp_change(int d) {
	fpSel = (fpSel + d + SONG_COUNT) % SONG_COUNT;
	audio_sfx(SFX_SCROLL, 0.5f);
	fp_sub();
}

static void fp_accept(void) {
	g_storyMode = false;
	g_song = fpSel;
	g_diff = fpDiff;
	save.lastDiff = fpDiff;
	audio_sfx(SFX_CONFIRM, 0.7f);
	audio_stop();
	state_switch(ST_PLAY);
}

void freeplay_update(float dt) {
	fpScroll += (fpSel - fpScroll) * fps_ratio(0.2f, dt);
	if (kDown & KEY_UP) fp_change(-1);
	if (kDown & KEY_DOWN) fp_change(1);
	if (kDown & (KEY_LEFT | KEY_L)) { fpDiff = (fpDiff + 2) % 3; audio_sfx(SFX_SCROLL, 0.5f); fp_sub(); }
	if (kDown & (KEY_RIGHT | KEY_R)) { fpDiff = (fpDiff + 1) % 3; audio_sfx(SFX_SCROLL, 0.5f); fp_sub(); }
	if (kDown & KEY_ACCEPT) { fp_accept(); return; }
	if (kDown & KEY_B) { audio_sfx(SFX_CANCEL, 0.6f); state_switch(ST_MAINMENU); }
	if (kDown & KEY_TOUCH) {
		if (touch.py >= 30 && touch.py < 126) {
			int i = (touch.py - 74 + 7 + 140) / 14 - 10;
			if (i == 0) fp_accept();
			else if (i >= -3 && i <= 3) fp_change(i);
		} else if (touch.py >= 130 && touch.py < 150) {
			fpDiff = (fpDiff + (touch.px < 128 ? 2 : 1)) % 3;
			fp_sub();
		}
	}
	(void)fpLerpScore;
}

// Alphabet (bold) text, x/y = top-left in DS px, sc = DS px per unit
static void alpha_text(float x, float y, float sc, const char *s, u16 color, int alpha) {
	for (; *s; s++) {
		int c = (u8)*s;
		if (c < 32 || c > 126) c = '?';
		const s16 *m = alphabet_map[c - 32];
		if (m[0] >= 0) sheet_draw(&shAlpha, m[0], x, y, sc, color, alpha);
		x += m[1] * sc;
	}
}

static float alpha_width(const char *s, float sc) {
	float w = 0;
	for (; *s; s++) {
		int c = (u8)*s;
		if (c < 32 || c > 126) c = '?';
		w += alphabet_map[c - 32][1] * sc;
	}
	return w;
}

void freeplay_draw(void) {
	u32 c = fp_song_week_color(fpSel);
	float sc0 = 0.27f;
	sheet_draw(&shDesat, 0, 128 - 1280 * sc0 / 2, 96 - 720 * sc0 / 2, sc0, HEXC(c), 31);
	for (int i = -3; i <= 3; i++) {
		int s = fpSel + i;
		if (s < 0 || s >= SONG_COUNT) continue;
		float d = s - fpScroll;
		float y = 96 + d * 34 - 12;
		float x = 30 + d * 10;
		float sc = 0.32f;
		float w = alpha_width(song_info[s].name, sc);
		if (w > 230) sc *= 230 / w;
		alpha_text(x, y, sc, song_info[s].name, WHITE, i == 0 ? 31 : 18);
	}
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
		sub_text(FONT_VCR8O, 20, y + 5, opNames[i], s ? RGB(30, 20, 10) : WHITE);
		const char *v = NULL;
		switch (i) {
		case 0: v = save.downscroll ? "ON" : "OFF"; break;
		case 1: v = save.ghostTapping ? "ON" : "OFF"; break;
		case 2: v = save.botplay ? "ON" : "OFF"; break;
		case 3: v = save.camZoom ? "ON" : "OFF"; break;
		case 4: v = save.flashing ? "ON" : "OFF"; break;
		case 5: snprintf(b, sizeof(b), "< %d ms >", save.noteOffset); v = b; break;
		}
		if (v) sub_text(FONT_VCR8O, 236 - strlen(v) * 8, y + 5, v, s ? RGB(30, 20, 10) : RGB(0x33, 0xFF, 0xFF));
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
