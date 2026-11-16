// GameOverSubState
#include "play.h"

static Character dead;
static Cam cam;
static float gx, gy, camTX, camTY, timer;
static int phase;            // 0 first death, 1 loop, 2 confirm
static bool pixel;

void gameover_enter(void) {
	const SongInfo *si = &song_info[g_song];
	pixel = si->pixel;
	chr_slots_reset();
	char tag[48];
	snprintf(tag, sizeof(tag), "%s_dead", si->player);
	if (!chr_load(&dead, tag, ROLE_BF)) chr_load(&dead, si->player, ROLE_BF);
	dead.dead = true;
	chr_place(&dead, &si->stage->chars[0]);
	chr_play(&dead, "firstDeath", true);
	if (dead.anim < 0) chr_play(&dead, "singUPmiss", true);
	cam.zoom = si->stage->zoom * 0.8f;
	cam.camX = P.camX;
	cam.camY = P.camY;
	camTX = dead.camFocusX;
	camTY = dead.camFocusY;
	gx = 0;
	audio_stop();
	audio_sfx(pixel ? SFX_LOSS_PIXEL : SFX_LOSS, 1);
	phase = 0;
	timer = 0;
	font_load(FONT_VCR12O, false);
	font_load(FONT_VCR8, false);
	sub_clear(RGB(0, 0, 0));
	sub_text_centered(FONT_VCR12O, 128, 70, "GAME OVER", RGB(255, 80, 80));
	sub_text_centered(FONT_VCR8, 128, 100, "A: retry    B: exit", WHITE);
	sub_present();
	(void)gy;
}

void gameover_leave(void) {
	audio_stop();
	chr_unload(&dead);
	chr_slots_reset();
}

void gameover_update(float dt) {
	timer += dt;
	chr_update(&dead, dt, 100, false);
	cam.camX += (camTX - cam.camX) * fps_ratio(0.01f, dt);
	cam.camY += (camTY - cam.camY) * fps_ratio(0.01f, dt);
	if (phase == 0 && (dead.finished || timer > 3)) {
		phase = 1;
		chr_play(&dead, "deathLoop", true);
		audio_play_music(pixel ? "nitro:/snd/gameover_pixel.adp" : "nitro:/snd/gameover.adp", true, 1);
	}
	if (phase < 2 && (kDown & KEY_A)) {
		phase = 2;
		timer = 0;
		chr_play(&dead, "deathConfirm", true);
		audio_play_music(pixel ? "nitro:/snd/gameoverend_pixel.adp" : "nitro:/snd/gameoverend.adp", false, 1);
	} else if (phase < 2 && (kDown & KEY_B)) {
		menu_music(false);
		state_switch(g_storyMode ? ST_STORY : ST_FREEPLAY);
		return;
	}
	if (phase == 2) {
		if (timer > 0.7f) g_fade = clampf((timer - 0.7f) / 2, 0, 1);
		if (timer > 2.8f) state_switch(ST_PLAY);
	}
}

void gameover_draw(void) {
	chr_draw(&dead, &cam);
}
