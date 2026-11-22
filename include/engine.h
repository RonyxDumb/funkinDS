// Funkin 3DS - small engine shared declarations
#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gen_assets.h"

// ---------------------------------------------------------------------------
// screens and coordinate helpers
// ---------------------------------------------------------------------------
#define SW 400                      // top screen
#define SH 240
#define BW 320                      // bottom screen
#define BH 240
#define VW 1280.0f
#define VH 720.0f
// virtual (HaxeFlixel) px -> top screen px: the whole 1280x720 view is
// 400x225, centered vertically (the game camera fills the 7 px above and below)
#define KH (400.0f / 1280.0f)
#define HUD_OY 7.5f
static inline float hx(float v) { return v * KH; }
static inline float hy(float v) { return v * KH + HUD_OY; }

static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline int AL(float a) { return (int)(clampf(a, 0, 1) * 255 + 0.5f); }
// flixel fps-independent lerp ratio (CoolUtil.getFPSRatio)
static inline float fps_ratio(float r, float dt) { return 1.0f - powf(1.0f - r, dt * 60.0f); }
static inline float frand(void) { return (rand() & 0xFFFF) / 65536.0f; }
static inline float frand_range(float a, float b) { return a + (b - a) * frand(); }
static inline int irand_range(int a, int b) { return a + rand() % (b - a + 1); }

// ---------------------------------------------------------------------------
// files (romfs)
// ---------------------------------------------------------------------------
void *file_load(const char *path, u32 *size);   // malloc'd, NULL on failure

// ---------------------------------------------------------------------------
// GPU renderer: textured quads batched per screen, drawn at the end of the
// frame (twice on the top screen in stereoscopic 3D)
// ---------------------------------------------------------------------------
#define RGB(r, g, b) ((u32)(((r) & 255) << 16 | ((g) & 255) << 8 | ((b) & 255)))
#define HEXC(c) ((u32)(c) & 0xFFFFFF)
#define WHITE 0xFFFFFFu
#define BLACK 0x000000u
static inline u32 color_lerp(u32 a, u32 b, float t) {
	int r = (int)lerpf((a >> 16) & 255, (b >> 16) & 255, t), g = (int)lerpf((a >> 8) & 255, (b >> 8) & 255, t),
		bb = (int)lerpf(a & 255, b & 255, t);
	return RGB(r, g, bb);
}

enum { SCREEN_TOP, SCREEN_BOTTOM };

typedef struct {
	const SheetDef *def;
	C3D_Tex *tex;           // nTex textures
	bool loaded;
} Sheet;

void gfx_init(void);
void gfx_exit(void);
void gfx_frame_begin(void);     // waits for the GPU, opens the draw lists
void gfx_frame_end(void);       // renders the lists, swaps
void gfx_screen(int screen);    // draw list the next draws go to
void gfx_depth(float d);        // stereo depth of the next draws (top screen px at full 3D, > 0 = behind)
void gfx_blend(int mode);       // BLEND_*
void gfx_flush_textures(void);  // deferred texture frees (after the GPU is done with them)
float gfx_stereo(void);         // 3D slider (0 = 2D)

C3D_Tex *tex_new(int w, int h, GPU_TEXCOLOR fmt, bool linear);
void tex_delete(C3D_Tex *t);    // deferred
void tex_upload_done(C3D_Tex *t);   // flush the CPU cache after writing t->data
u32 tex_tiled_offset(int x, int y, int w);   // texel index of (x, y) in a tiled texture of width w
extern C3D_Tex *g_whiteTex;

bool sheet_load(Sheet *s, const SheetDef *d);
void sheet_unload(Sheet *s);
void sheet_draw(Sheet *s, int frame, float x, float y, float scale, u32 color, int alpha);
void sheet_draw_ex(Sheet *s, int frame, float x, float y, float sx, float sy, u32 color, int alpha);
// flip: bit0 = mirror horizontally around x, bit1 = vertically around y
void sheet_draw_flip(Sheet *s, int frame, float x, float y, float sx, float sy, int flip, u32 color, int alpha);
// rotated around (cx, cy) (frame origin at x, y), angle in radians
void sheet_draw_rot(Sheet *s, int frame, float x, float y, float sx, float sy, float cx, float cy, float ang, u32 color, int alpha);
// sub-rectangle of a single-part frame: u/v fractions in [0,1]
void sheet_draw_sub(Sheet *s, int frame, float x0, float y0, float x1, float y1, float fu0, float fv0, float fu1, float fv1, u32 color, int alpha);
void sheet_draw_rect(Sheet *s, int frame, float x0, float y0, float x1, float y1, float fv0, float fv1, u32 color, int alpha);
void sheet_frame_bounds(const Sheet *s, int frame, float *x0, float *y0, float *x1, float *y1);
void gfx_rect(float x0, float y0, float x1, float y1, u32 color, int alpha);
void gfx_gradient(float x0, float y0, float x1, float y1, u32 top, u32 bottom, int alpha);
void gfx_gradient_h(float x0, float y0, float x1, float y1, u32 left, u32 right, int alpha);
void gfx_quad(C3D_Tex *t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 color, int alpha);
// arbitrary quad: corners (x, y) in order top-left, top-right, bottom-right, bottom-left
void gfx_quad4(C3D_Tex *t, const float *xy, float u0, float v0, float u1, float v1, u32 color, int alpha);

// menus: the whole 1280x720 view in 400x225 (see hx / hy)
#define MS KH
static inline float mx(float v) { return hx(v); }
static inline float my(float v) { return hy(v); }

// fonts
void font_load(int id);
Sheet *font_sheet(int id);
int font_width(int id, const char *s);
int font_height(int id);
void font_draw(int id, float x, float y, const char *s, u32 color, int alpha);
void font_draw_scaled(int id, float x, float y, float sc, const char *s, u32 color, int alpha);
void font_draw_centered(int id, float cx, float y, const char *s, u32 color, int alpha);
void font_draw_right(int id, float rx, float y, const char *s, u32 color, int alpha);

// ---------------------------------------------------------------------------
// bottom screen: a retained display list (redrawn by the GPU every frame)
// ---------------------------------------------------------------------------
void sub_init(void);
void sub_render(void);          // replays the list on the bottom screen
void sub_present(void);
void sub_clear(u32 color);
void sub_fill(int x, int y, int w, int h, u32 color);
void sub_fill_blend(int x, int y, int w, int h, u32 color, int a255);
void sub_gradient(int x, int y, int w, int h, u32 top, u32 bottom);
void sub_rect(int x, int y, int w, int h, u32 color);
void sub_button(int x, int y, int w, int h, bool sel);   // rounded menu button
void sub_text(int font, int x, int y, const char *s, u32 color);
void sub_text_centered(int font, int cx, int y, const char *s, u32 color);
void sub_text_right(int font, int rx, int y, const char *s, u32 color);
void sub_sprite(Sheet *sh, int frame, float x, float y, float scale, bool flip);
// immediate (per frame) draws on the bottom screen, over the display list
typedef void (*SubHook)(void);
void sub_set_hook(SubHook h);

// ---------------------------------------------------------------------------
// audio (NDSP): two streamed IMA ADPCM tracks + sound effects
// ---------------------------------------------------------------------------
enum { TR_MUSIC, TR_VOICES, TR_COUNT };
void audio_init(void);
void audio_exit(void);
void audio_update(void);
void audio_play_music(const char *path, bool loop, float volume);
void audio_play_song(const char *inst, const char *voices);
void audio_stop(void);
void audio_stop_music(void);
void audio_set_volume(float v);          // music / inst volume
float audio_get_volume(void);
void audio_set_voices(float v);
void audio_set_pitch(float p);           // music pitch
void audio_pause_song(bool paused);
bool audio_song_finished(void);
bool audio_music_playing(void);
float audio_song_time(void);             // ms
float audio_music_time(void);            // ms (menu music)
float audio_song_length(void);           // ms (0 = unknown)
void audio_sfx(int id, float vol);
void audio_sfx_pitch(int id, float vol, float pitch);
void audio_fade_music(float to, float dur);
bool audio_ok(void);

// time: svcGetSystemTick / 256 (~1.05 MHz)
#define TICK_FREQ ((float)SYSCLOCK_ARM11 / 256.0f)
static inline u32 time_ticks(void) { return (u32)(svcGetSystemTick() >> 8); }

// ---------------------------------------------------------------------------
// video (Weekend 1 cutscenes): decoded into a texture, drawn with video_draw_frame
// ---------------------------------------------------------------------------
bool video_open(const char *path, bool loop);
void video_close(void);
bool video_active(void);
bool video_update(float ms);
void video_draw_frame(float x0, float y0, float x1, float y1, int alpha);
float video_duration(void);

// ---------------------------------------------------------------------------
// tweens
// ---------------------------------------------------------------------------
enum {
	EASE_LINEAR = 0,
	EASE_QUADIN = 3, EASE_QUADOUT, EASE_QUADINOUT,
	EASE_CUBEIN, EASE_CUBEOUT, EASE_CUBEINOUT,
	EASE_QUARTIN, EASE_QUARTOUT, EASE_QUARTINOUT,
	EASE_QUINTIN, EASE_QUINTOUT, EASE_QUINTINOUT,
	EASE_SINEIN, EASE_SINEOUT, EASE_SINEINOUT,
	EASE_CIRCIN, EASE_CIRCOUT, EASE_CIRCINOUT,
	EASE_EXPOIN, EASE_EXPOOUT, EASE_EXPOINOUT,
	EASE_BACKIN, EASE_BACKOUT, EASE_BACKINOUT,
	EASE_CLASSIC = 255
};
typedef void (*TweenCb)(void *ctx);
int tween_num(float *target, float from, float to, float dur, int ease, float delay, TweenCb cb, void *ctx);
int tween_to(float *target, float to, float dur, int ease, float delay, TweenCb cb, void *ctx);
void tween_cancel(int id);
bool tween_active(int id);
void tween_cancel_target(float *target);
void tween_update(float dt);
void tween_clear(void);
int timer_after(float delay, TweenCb cb, void *ctx);
float ease_apply(int ease, float t);

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------
extern u32 kDown, kHeld, kUp;
extern touchPosition touch;
extern bool touchDown;
#define KEY_ACCEPT (KEY_A | KEY_START)
#define KEY_BACK (KEY_B)
static inline bool touch_in(int x, int y, int w, int h) {
	return (kDown & KEY_TOUCH) && touch.px >= x && touch.px < x + w && touch.py >= y && touch.py < y + h;
}

// ---------------------------------------------------------------------------
// save data (preferences + highscores)
// ---------------------------------------------------------------------------
typedef struct {
	bool downscroll, ghostTapping, botplay, flashing, camZoom;
	int noteOffset;                       // ms
	int songScore[SONG_COUNT][DIFF_COUNT];
	int weekScore[WEEK_COUNT][DIFF_COUNT];
	u8 songRank[SONG_COUNT][DIFF_COUNT];
	int lastDiff;
	int player;                           // 0 bf, 1 pico
	int lastFreeplay;
	int depth3d;                          // stereoscopic depth, percent
	bool middlescroll;
} Save;
extern Save save;
void save_load(void);
void save_write(void);

// ---------------------------------------------------------------------------
// states
// ---------------------------------------------------------------------------
enum { ST_TITLE, ST_MAINMENU, ST_STORY, ST_FREEPLAY, ST_OPTIONS, ST_PLAY, ST_GAMEOVER, ST_CHARSELECT, ST_RESULTS, ST_VIDEO, ST_COUNT };
void state_switch(int st);
typedef struct { void (*enter)(void); void (*update)(float); void (*draw)(void); void (*leave)(void); } StateDef;
#define DECL_STATE(n) void n##_enter(void); void n##_update(float dt); void n##_draw(void); void n##_leave(void);
DECL_STATE(title) DECL_STATE(mainmenu) DECL_STATE(story) DECL_STATE(freeplay) DECL_STATE(options) DECL_STATE(play)
DECL_STATE(gameover) DECL_STATE(charselect) DECL_STATE(results)
void video_enter(void); void video_update_state(float dt); void video_draw(void); void video_leave(void);

// song selection (PlayStatePlaylist)
extern int g_song;              // song_info index
extern int g_diff;              // 0 easy, 1 normal, 2 hard, 3 erect, 4 nightmare
extern bool g_storyMode;
extern int g_week;
extern int g_weekPos;           // index in the week playlist
extern int g_weekScore;
extern int g_optionsReturn;     // state to go back to from the options menu
extern bool g_titleSeen;

// fades: 0 = visible, 1 = black
extern float g_fade;
extern float g_subFade;
extern float g_flash;
void screen_flash(float dur);   // white flash on both screens (honours the flashing lights option)

// profiler (DEBUG_HUD): max microseconds per section over the last second
enum { PF_UPDATE, PF_DRAW, PF_GPU, PF_SUB, PF_AUDIO, PF_DECOMP, PF_SUBDRAW, PF_FRAME, PF_COUNT };
extern u32 prof_max[PF_COUNT], prof_show[PF_COUNT];
static inline u32 prof_us(u32 t0) { return (u32)((u64)(time_ticks() - t0) * 1000000 / (u64)TICK_FREQ); }
static inline void prof_add(int k, u32 t0) { u32 u = prof_us(t0); if (u > prof_max[k]) prof_max[k] = u; }

void menu_music(bool fadeIn);

// results (ResultState): totals of the song, or of the whole week in story mode
typedef struct { int score, misses, maxCombo, notes; int judged[4]; char title[48]; bool newBest; } Results;
extern Results g_results;

// video cutscenes (VideoCutscene)
extern const char *g_video;      // video to play in ST_VIDEO
extern int g_videoNext;          // state after it
extern bool g_cutsceneDone;      // the start cutscene of the current song was shown
const Cutscene *cutscene_for(const char *song, bool ending);
void format_score(char *out, int score);
const char *diff_name(int d);
