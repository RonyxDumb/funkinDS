// VideoCutscene: Weekend 1 videos (START skips)
#include "engine.h"

static float vT;
static bool vOk;

void video_enter(void) {
	char p[64];
	snprintf(p, sizeof(p), "romfs:/vid/%s.vid", g_video);
	vOk = video_open(p, false);
	snprintf(p, sizeof(p), "romfs:/snd/%s.adp", g_video);
	if (vOk) audio_play_music(p, false, 1);
	vT = 0;
	sub_clear(RGB(0, 0, 0));
	sub_text_centered(FONT_VCR12O, BW / 2, 200, "START: skip", RGB(150, 150, 150));
	g_fade = 0;
}

void video_leave(void) {
	video_close();
	audio_stop();
}

void video_update_state(float dt) {
	vT += dt;
	float ms = audio_music_playing() ? audio_music_time() : vT * 1000;
	if (!vOk || !video_update(ms) || (kDown & KEY_START) || (!audio_music_playing() && vT > 1 && ms >= video_duration()))
		state_switch(g_videoNext);
}

void video_draw(void) { video_draw_frame(0, 0, SW, SH, 255); }
