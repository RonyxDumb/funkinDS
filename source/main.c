// Funkin 3DS - entry point, state machine, save data
#include "engine.h"
#include "character.h"
#include <sys/stat.h>

u32 kDown, kHeld, kUp;
touchPosition touch;
bool touchDown;
float g_fade = 0;
float g_subFade = 0;
int g_song = 0;
int g_diff = 1;
bool g_storyMode = false;
int g_week = 0;
int g_weekPos = 0;
int g_weekScore = 0;
int g_optionsReturn = ST_MAINMENU;
bool g_titleSeen = false;
Results g_results;
u32 prof_max[PF_COUNT], prof_show[PF_COUNT];
static int profFrames;
const char *g_video;
int g_videoNext;
bool g_cutsceneDone;
int perf_drops, perf_frames;

const Cutscene *cutscene_for(const char *song, bool ending) {
	for (int i = 0; i < cutscene_count; i++)
		if (!strcmp(cutscenes[i].song, song) && cutscenes[i].ending == ending) return &cutscenes[i];
	return NULL;
}

Save save;
static int curState = -1, nextState = -1;

static const StateDef states[ST_COUNT] = {
	[ST_TITLE] = { title_enter, title_update, title_draw, title_leave },
	[ST_MAINMENU] = { mainmenu_enter, mainmenu_update, mainmenu_draw, mainmenu_leave },
	[ST_STORY] = { story_enter, story_update, story_draw, story_leave },
	[ST_FREEPLAY] = { freeplay_enter, freeplay_update, freeplay_draw, freeplay_leave },
	[ST_OPTIONS] = { options_enter, options_update, options_draw, options_leave },
	[ST_PLAY] = { play_enter, play_update, play_draw, play_leave },
	[ST_GAMEOVER] = { gameover_enter, gameover_update, gameover_draw, gameover_leave },
	[ST_CHARSELECT] = { charselect_enter, charselect_update, charselect_draw, charselect_leave },
	[ST_RESULTS] = { results_enter, results_update, results_draw, results_leave },
	[ST_VIDEO] = { video_enter, video_update_state, video_draw, video_leave },
};

const char *diff_name(int d) {
	static const char *n[5] = { "easy", "normal", "hard", "erect", "nightmare" };
	return n[d < 0 ? 0 : d > 4 ? 4 : d];
}

void *file_load(const char *path, u32 *size) {
	char full[128];
	snprintf(full, sizeof(full), "romfs:%s", path);
	FILE *f = fopen(full, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buf = malloc(n + 4);
	if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
	fclose(f);
	if (size) *size = n;
	return buf;
}

// ---------------------------------------------------------------------------
// save data (FlxSave "WwcdSave" + FunkinSave highscores + options)
// ---------------------------------------------------------------------------
#define SAVE_DIR "sdmc:/3ds/funkin3ds"
#define SAVE_PATH SAVE_DIR "/save.dat"
#define SAVE_MAGIC 0x33334B46   // "FK33"

static void save_defaults(void) {
	memset(&save, 0, sizeof(save));
	save.ghostTapping = true;
	save.camZoom = true;
	save.flashing = true;
	save.lastDiff = 1;
	save.depth3d = 100;
}

void save_load(void) {
	save_defaults();
	FILE *f = fopen(SAVE_PATH, "rb");
	if (!f) return;
	u32 magic = 0, size = 0;
	if (fread(&magic, 4, 1, f) == 1 && magic == SAVE_MAGIC && fread(&size, 4, 1, f) == 1 && size == sizeof(Save)) {
		Save s;
		if (fread(&s, sizeof(s), 1, f) == 1) save = s;
	}
	fclose(f);
}

void save_write(void) {
	mkdir("sdmc:/3ds", 0777);
	mkdir(SAVE_DIR, 0777);
	FILE *f = fopen(SAVE_PATH, "wb");
	if (!f) return;
	u32 magic = SAVE_MAGIC, size = sizeof(Save);
	fwrite(&magic, 4, 1, f);
	fwrite(&size, 4, 1, f);
	fwrite(&save, sizeof(save), 1, f);
	fclose(f);
}

// CoolUtil.playMenuSong(fadeIn)
void menu_music(bool fadeIn) {
	if (audio_music_playing()) return;
	audio_play_music("romfs:/snd/menu.adp", true, fadeIn ? 0 : 1);
	if (fadeIn) audio_fade_music(0.7f, 4);
}

void format_score(char *out, int score) {
	char b[16];
	bool neg = score < 0;
	snprintf(b, sizeof(b), "%d", neg ? -score : score);
	int len = strlen(b), o = 0;
	if (neg) out[o++] = '-';
	for (int i = 0; i < len; i++) {
		out[o++] = b[i];
		int rev = len - 1 - i;
		if (rev > 0 && rev % 3 == 0) out[o++] = ',';
	}
	out[o] = 0;
}

void state_switch(int st) { nextState = st; }

// screen transitions: every state change fades out (and fades the song
// out when leaving gameplay), then the new state fades in
static int trans;            // 0 none, 1 fading out, 2 fading in
float g_flash;               // white flash on both screens (FlxG.camera.flash)
static float flashDur = 1;

void screen_flash(float dur) {
	if (!save.flashing) return;
	g_flash = 1;
	flashDur = dur;
}

static void do_switch(void) {
	if (curState >= 0) states[curState].leave();
	tween_clear();
	video_close();
	sub_clear(0);
	curState = nextState;
	nextState = -1;
	states[curState].enter();
}

#ifdef AUTOTEST
// scripted key presses (seconds since boot) to check the screens in an emulator
typedef struct { float t; u32 key; } Step;
#if AUTOTEST == 2
// story mode week 6 (dialogue, pixel stage), options, character select
static const bool autoBot = true;
static const Step script[] = {
	{ 4, KEY_START }, { 6, KEY_A },
	{ 8, KEY_DOWN }, { 8.3f, KEY_DOWN }, { 8.6f, KEY_DOWN }, { 8.9f, KEY_DOWN }, { 9.2f, KEY_DOWN }, { 9.5f, KEY_DOWN },
	{ 12, KEY_A }, { 20, KEY_A }, { 22, KEY_A }, { 24, KEY_START },
	{ 45, KEY_START }, { 46, KEY_DOWN }, { 46.5f, KEY_DOWN }, { 47, KEY_A },
	{ 50, KEY_B }, { 53, KEY_DOWN }, { 53.5f, KEY_DOWN }, { 54, KEY_A },
	{ 58, KEY_DOWN }, { 59, KEY_B }, { 62, KEY_UP }, { 63, KEY_A },
	{ 66, KEY_SELECT }, { 69, KEY_RIGHT }, { 71, KEY_A }, { 76, KEY_B },
};
#elif AUTOTEST == 3
// weekend 1: video cutscene, philly streets
static const bool autoBot = true;
static const Step script[] = {
	{ 4, KEY_START }, { 6, KEY_A },
	{ 8, KEY_UP }, { 12, KEY_A }, { 18, KEY_START },
};
#elif AUTOTEST == 4
// tutorial to the end (results), then a song without botplay (game over)
static const bool autoBot = true;
static const Step script[] = {
	{ 4, KEY_START }, { 6, KEY_DOWN }, { 7, KEY_A }, { 10, KEY_A },
	{ 105, KEY_A }, { 110, KEY_DOWN }, { 111, KEY_A },
	{ 150, KEY_A }, { 156, KEY_B },
};
#else
// freeplay week 1 song, pause, story mode week 1
static const bool autoBot = true;
static const Step script[] = {
	{ 4, KEY_START }, { 6, KEY_DOWN }, { 7, KEY_A },                 // title -> main menu -> freeplay
	{ 10, KEY_DOWN }, { 11, KEY_DOWN }, { 12, KEY_A },               // a week 1 song
	{ 40, KEY_START }, { 43, KEY_START },                            // pause / resume
	{ 60, KEY_START }, { 61, KEY_DOWN }, { 62, KEY_DOWN }, { 63, KEY_A },   // exit to freeplay
	{ 66, KEY_B }, { 68, KEY_UP }, { 69, KEY_UP }, { 70, KEY_DOWN }, { 71, KEY_A },   // story mode
	{ 74, KEY_DOWN }, { 76, KEY_A },
};
#endif
static void autotest_keys(void) {
	static u32 t0;
	static int next;
	if (!t0) { t0 = time_ticks(); save.botplay = autoBot; }
#if AUTOTEST == 4
	if (curState == ST_RESULTS) save.botplay = false;
#endif
	float t = (time_ticks() - t0) / TICK_FREQ;
	if (next < (int)(sizeof(script) / sizeof(script[0])) && t >= script[next].t) {
		kDown |= script[next].key;
		kHeld |= script[next].key;
		next++;
	}
}
#endif

static void fatal(const char *msg) {
	consoleInit(GFX_BOTTOM, NULL);
	printf("\n\n  Funkin 3DS\n\n  %s\n\n  START: exit\n", msg);
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
	gfxExit();
	exit(0);
}

// fades and flashes over each screen
static void overlay(int screen, float fade) {
	gfx_screen(screen);
	float w = screen == SCREEN_TOP ? SW : BW;
	float fa = clampf(fade, 0, 1);
	fa = fa * fa * (3 - 2 * fa);
	if (fa > 0.004f) gfx_rect(-16, 0, w + 16, SH, BLACK, AL(fa));
	float fl = clampf(g_flash, 0, 1);
	if (fl > 0.004f && fa < 0.5f) gfx_rect(-16, 0, w + 16, SH, WHITE, AL(fl * 0.85f));
}

int main(int argc, char **argv) {
	osSetSpeedupEnable(true);    // New 3DS: 804 MHz + L2 cache
	gfx_init();
	Result rc = romfsInit();
	if (R_FAILED(rc)) fatal("RomFS init failed.");
	FILE *test = fopen("romfs:/font/vcr8.bin", "rb");
	if (!test) fatal("Game data not found in RomFS.\n  Convert the assets before building\n  (see README.md).");
	fclose(test);
	srand(12345);
	save_load();
	sub_init();
	audio_init();
#ifdef TEST_BOTPLAY
	save.botplay = true;
#endif

#ifdef START_SONG
	g_song = START_SONG;
	g_diff = 2;
	nextState = ST_PLAY;
#else
	nextState = ST_TITLE;
#endif
	u32 lastTicks = time_ticks();

	while (aptMainLoop()) {
		if (nextState >= 0 && trans != 1) {
			if (curState < 0) {
				do_switch();
				g_fade = 1;
				trans = 2;
				lastTicks = time_ticks();
			} else trans = 1;
		}
		hidScanInput();
		kDown = hidKeysDown();
		kHeld = hidKeysHeld();
		kUp = hidKeysUp();
#ifdef AUTOTEST
		autotest_keys();
#endif
		touchDown = (kHeld & KEY_TOUCH) != 0;
		if (touchDown) hidTouchRead(&touch);
		if ((kHeld & (KEY_START | KEY_SELECT | KEY_L | KEY_R)) == (KEY_START | KEY_SELECT | KEY_L | KEY_R)) break;   // quit

		u32 now = time_ticks();
		float dt = (now - lastTicks) / TICK_FREQ;
		lastTicks = now;
		if (curState == ST_PLAY) { perf_frames++; if (dt > 1.5f / 59.83f) perf_drops += (int)(dt * 59.83f + 0.5f) - 1; }
		if (dt > 0.1f) dt = 0.1f;
		if (dt < 0.001f) dt = 1.0f / 60;

		if (trans == 1) {
			// fading out: the state keeps drawing but takes no input
			g_fade += dt / 0.25f;
			if (curState == ST_PLAY || curState == ST_VIDEO || curState == ST_RESULTS || curState == ST_GAMEOVER ||
				nextState == ST_PLAY || nextState == ST_VIDEO)
				audio_set_volume(clampf(1 - g_fade, 0, 1));
			if (g_fade >= 1) {
				g_fade = 1;
				do_switch();
				g_fade = 1;
				g_flash = 0;
				trans = 2;
				lastTicks = time_ticks();
				continue;
			}
		} else {
			u32 t0 = time_ticks();
			states[curState].update(dt);
			prof_add(PF_UPDATE, t0);
			if (trans == 2 && nextState < 0) {
				g_fade -= dt / 0.3f;
				if (g_fade <= 0) { g_fade = 0; trans = 0; }
			}
		}
		tween_update(dt);
		if (g_flash > 0) g_flash -= dt / flashDur;

		u32 t1 = time_ticks();
		gfx_frame_begin();
		prof_add(PF_GPU, t1);
		t1 = time_ticks();
		chr_frame_begin();
		gfx_screen(SCREEN_TOP);
		states[curState].draw();
		overlay(SCREEN_TOP, g_fade);
		u32 t2 = time_ticks();
		sub_render();
		overlay(SCREEN_BOTTOM, g_fade > g_subFade ? g_fade : g_subFade);
		prof_add(PF_SUB, t2);
		gfx_frame_end();
		prof_add(PF_DRAW, t1);
		prof_add(PF_FRAME, now);
		if (++profFrames >= 60) { memcpy(prof_show, prof_max, sizeof(prof_max)); memset(prof_max, 0, sizeof(prof_max)); profFrames = 0; }
		t1 = time_ticks();
		audio_update();
		prof_add(PF_AUDIO, t1);
	}
	if (curState >= 0) states[curState].leave();
	audio_exit();
	romfsExit();
	gfx_exit();
	return 0;
}
