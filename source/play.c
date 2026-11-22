// PlayState: a port of Friday Night Funkin' (V-Slice) PlayState / Strumline /
// Conductor / scoring behaviour. The top screen shows the whole V-Slice
// layout (1280x720 view in 400x225, the game camera fills the 400x240 screen).
#include "play.h"

PlayState P;

#define HIT_WINDOW 160.0f          // Constants.HIT_WINDOW_MS / PBOT1_MISS_THRESHOLD
#define NOTE_SPACING 112.0f       // Strumline.NOTE_SPACING (hud units between lanes)
static const u32 laneKeys[4] = { KEY_LEFT | KEY_Y | KEY_ZL, KEY_DOWN | KEY_B, KEY_UP | KEY_X, KEY_RIGHT | KEY_A | KEY_ZR };
static bool subDirty;
static bool subStatic;     // bottom screen static part drawn
static int pauseSel;
static float botHoldEnd[4];

// ---------------------------------------------------------------------------
// conductor
// ---------------------------------------------------------------------------
static int time_index(float pos) {
	int i = 0;
	while (i + 1 < P.chart->nTime && P.times[i + 1].t <= pos) i++;
	return i;
}

static void conductor_update(void) {
	int i = time_index(P.songPos);
	float steps = 0;
	for (int k = 0; k < i; k++) steps += (P.times[k + 1].t - P.times[k].t) / (15000.0f / P.times[k].bpm);
	P.stepMs = 15000.0f / P.times[i].bpm;
	P.crochet = P.stepMs * 4;
	P.stepF = steps + (P.songPos - P.times[i].t) / P.stepMs;
	P.beatF = P.stepF / 4;
}

// ---------------------------------------------------------------------------
// notes
// ---------------------------------------------------------------------------
static void strum_setup(Strum *s, const ChartNote *src, int n, bool cpu) {
	memset(s, 0, sizeof(*s));
	s->cpu = cpu;
	s->notes = malloc(sizeof(Note) * (n ? n : 1));
	for (int i = 0; i < n; i++) {
		Note *d = &s->notes[i];
		memset(d, 0, sizeof(*d));
		d->t = src[i].t;
		d->len = src[i].len;
		d->dir = src[i].dir & 3;
		d->kind = src[i].kind;
	}
	s->n = n;
	s->scale = 1;
	// PlayState: opponent strumline at x = STRUMLINE_X_OFFSET (48), the player
	// one at FlxG.width / 2 + 48; arrow centers 25 units right of the lane origin
	float x0 = cpu ? 48 : 640 + 48;
	for (int i = 0; i < 4; i++) s->cx[i] = hx(x0 + 25 + i * NOTE_SPACING);
}

static float strum_y(void) {
	// strumline y = 24 (STRUMLINE_Y_OFFSET), center = y + 52 (hud units)
	return save.downscroll ? hy(720 - 24 - 52) : hy(24 + 52);
}

static Character *sl_char(int idx) { return idx == 0 ? P.chars[ROLE_BF] : P.chars[ROLE_DAD]; }

static void popup_rating(int r) {
	static int rr;
	bool pixel = P.info->pixel;
	const float (*sz)[2] = pixel ? popup_pixel_size : popup_funkin_size;
	// rating sprite (PopUpStuff.displayRating)
	Popup *p = &P.pops[rr++ % 16];
	memset(p, 0, sizeof(*p));
	p->used = true;
	p->frame = r;
	p->x = 1280 * 0.474f - sz[r][0] / 2;
	p->y = 720 * 0.45f - 60 - sz[r][1] / 2;
	p->ay = 550;
	p->vy = -(float)irand_range(140, 175);
	p->vx = -(float)irand_range(0, 10);
	p->alpha = 1;
	p->fadeDelay = P.crochet * 0.001f;
	if (P.combo < 10 && P.combo != 0) return;
	char b[12];
	snprintf(b, sizeof(b), "%03d", P.combo);
	int len = strlen(b);
	for (int i = 0; i < len; i++) {
		int dgt = b[i] - '0';
		p = &P.pops[rr++ % 16];
		memset(p, 0, sizeof(*p));
		p->used = true;
		p->frame = 4 + dgt;
		p->x = 1280 * 0.507f - 90 + i * (pixel ? 36 : 36) - sz[4 + dgt][0] / 2 + 60;
		p->y = 720 * 0.44f + 80 - sz[4 + dgt][1] / 2;
		p->ay = irand_range(250, 300);
		p->vy = -(float)irand_range(130, 150);
		p->vx = frand_range(-5, 5);
		p->alpha = 1;
		p->fadeDelay = P.crochet * 0.002f;
	}
}

static void splash_show(int lane) {
	for (int i = 0; i < 6; i++) {
		if (!P.splash[i].used) {
			P.splash[i].used = true;
			P.splash[i].lane = lane;
			P.splash[i].var = rand() & 1;
			P.splash[i].t = 0;
			return;
		}
	}
}

static void vocals(bool on) { audio_set_voices(on ? 1 : 0); }

static int pbot1_score(float ms) {
	float a = fabsf(ms);
	if (a > 160) return -100;
	if (a < 5) return 500;
	float factor = 1.0f - 1.0f / (1.0f + expf(-0.080f * (a - 54.99f)));
	return (int)(500 * factor + 9);
}

// note kinds: scripted character reactions (all characters react to every
// note of that kind), then the singer's default behaviour
static bool kind_anim(Character *c, int kind, bool hit) {
	if (!c || !kind) return false;
	for (int i = 0; i < kind_anim_count; i++) {
		const KindAnim *k = &kind_anims[i];
		if (k->kind != kind || strcmp(k->chr, c->id)) continue;
		const char *a = hit ? k->hit : k->miss;
		if (!a || !*a) return false;
		chr_play_any(c, a);
		c->holdTimer = 0;
		return true;
	}
	return false;
}

static void note_react(Character *singer, int kind, int dir, bool hit) {
	bool handled = false;
	for (int r = 0; r < 3; r++)
		if (kind_anim(P.chars[r], kind, hit) && P.chars[r] == singer) handled = true;
	if (handled || !singer) return;
	const char *k = kind < NOTE_KIND_COUNT ? note_kinds[kind] : "";
	if (!strcmp(k, "noanim")) return;
	if (!hit) { chr_sing(singer, dir, true, NULL); return; }
	if (!strcmp(k, "mom")) chr_sing(singer, dir, false, "alt");
	else chr_sing(singer, dir, false, NULL);
}

static void note_hit(int idx, Note *n) {
	Strum *s = &P.sl[idx];
	Character *c = sl_char(idx);
	if (!s->cpu) {
		float diff = n->t - P.songPos;
		float a = fabsf(diff);
		int r = a <= 45 ? 0 : a <= 90 ? 1 : a <= 135 ? 2 : 3;
		static const float hp[4] = { 0.03f, 0.015f, 0.0f, -0.02f };
		P.score += pbot1_score(diff);
		P.health += hp[r];
		P.judged[r]++;
		if (r <= 1) {
			P.combo++;
			if (P.combo > P.maxCombo) P.maxCombo = P.combo;
		} else P.combo = 0;
		popup_rating(r);
		if (r == 0) splash_show(n->dir);
		vocals(true);
	}
	note_react(c, n->kind, n->dir, true);
	s->anim[n->dir] = 2;
	s->animTime[n->dir] = 0;
	s->cpuRelease[n->dir] = n->t + (n->len > 0 ? n->len : 0) + P.crochet / 4;
	n->state = n->len > 0 ? NS_HELD : NS_DONE;
	if (n->len > 0) n->holdEnd = n->t;
	subDirty = true;
}

static void note_miss(int idx, Note *n, int dir) {
	P.health -= 0.08f;
	P.score -= n ? 100 : 10;
	P.combo = 0;
	P.misses++;
	audio_sfx(SFX_MISSNOTE1 + rand() % 3, frand_range(0.5f, 0.6f));
	vocals(false);
	note_react(sl_char(idx), n ? n->kind : 0, dir, false);
	if (n) n->state = NS_MISSED;
	subDirty = true;
}

static void update_notes(int idx, float dt) {
	Strum *s = &P.sl[idx];
	float pos = P.songPos;
	while (s->first < s->n && (s->notes[s->first].state == NS_DONE || s->notes[s->first].state == NS_MISSED) &&
		s->notes[s->first].t + s->notes[s->first].len < pos - 400) s->first++;
	for (int d = 0; d < 4; d++) s->holding[d] = false;
	for (int i = s->first; i < s->n; i++) {
		Note *n = &s->notes[i];
		if (n->t > pos + 3000) break;
		if (n->state == NS_PENDING) {
			if (s->cpu && n->t <= pos) note_hit(idx, n);
			else if (!s->cpu && n->t < pos - HIT_WINDOW) note_miss(idx, n, n->dir);
		}
		if (n->state == NS_HELD) {
			float end = n->t + n->len;
			if (s->cpu || (kHeld & laneKeys[n->dir]) || save.botplay) {
				n->holdEnd = pos < end ? pos : end;
				s->holding[n->dir] = true;
				if (!s->cpu) {
					P.health += 0.12f * dt;
					P.score += (int)(250 * dt + 0.5f);
				}
				// keep singing while holding
				Character *c = sl_char(idx);
				if (c && chr_is_singing(c)) c->holdTimer = 0;
				if (pos >= end) n->state = NS_DONE;
			} else {
				// released too early (hold dropped)
				n->state = NS_MISSED;
				vocals(false);
				P.combo = 0;
			}
		}
	}
}

static void update_input(void) {
	Strum *s = &P.sl[0];
	float pos = P.songPos;
	bool just[4], held[4], rel[4];
	for (int d = 0; d < 4; d++) {
		just[d] = (kDown & laneKeys[d]) != 0;
		held[d] = (kHeld & laneKeys[d]) != 0;
		rel[d] = (kUp & laneKeys[d]) && !held[d];
	}
	if (save.botplay) {
		for (int d = 0; d < 4; d++) { just[d] = false; held[d] = botHoldEnd[d] > pos; rel[d] = false; }
		for (int i = s->first; i < s->n; i++) {
			Note *n = &s->notes[i];
			if (n->t > pos) break;
			if (n->state == NS_PENDING) { just[n->dir] = true; held[n->dir] = true; botHoldEnd[n->dir] = n->t + n->len + 60; }
		}
	}
	for (int d = 0; d < 4; d++) {
		if (!just[d]) continue;
		Note *best = NULL;
		for (int i = s->first; i < s->n; i++) {
			Note *n = &s->notes[i];
			if (n->t > pos + HIT_WINDOW) break;
			if (n->state != NS_PENDING || n->dir != d) continue;
			if (n->t < pos - HIT_WINDOW) continue;
			best = n;
			break;
		}
		if (best) note_hit(0, best);
		else {
			if (!save.ghostTapping) note_miss(0, NULL, d);
			s->anim[d] = 1;
			s->animTime[d] = 0;
		}
	}
	for (int d = 0; d < 4; d++) {
		if (!held[d] && !s->holding[d] && s->anim[d] != 0) s->anim[d] = 0;
		(void)rel;
	}
}

// ---------------------------------------------------------------------------
// camera + events
// ---------------------------------------------------------------------------
static void cb_follow_on(void *ctx) { P.followEnabled = true; P.camTweenId = 0; }
static void cb_zoom_done(void *ctx) { P.zoomTween = 0; }

static void cam_cancel_tween(void) {
	if (P.camTweenId) {
		tween_cancel_target(&P.camX);
		tween_cancel_target(&P.camY);
		P.camTweenId = 0;
	}
	P.followEnabled = true;
}

static void run_event(const ChartEvent *e) {
	switch (e->type) {
	case CEV_FOCUS: {
		float tx = e->f[0], ty = e->f[1];
		Character *c = NULL;
		if (e->i0 == 0) c = P.chars[ROLE_BF];
		else if (e->i0 == 1) c = P.chars[ROLE_DAD];
		else if (e->i0 == 2) c = P.chars[ROLE_GF];
		if (e->i0 >= 0 && e->i0 <= 2) {
			if (!c) break;
			tx += c->camFocusX;
			ty += c->camFocusY;
		}
		cam_cancel_tween();
		P.followX = tx;
		P.followY = ty;
		if (e->ease == 255) break;                       // CLASSIC: follow lerp
		float dur = e->ease == 254 ? 0 : P.stepMs * e->f[2] / 1000;
		if (dur <= 0) { P.camX = tx; P.camY = ty; break; }
		P.followEnabled = false;
		P.camTweenId = tween_to(&P.camX, tx, dur, e->ease, 0, cb_follow_on, NULL);
		tween_to(&P.camY, ty, dur, e->ease, 0, NULL, NULL);
		break;
	}
	case CEV_ZOOM: {
		float z = e->f[0] * (e->i0 ? 1 : P.stageZoom);
		if (P.zoomTween) { tween_cancel(P.zoomTween); P.zoomTween = 0; }
		float dur = e->ease == 254 ? 0 : P.stepMs * e->f[1] / 1000;
		if (dur <= 0) P.currentZoom = z;
		else P.zoomTween = tween_to(&P.currentZoom, z, dur, e->ease == 255 ? EASE_LINEAR : e->ease, 0, cb_zoom_done, NULL);
		break;
	}
	case CEV_BOP:
		P.zoomRate = e->f[0];
		P.bopIntensity = 0.015f * e->f[1] + 1;
		P.hudBop = 0.015f * e->f[1] * 2;
		P.zoomOffset = e->f[2];
		break;
	case CEV_ANIM: {
		Character *c = e->i0 == 0 ? P.chars[ROLE_BF] : e->i0 == 1 ? P.chars[ROLE_DAD] : e->i0 == 2 ? P.chars[ROLE_GF] : NULL;
		if (c) chr_play(c, e->s, e->f[0] != 0);
		else if (e->i0 == 3) stage_prop_play(e->s, e->s, true);
		break;
	}
	case CEV_SCROLL: {
		float sp = e->f[0] * (e->i0 ? 1 : P.chart->speed);
		if (P.scrollTween) tween_cancel(P.scrollTween);
		P.scrollTween = 0;
		float dur = e->ease == 254 ? 0 : P.stepMs * e->f[1] / 1000;
		if (dur <= 0) P.scrollSpeed = sp;
		else P.scrollTween = tween_to(&P.scrollSpeed, sp, dur, e->ease == 255 ? EASE_LINEAR : e->ease, 0, NULL, NULL);
		break;
	}
	}
}

// ---------------------------------------------------------------------------
// dialogue (Conversation / DialogueBox / Speaker)
// ---------------------------------------------------------------------------
static int dialogueSong = -1;

static void sub_header_dialogue(void) {
	sub_gradient(0, 0, BW, BH, RGB(40, 24, 72), RGB(12, 8, 24));
	sub_text_centered(FONT_VCR16O, BW / 2, 50, P.info->name, WHITE);
	sub_button(60, 110, 200, 34, true);
	sub_text_centered(FONT_VCR12O, BW / 2, 119, "A: next", RGB(40, 24, 10));
	sub_button(60, 156, 200, 34, false);
	sub_text_centered(FONT_VCR12O, BW / 2, 165, "START: skip", WHITE);
}

static void dlg_load_line(void) {
	const DialogueLine *l = &P.dlg->lines[P.dlgLine];
	if (l->box != P.dlgBoxId) {
		sheet_unload(&P.shDlgBox);
		sheet_load(&P.shDlgBox, dlg_boxes[l->box].sheet);
		P.dlgBoxId = l->box;
	}
	bool have = false;
	for (int i = 0; i < 4; i++) if (P.dlgSpkId[i] == l->speaker && P.shDlgSpk[i].loaded) have = true;
	if (!have) {
		int slot = P.dlgLine & 3;
		sheet_unload(&P.shDlgSpk[slot]);
		sheet_load(&P.shDlgSpk[slot], dlg_speakers[l->speaker].sheet);
		P.dlgSpkId[slot] = l->speaker;
	}
	P.dlgChars = 0;
}

static void dlg_end(void) {
	for (int i = 0; i < 4; i++) sheet_unload(&P.shDlgSpk[i]);
	sheet_unload(&P.shDlgBox);
	if (P.dlg->music[0]) audio_stop_music();
	P.dlg = NULL;
	sheet_load(&P.shCountdown, P.info->pixel ? &sheet_countdown_pixel : &sheet_countdown_funkin);
	subDirty = true;
	subStatic = false;
}

static void dlg_start(void) {
	for (int i = 0; i < conversation_count; i++)
		if (!strcmp(conversations[i].song, P.info->tag)) P.dlg = &conversations[i];
	if (!P.dlg) return;
	dialogueSong = g_song;
	for (int i = 0; i < 4; i++) P.dlgSpkId[i] = -1;
	P.dlgBoxId = -1;
	P.dlgLine = 0;
	P.dlgFade = 0;
	font_load(FONT_VCR12O);
	if (P.dlg->music[0]) {
		char m[48];
		snprintf(m, sizeof(m), "romfs:/snd/%s.adp", P.dlg->music);
		audio_play_music(m, true, 0);
		audio_fade_music(0.8f, 2);
	}
	dlg_load_line();
	sub_header_dialogue();
}

static void dlg_update(float dt) {
	P.dlgFade = clampf(P.dlgFade + dt / 2, 0, 1);
	const char *t = P.dlg->lines[P.dlgLine].text;
	int len = strlen(t);
	P.dlgChars += dt * 24;
	if (kDown & KEY_START) { dlg_end(); return; }
	if (kDown & (KEY_A | KEY_TOUCH)) {
		if (P.dlgChars < len) P.dlgChars = len;
		else {
			audio_sfx(SFX_SCROLL, 0.6f);
			if (++P.dlgLine >= P.dlg->n) { dlg_end(); return; }
			dlg_load_line();
		}
	}
}

static void dlg_draw(void) {
	u32 bg = P.dlg->bg;
	gfx_depth(-1);
	gfx_rect(-16, 0, SW + 16, SH, HEXC(bg), AL(0.7f * P.dlgFade));
	const DialogueLine *l = &P.dlg->lines[P.dlgLine];
	for (int i = 0; i < 4; i++) {
		if (P.dlgSpkId[i] != l->speaker || !P.shDlgSpk[i].loaded) continue;
		const DialogueSprite *d = &dlg_speakers[l->speaker];
		float x = hx(d->x), w = d->w * KH;
		if (x + w > SW + 6) x = SW + 6 - w;
		if (x < -6) x = -6;
		sheet_draw(&P.shDlgSpk[i], 0, x, hy(d->y), KH, WHITE, 255);
	}
	const DialogueSprite *b = &dlg_boxes[l->box];
	// keep the box on the screen (its sprite may be wider than the trimmed frame)
	float bx = hx(b->x), shift = 0;
	if (P.shDlgBox.loaded) {
		float fx0, fy0, fx1, fy1;
		sheet_frame_bounds(&P.shDlgBox, 0, &fx0, &fy0, &fx1, &fy1);
		float r = bx + fx1 * KH, l0 = bx + fx0 * KH;
		if (r > SW - 2) shift = SW - 2 - r;
		if (l0 + shift < 2) shift = 2 - l0;
		sheet_draw(&P.shDlgBox, 0, bx + shift, hy(b->y), KH, WHITE, 255);
	}
	// text (typewriter), wrapped inside the box
	char buf[256];
	int n = (int)P.dlgChars;
	snprintf(buf, sizeof(buf), "%.*s", n, l->text);
	float x0 = hx(b->x + b->w * 0.12f) + shift, y = hy(b->y + b->h * 0.56f), maxw = b->w * 0.76f * KH;
	if (x0 < 30) x0 = 30;
	if (x0 + maxw > SW - 30) maxw = SW - 30 - x0;
	char line[240];
	line[0] = 0;
	int ll = 0;
	const char *p = buf;
	int lh = font_height(FONT_VCR12O) + 1;
	while (*p) {
		const char *w = p;
		while (*p && *p != ' ') p++;
		char word[64];
		int wl = p - w < 63 ? p - w : 63;
		memcpy(word, w, wl);
		word[wl] = 0;
		char test[240];
		snprintf(test, sizeof(test), "%s%s%s", line, ll ? " " : "", word);
		if (ll && font_width(FONT_VCR12O, test) > maxw) {
			font_draw(FONT_VCR12O, x0, y, line, dlg_box_color[l->box], 255);
			y += lh;
			snprintf(line, sizeof(line), "%s", word);
		} else snprintf(line, sizeof(line), "%s", test);
		ll = strlen(line);
		if (*p) p++;
	}
	if (ll) font_draw(FONT_VCR12O, x0, y, line, dlg_box_color[l->box], 255);
	gfx_depth(0);
}

// ---------------------------------------------------------------------------
// enter / leave
// ---------------------------------------------------------------------------
static bool load_icon(Sheet *sh, const char *name) {
	for (int i = 0; i < icon_count; i++)
		if (!strcmp(icon_defs[i].name, name)) return sheet_load(sh, icon_defs[i].sheet);
	return false;
}

void play_enter(void) {
	g_fade = 1;
	audio_stop();
	memset(&P, 0, sizeof(P));
	subStatic = false;
	P.info = &song_info[g_song];
	// story mode start cutscene (songs/darnell.hxc)
	const Cutscene *cs = g_storyMode && !g_cutsceneDone ? cutscene_for(P.info->tag, false) : NULL;
	g_cutsceneDone = true;
	if (cs) {
		g_video = cs->video;
		g_videoNext = ST_PLAY;
		P.chart = NULL;
		state_switch(ST_VIDEO);
		return;
	}
	char path[64];
	snprintf(path, sizeof(path), "/song/%s-%s.cht", P.info->tag, diff_name(g_diff));
	u32 size;
	P.chartData = file_load(path, &size);
	if (!P.chartData) { state_switch(ST_MAINMENU); return; }
	P.chart = (const ChartHeader *)P.chartData;
	P.times = (const TimeChange *)(P.chartData + sizeof(ChartHeader));
	P.events = (const ChartEvent *)(P.times + P.chart->nTime);
	const ChartNote *notes = (const ChartNote *)(P.events + P.chart->nEvents);
	strum_setup(&P.sl[0], notes, P.chart->nNotes[0], false);
	strum_setup(&P.sl[1], notes + P.chart->nNotes[0], P.chart->nNotes[1], true);
	// Blazin': the opponent strumline is hidden and the player one centered (songs/blazin.hxc)
	if (!strcmp(P.info->id, "blazin") || save.middlescroll) {
		P.hideOpp = true;
		for (int i = 0; i < 4; i++) P.sl[0].cx[i] = hx(640 + (i - 1.5f) * NOTE_SPACING);
	}
	P.scrollSpeed = P.chart->speed;
	P.health = 1;
	P.stageZoom = P.info->stage->zoom;
	P.currentZoom = P.stageZoom;
	P.bopMult = 1;
	P.bopIntensity = 1.015f;
	P.hudZoom = 1;
	P.hudBop = 0.03f;
	P.zoomRate = 4;
	P.followEnabled = true;
	P.iconScale[0] = P.iconScale[1] = 1;
	P.skin = P.info->pixel ? &noteskin_pixel : &noteskin_funkin;
	P.splashDef = P.info->pixel ? &splash_pixel : &splash_funkin;
	// characters (the opponent first: it is the biggest one most of the time)
	chr_slots_reset();
	const StageDef *st = P.info->stage;
	if (chr_load(&P.chr[ROLE_DAD], P.info->opp, ROLE_DAD)) P.chars[ROLE_DAD] = &P.chr[ROLE_DAD];
	if (chr_load(&P.chr[ROLE_BF], P.info->player, ROLE_BF)) P.chars[ROLE_BF] = &P.chr[ROLE_BF];
	if (P.info->gf[0] && chr_load(&P.chr[ROLE_GF], P.info->gf, ROLE_GF)) P.chars[ROLE_GF] = &P.chr[ROLE_GF];
	static const int stageIdx[3] = { 0, 1, 2 };   // StageDef chars: bf, gf, dad
	for (int r = 0; r < 3; r++) if (P.chars[r]) chr_place(P.chars[r], &st->chars[stageIdx[r]]);
	// Tutorial: Girlfriend is the opponent, placed where she normally stands
	if (!P.info->gf[0] && P.chars[ROLE_DAD]) chr_place(P.chars[ROLE_DAD], &st->chars[1]);
	// Nene stands on A-Bot (scripts/characters/nene.hxc setupAbot)
	Character *gfc = P.chars[ROLE_GF];
	if (gfc && !strncmp(gfc->id, "nene", 4) && strcmp(gfc->id, "nene-pixel")) {
		if (chr_load(&P.abot, strstr(gfc->id, "dark") ? "abot_dark" : "abot", ROLE_OTHER)) {
			P.abot.x = gfc->x - 95 + gfc->hdr->gx;
			P.abot.y = gfc->y + 384 + gfc->hdr->gy;
			P.abot.sfx = gfc->sfx;
			P.abot.sfy = gfc->sfy;
		}
	}
	stage_load(st);
	sheet_load(&P.shNotes, P.skin->sheet);
	sheet_load(&P.shSplash, P.splashDef->sheet);
	sheet_load(&P.shPopup, P.info->pixel ? &sheet_popup_pixel : &sheet_popup_funkin);
	bool dialogue = false;
	for (int i = 0; i < conversation_count; i++) if (!strcmp(conversations[i].song, P.info->tag)) dialogue = true;
	dialogue = dialogue && g_storyMode && dialogueSong != g_song;
	if (!dialogue) sheet_load(&P.shCountdown, P.info->pixel ? &sheet_countdown_pixel : &sheet_countdown_funkin);
	load_icon(&P.shIcon[0], P.info->iconP);
	load_icon(&P.shIcon[1], P.info->iconO);
	font_load(FONT_VCR8O);
	font_load(FONT_VCR8);
	font_load(FONT_VCR12O);
	font_load(FONT_VCR16O);
	// camera starts on girlfriend (resetCamera)
	Character *f = P.chars[ROLE_GF] ? P.chars[ROLE_GF] : P.chars[ROLE_BF];
	if (f) { P.followX = f->camFocusX; P.followY = f->camFocusY; }
	P.camX = P.followX;
	P.camY = P.followY;
	conductor_update();
	// countdown: the song starts 5 beats later
	P.songPos = -P.crochet * 5;
	P.countdownStep = -1;
	P.curStep = P.curBeat = -1000;
	conductor_update();
	P.cam.zoom = P.currentZoom;
	P.cam.camX = P.camX;
	P.cam.camY = P.camY;
	if (g_storyMode && dialogueSong != g_song) dlg_start();
	subDirty = true;
	g_fade = 0;
}

void play_leave(void) {
	audio_stop();
	for (int r = 0; r < 3; r++) chr_unload(&P.chr[r]);
	chr_unload(&P.abot);
	chr_slots_reset();
	stage_unload();
	sheet_unload(&P.shNotes);
	sheet_unload(&P.shSplash);
	sheet_unload(&P.shPopup);
	sheet_unload(&P.shCountdown);
	sheet_unload(&P.shIcon[0]);
	sheet_unload(&P.shIcon[1]);
	if (P.dlg) dlg_end();
	for (int i = 0; i < 2; i++) { free(P.sl[i].notes); P.sl[i].notes = NULL; }
	free(P.chartData);
	P.chartData = NULL;
}

// ---------------------------------------------------------------------------
// bottom screen
// ---------------------------------------------------------------------------
static const char *pauseItems[3] = { "Resume", "Restart Song", "Exit to menu" };

// The bottom screen is a display list: the static part once, then the
// fields whose value changed (an opaque fill replaces the old text). The
// song progress and health bars are drawn every frame (sub hook).
#define SUB_BG RGB(20, 12, 40)
#define SUB_PANEL RGB(0x2C, 0x1A, 0x52)
enum { SF_SCORE, SF_MISSES, SF_COMBO, SF_J0, SF_J1, SF_J2, SF_J3, SF_DEBUG, SF_COUNT };
static int subVal[SF_COUNT];
static int subField;

static int sub_value(int f) {
	switch (f) {
	case SF_SCORE: return P.score;
	case SF_MISSES: return P.misses * 100000 + P.judged[0] + P.judged[1] * 7 + P.judged[2] * 131 + P.judged[3] * 2003;
	case SF_COMBO: return P.combo * 10000 + P.maxCombo;
	case SF_DEBUG: return P.curBeat;
	default: return P.judged[f - SF_J0];
	}
}

static void sub_draw_field(int f) {
	char b[64], sc[24];
	static const char *jn[4] = { "Sick", "Good", "Bad", "Shit" };
	static const u32 jc[4] = { RGB(80, 220, 255), RGB(120, 255, 120), RGB(255, 200, 80), RGB(255, 100, 100) };
	switch (f) {
	case SF_SCORE:
		sub_fill(8, 54, 304, 30, SUB_PANEL);
		format_score(sc, P.score);
		snprintf(b, sizeof(b), "%s", sc);
		sub_text_centered(FONT_VCR24O, BW / 2, 54, b, RGB(0xF9, 0xCF, 0x51));
		break;
	case SF_MISSES: {
		sub_fill(8, 88, 304, 20, SUB_BG);
		int total = P.judged[0] + P.judged[1] + P.judged[2] + P.judged[3] + P.misses;
		float acc = total ? (P.judged[0] + P.judged[1] * 0.75f + P.judged[2] * 0.4f + P.judged[3] * 0.1f) / total * 100 : 100;
		snprintf(b, sizeof(b), "Misses: %d   Acc: %d.%02d%%", P.misses, (int)acc, (int)(acc * 100) % 100);
		sub_text_centered(FONT_VCR12O, BW / 2, 90, b, WHITE);
		break;
	}
	case SF_COMBO:
		sub_fill(8, 108, 304, 20, SUB_BG);
		snprintf(b, sizeof(b), "Combo: %d   (max %d)", P.combo, P.maxCombo);
		sub_text_centered(FONT_VCR12O, BW / 2, 110, b, P.combo >= 50 ? RGB(0x33, 0xFF, 0xFF) : WHITE);
		break;
	case SF_J0: case SF_J1: case SF_J2: case SF_J3: {
		int i = f - SF_J0;
		int x = 12 + (i & 1) * 152, y = 132 + (i >> 1) * 26;
		sub_fill(x, y, 144, 22, RGB(0x22, 0x14, 0x40));
		sub_fill(x, y, 4, 22, jc[i]);
		snprintf(b, sizeof(b), "%s", jn[i]);
		sub_text(FONT_VCR12O, x + 10, y + 3, b, jc[i]);
		snprintf(b, sizeof(b), "%d", P.judged[i]);
		sub_text_right(FONT_VCR12O, x + 138, y + 3, b, WHITE);
		break;
	}
	case SF_DEBUG:
#ifdef DEBUG_HUD
		sub_fill(0, 186, 320, 30, RGB(0, 0, 0));
		snprintf(b, sizeof(b), "u%d d%d g%d s%d lz%d f%d", (int)prof_show[PF_UPDATE], (int)prof_show[PF_DRAW], (int)prof_show[PF_GPU],
			(int)prof_show[PF_SUB], (int)prof_show[PF_DECOMP], (int)prof_show[PF_FRAME]);
		sub_text(FONT_VCR8, 2, 188, b, RGB(255, 255, 0));
		extern int perf_drops, perf_frames;
		snprintf(b, sizeof(b), "drops %d/%d lin %dK dsp %d len %d", perf_drops, perf_frames, (int)(linearSpaceFree() / 1024),
			audio_ok(), (int)(audio_song_length() / 1000));
		sub_text(FONT_VCR8, 2, 202, b, RGB(255, 255, 0));
#endif
		break;
	}
	subVal[f] = sub_value(f);
}

// every frame on the bottom screen: health bar with the icons, song progress
static void play_sub_hook(void) {
	if (P.paused || P.dlg) return;
	// health bar
	float x0 = 24, x1 = 296, y = 202, h = 10;
	gfx_rect(x0 - 2, y - 2, x1 + 2, y + h + 2, BLACK, 255);
	float split = x0 + (x1 - x0) * (1 - P.health / 2);
	gfx_rect(x0, y, split, y + h, RGB(255, 0, 0), 255);
	gfx_rect(split, y, x1, y + h, RGB(0x66, 0xFF, 0x33), 255);
	for (int i = 0; i < 2; i++) {
		Sheet *sh = &P.shIcon[i];
		if (!sh->loaded) continue;
		bool losing = i == 0 ? P.health < 0.4f : P.health > 1.6f;
		float k = 0.2f * P.iconScale[i];
		float sz = 150 * k;
		float cx = i == 0 ? split + sz / 2 - 6 : split - sz / 2 + 6;
		if (i == 0) sheet_draw_flip(sh, losing, cx + sz / 2, y + h / 2 - sz / 2, k, k, 1, WHITE, 255);
		else sheet_draw(sh, losing, cx - sz / 2, y + h / 2 - sz / 2, k, WHITE, 255);
	}
	// song progress (V-Slice time bar)
	float len = audio_song_length();
	float t = P.started ? P.songPos : 0;
	float k = len > 0 ? clampf(t / len, 0, 1) : 0;
	gfx_rect(0, 228, BW, 240, RGB(0x10, 0x08, 0x20), 255);
	gfx_gradient_h(0, 230, BW * k, 238, RGB(0x60, 0x30, 0xC0), RGB(0x33, 0xFF, 0xFF), 255);
	char b[32];
	int rem = len > 0 ? (int)((len - t) / 1000) : 0;
	if (rem < 0) rem = 0;
	snprintf(b, sizeof(b), "%d:%02d", rem / 60, rem % 60);
	font_draw_centered(FONT_VCR8O, BW / 2, 227, b, WHITE, 255);
}

static void draw_sub(void) {
	char b[64];
	sub_clear(SUB_BG);
	if (P.paused) {
		subStatic = false;
		sub_gradient(0, 0, BW, BH, RGB(40, 24, 72), RGB(10, 6, 20));
		sub_text_centered(FONT_VCR24O, BW / 2, 12, "PAUSED", WHITE);
		snprintf(b, sizeof(b), "%s (%s)", P.info->name, diff_name(g_diff));
		sub_text_centered(FONT_VCR12O, BW / 2, 48, b, RGB(200, 200, 220));
		for (int i = 0; i < 3; i++) {
			int y = 76 + i * 50;
			sub_button(40, y, 240, 40, i == pauseSel);
			sub_text_centered(FONT_VCR16O, BW / 2, y + 9, pauseItems[i], i == pauseSel ? RGB(40, 24, 10) : WHITE);
		}
		return;
	}
	sub_gradient(0, 0, BW, 44, RGB(0x4A, 0x2A, 0x86), RGB(0x2A, 0x18, 0x50));
	sub_text_centered(FONT_VCR16O, BW / 2, 4, P.info->name, WHITE);
	snprintf(b, sizeof(b), "%s - %s", P.info->artist, diff_name(g_diff));
	sub_text_centered(FONT_VCR8, BW / 2, 28, b, RGB(190, 190, 210));
	if (save.botplay) sub_text_centered(FONT_VCR12O, BW / 2, 180, "BOTPLAY", RGB(255, 255, 120));
#ifndef DEBUG_HUD
	else sub_text_centered(FONT_VCR8, BW / 2, 184, "START: pause", RGB(150, 140, 170));
#endif
	for (int f = 0; f < SF_COUNT; f++) sub_draw_field(f);
	sub_set_hook(play_sub_hook);
	subStatic = true;
}

// one field per frame, only when its value changed
static void sub_update(void) {
	if (!subStatic) { draw_sub(); return; }
	for (int k = 0; k < SF_COUNT; k++) {
		int f = (subField + k) % SF_COUNT;
		if (sub_value(f) != subVal[f]) {
			sub_draw_field(f);
			subField = (f + 1) % SF_COUNT;
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// pause
// ---------------------------------------------------------------------------
static void pause_select(void) {
	switch (pauseSel) {
	case 0:
		P.paused = false;
		if (P.started) audio_pause_song(false);
		subStatic = false;
		break;
	case 1: state_switch(ST_PLAY); break;
	case 2:
		state_switch(g_storyMode ? ST_STORY : ST_FREEPLAY);
		break;
	}
	subDirty = true;
}

static void update_pause(void) {
	int old = pauseSel;
	if (kDown & KEY_UP) pauseSel = (pauseSel + 2) % 3;
	if (kDown & KEY_DOWN) pauseSel = (pauseSel + 1) % 3;
	if (old != pauseSel) { audio_sfx(SFX_SCROLL, 0.5f); subDirty = true; }
	if (kDown & KEY_A) { pause_select(); return; }
	if (kDown & (KEY_START | KEY_B)) { pauseSel = 0; pause_select(); return; }
	if (kDown & KEY_TOUCH) {
		for (int i = 0; i < 3; i++)
			if (touch_in(40, 76 + i * 50, 240, 40)) { pauseSel = i; pause_select(); return; }
	}
}

// ---------------------------------------------------------------------------
// song end (PlayState.endSong)
// ---------------------------------------------------------------------------
static void add_results(void) {
	Results *r = &g_results;
	r->score += P.score;
	r->misses += P.misses;
	if (P.maxCombo > r->maxCombo) r->maxCombo = P.maxCombo;
	for (int i = 0; i < 4; i++) r->judged[i] += P.judged[i];
	r->notes += P.sl[0].n;
}

static void end_song(void) {
	P.ended = true;
	audio_stop_music();
	if (!g_storyMode || g_weekPos == 0) memset(&g_results, 0, sizeof(g_results));
	add_results();
	if (g_storyMode) {
		const WeekInfo *w = &week_info[g_week];
		g_weekPos++;
		const Cutscene *cs = cutscene_for(P.info->tag, true);
		g_cutsceneDone = false;
		if (g_weekPos < w->nSongs) {
			g_song = w->songs[g_weekPos];
			if (cs) { g_video = cs->video; g_videoNext = ST_PLAY; state_switch(ST_VIDEO); }
			else state_switch(ST_PLAY);
			return;
		}
		if (cs) { g_video = cs->video; g_videoNext = ST_RESULTS; }
		snprintf(g_results.title, sizeof(g_results.title), "%s", w->name);
		if (!save.botplay && g_results.score > save.weekScore[g_week][g_diff]) {
			save.weekScore[g_week][g_diff] = g_results.score;
			g_results.newBest = true;
		}
	} else {
		snprintf(g_results.title, sizeof(g_results.title), "%s", P.info->name);
		if (!save.botplay && P.score > save.songScore[g_song][g_diff]) {
			save.songScore[g_song][g_diff] = P.score;
			g_results.newBest = true;
		}
	}
	save_write();
	state_switch(g_storyMode && cutscene_for(P.info->tag, true) ? ST_VIDEO : ST_RESULTS);
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------
static void countdown_tick(int k) {
	static const int sfx[2][4] = { { SFX_INTROTHREE_FUNKIN, SFX_INTROTWO_FUNKIN, SFX_INTROONE_FUNKIN, SFX_INTROGO_FUNKIN },
		{ SFX_INTROTHREE_PIXEL, SFX_INTROTWO_PIXEL, SFX_INTROONE_PIXEL, SFX_INTROGO_PIXEL } };
	if (k < 0 || k > 3) return;
	audio_sfx(sfx[P.info->pixel][k], 0.6f);
	if (k >= 1) {
		P.countdownFrame = k - 1;
		P.countdownAlpha = 1;
		tween_cancel_target(&P.countdownAlpha);
		tween_to(&P.countdownAlpha, 0, P.crochet / 1000, EASE_CUBEINOUT, 0, NULL, NULL);
	}
}

void play_flash(u32 color, float dur) {
	if (!save.flashing) return;
	P.flashCol = color;
	P.flashA = 1;
	P.flashDur = dur;
}

void play_update(float dt) {
	if (P.dead || !P.chart) return;
	P.pauseA += ((P.paused ? 0.65f : 0) - P.pauseA) * fps_ratio(0.3f, dt);
	if (P.flashA > 0) P.flashA -= dt / P.flashDur;
	if (P.paused) {
		update_pause();
		if (subDirty) { draw_sub(); subDirty = false; }
		return;
	}
	if (P.dlg) {
		dlg_update(dt);
		for (int r = 0; r < 3; r++) if (P.chars[r]) chr_update(P.chars[r], dt, P.stepMs, false);
		stage_update(dt);
		return;
	}
	// conductor
	if (!P.started) {
		P.songPos += dt * 1000;
		int k = (int)floorf((P.songPos + P.crochet * 4) / P.crochet);
		if (k > P.countdownStep && P.songPos < 0) {
			for (int i = P.countdownStep + 1; i <= k; i++) countdown_tick(i);
			P.countdownStep = k;
		}
		if (P.songPos >= 0) {
			P.started = true;
			char a[48], b[48];
			snprintf(a, sizeof(a), "romfs:/snd/%s_inst.adp", P.info->tag);
			snprintf(b, sizeof(b), "romfs:/snd/%s_voices.adp", P.info->tag);
			audio_play_song(a, b);
			P.songPos = 0;
		}
	} else if (!P.ended) {
		float t = audio_song_time() - save.noteOffset;
		float predicted = P.songPos + dt * 1000;
		if (fabsf(t - predicted) > 30) P.songPos = t;
		else P.songPos = predicted + (t - predicted) * 0.1f;
		if (audio_song_finished()) { end_song(); return; }
		if (audio_song_length() <= 0) {
			// no audio: end after the last note
			float last = 0;
			for (int k = 0; k < 2; k++)
				if (P.sl[k].n) { Note *n = &P.sl[k].notes[P.sl[k].n - 1]; if (n->t + n->len > last) last = n->t + n->len; }
			if (P.songPos > last + 2000) { end_song(); return; }
		}
	}
	conductor_update();
	int step = (int)floorf(P.stepF), beat = (int)floorf(P.beatF);
	if (P.curStep == -1000) P.curStep = step - 1;
	if (step != P.curStep) {
		int from = step > P.curStep ? P.curStep + 1 : step;
		for (int s = from; s <= step; s++) {
			for (int r = 0; r < 3; r++) if (P.chars[r]) chr_step(P.chars[r], s);
			stage_step(s);
			// camera bop (PlayState.stepHit)
			int rate = (int)(P.zoomRate * 4 + 0.5f);
			if (save.camZoom && rate > 0 && P.hudZoom < 1.35f && (((s + (int)(P.zoomOffset * 4)) % rate) + rate) % rate == 0) {
				P.bopMult = P.bopIntensity;
				P.hudZoom += P.hudBop;
			}
		}
		P.curStep = step;
	}
	if (beat != P.curBeat) {
		int from = beat > P.curBeat && P.curBeat != -1000 ? P.curBeat + 1 : beat;
		for (int b = from; b <= beat; b++) {
			stage_beat(b);
			if (P.abot.loaded) chr_play(&P.abot, "idle", true);
			P.iconScale[0] = P.iconScale[1] = 1.2f;
		}
		P.curBeat = beat;
		subDirty = true;
	}
	// events
	while (P.evIdx < P.chart->nEvents && P.events[P.evIdx].t <= P.songPos) run_event(&P.events[P.evIdx++]);

	if ((kDown & KEY_START) || ((kDown & KEY_TOUCH) && touch.py < 180)) {
		P.paused = true;
		pauseSel = 0;
		audio_pause_song(true);
		draw_sub();
		subDirty = false;
		return;
	}

	// notes and input
	update_notes(1, dt);
	update_notes(0, dt);
	update_input();
	for (int i = 0; i < 2; i++) {
		Strum *s = &P.sl[i];
		for (int d = 0; d < 4; d++) {
			s->animTime[d] += dt;
			if (s->cpu && s->anim[d] == 2 && P.songPos > s->cpuRelease[d] && !s->holding[d]) s->anim[d] = 0;
		}
	}
	bool holding = false;
	for (int d = 0; d < 4; d++) holding |= (kHeld & laneKeys[d]) != 0 || (save.botplay && botHoldEnd[d] > P.songPos);
	for (int r = 0; r < 3; r++) if (P.chars[r]) chr_update(P.chars[r], dt, P.stepMs, holding);
	if (P.abot.loaded) chr_update(&P.abot, dt, P.stepMs, false);
	stage_update(dt);

	// camera (PlayState.update + FlxCamera follow)
	float decay = powf(0.95f, dt * 60);
	P.bopMult = lerpf(1, P.bopMult, decay);
	P.hudZoom = lerpf(1, P.hudZoom, decay);
	if (P.followEnabled) {
		float r = fps_ratio(0.04f, dt);
		P.camX += (P.followX - P.camX) * r;
		P.camY += (P.followY - P.camY) * r;
	}
	P.cam.camX = P.camX;
	P.cam.camY = P.camY;
	P.cam.zoom = P.currentZoom * P.bopMult;
	for (int i = 0; i < 2; i++) P.iconScale[i] = lerpf(1, P.iconScale[i], powf(0.85f, dt * 60));

	// popups
	for (int i = 0; i < 16; i++) {
		Popup *p = &P.pops[i];
		if (!p->used) continue;
		p->vy += p->ay * dt;
		p->x += p->vx * dt;
		p->y += p->vy * dt;
		if (p->fadeDelay > 0) p->fadeDelay -= dt;
		else if ((p->alpha -= dt / 0.2f) <= 0) p->used = false;
	}
	for (int i = 0; i < 6; i++) {
		if (!P.splash[i].used) continue;
		P.splash[i].t += dt;
		if (P.splash[i].t * 24 >= P.splashDef->seq[P.splash[i].lane][0][1]) P.splash[i].used = false;
	}

	P.health = clampf(P.health, 0, 2);
	if (P.health <= 0 && !save.botplay) {
		P.dead = true;
		audio_stop();
		state_switch(ST_GAMEOVER);
		return;
	}
	{
		u32 t0 = time_ticks();
		if (subDirty && !subStatic) draw_sub();
		else sub_update();
		subDirty = false;
		prof_add(PF_SUBDRAW, t0);
	}
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------
#define HUD_DEPTH -1.5f

static void draw_strum(int idx) {
	Strum *s = &P.sl[idx];
	const NoteSkin *sk = P.skin;
	Sheet *sh = &P.shNotes;
	if (!sh->loaded) return;
	float hz = P.hudZoom;
	float ks = KH * s->scale * hz;
	bool down = save.downscroll;
	float sy0 = strum_y();
	#define HX(v) (((v) - 200) * hz + 200)
	#define HY(v) (((v) - 120) * hz + 120)
	for (int d = 0; d < 4; d++) {
		int fr = sk->fStatic[d];
		int f = (int)(s->animTime[d] * 24);
		if (s->anim[d] == 2) fr = sk->confirm[d][f < sk->nConfirm[d] ? f : sk->nConfirm[d] - 1];
		else if (s->anim[d] == 1) fr = sk->press[d][f < sk->nPress[d] ? f : sk->nPress[d] - 1];
		sheet_draw(sh, fr, HX(s->cx[d]), HY(sy0), ks, WHITE, 255);
	}
	float pxPerMs = 0.45f * P.scrollSpeed * KH * s->scale;
	float pos = P.songPos;
	float dir = down ? -1 : 1;
	for (int i = s->first; i < s->n; i++) {
		Note *n = &s->notes[i];
		float dy = (n->t - pos) * pxPerMs;
		if (dy > 280) break;
		float x = s->cx[n->dir];
		// hold trail
		if (n->len > 0 && n->state != NS_DONE) {
			float t0 = n->state == NS_HELD ? n->holdEnd : n->t;
			float y0 = sy0 + dir * (t0 - pos) * pxPerMs;
			float y1 = sy0 + dir * (n->t + n->len - pos) * pxPerMs;
			float w = sk->holdW * KH * s->scale;
			float eh = sk->endH * KH * s->scale;
			int alpha = n->state == NS_MISSED ? 90 : 255;
			if (!down) {
				float ye = y1 - eh;
				if (ye < y0) ye = y0;
				if (ye > y0) sheet_draw_rect(sh, sk->fHold[n->dir], HX(x - w / 2), HY(y0), HX(x + w / 2), HY(ye), 0, 1, WHITE, alpha);
				float v0 = (ye - (y1 - eh)) / eh;
				sheet_draw_rect(sh, sk->fEnd[n->dir], HX(x - w / 2), HY(ye), HX(x + w / 2), HY(y1), v0, 1, WHITE, alpha);
			} else {
				float ye = y1 + eh;
				if (ye > y0) ye = y0;
				if (ye < y0) sheet_draw_rect(sh, sk->fHold[n->dir], HX(x - w / 2), HY(ye), HX(x + w / 2), HY(y0), 0, 1, WHITE, alpha);
				float v0 = ((y1 + eh) - ye) / eh;
				sheet_draw_sub(sh, sk->fEnd[n->dir], HX(x - w / 2), HY(y1), HX(x + w / 2), HY(ye), 0, 1, 1, v0, WHITE, alpha);
			}
		}
		if (n->state != NS_PENDING) continue;
		float ny = sy0 + dir * dy;
		if (ny < -50 || ny > 290) continue;
		sheet_draw(sh, sk->fNote[n->dir], HX(x), HY(ny), ks, WHITE, 255);
	}
	// splashes
	if (!s->cpu && P.shSplash.loaded) {
		for (int i = 0; i < 6; i++) {
			if (!P.splash[i].used) continue;
			const u16 *sq = P.splashDef->seq[P.splash[i].lane][0];
			int f = (int)(P.splash[i].t * 24);
			if (f >= sq[1]) continue;
			sheet_draw(&P.shSplash, sq[0] + f, HX(s->cx[P.splash[i].lane]), HY(sy0), ks, WHITE, AL(P.splashDef->alpha));
		}
	}
	#undef HX
	#undef HY
}

static void draw_healthbar(void) {
	// PlayState.healthBar: y = FlxG.height * 0.9 (downscroll: 72)
	float hz = P.hudZoom;
	#define HX(v) (((v) - 200) * hz + 200)
	#define HY(v) (((v) - 120) * hz + 120)
	float y = save.downscroll ? hy(72) : hy(648);
	float w = 601 * KH, h = 19 * KH;
	float x0 = 200 - w / 2;
	gfx_rect(HX(x0), HY(y), HX(x0 + w), HY(y + h), BLACK, 255);
	float b = 4 * KH;
	float split = x0 + b + (w - 2 * b) * (1 - P.health / 2);
	gfx_rect(HX(x0 + b), HY(y + b), HX(split), HY(y + h - b), RGB(255, 0, 0), 255);
	gfx_rect(HX(split), HY(y + b), HX(x0 + w - b), HY(y + h - b), RGB(0x66, 0xFF, 0x33), 255);
	// score text (PlayState.scoreText: under the health bar, right aligned)
	{
		char t[64], sc[24];
		format_score(sc, P.score);
		if (save.botplay) snprintf(t, sizeof(t), "Bot Play Enabled");
		else snprintf(t, sizeof(t), "Score: %s", sc);
		font_draw(FONT_VCR8O, HX(x0 + w - font_width(FONT_VCR8O, t) + 4), HY(y + h + 5), t, WHITE, 255);
	}
	// icons (HealthIcon: 150 px, centered around the split point)
	float isz = 150 * KH;
	for (int i = 0; i < 2; i++) {
		Sheet *sh = &P.shIcon[i];
		if (!sh->loaded) continue;
		bool losing = i == 0 ? P.health < 0.4f : P.health > 1.6f;
		float sc = P.iconScale[i];
		float cx = i == 0 ? split + isz / 2 - 26 * KH : split - isz / 2 + 26 * KH;
		float cy = y + h / 2;
		float k = KH * sc * hz;
		float hw = isz * sc / 2;
		if (i == 0) sheet_draw_flip(sh, losing, HX(cx + hw), HY(cy - hw), k, k, 1, WHITE, 255);
		else sheet_draw(sh, losing, HX(cx - hw), HY(cy - hw), k, WHITE, 255);
	}
	#undef HX
	#undef HY
}

void play_draw(void) {
	if (!P.chart) return;
	stage_draw(&P.cam);
	if (P.flashA > 0.01f) gfx_rect(-16, 0, SW + 16, SH, P.flashCol, AL(P.flashA > 0.96f ? 0.96f : P.flashA));
	gfx_depth(HUD_DEPTH);
	// popups (rating / combo)
	if (P.shPopup.loaded) {
		for (int i = 0; i < 16; i++) {
			Popup *p = &P.pops[i];
			if (!p->used) continue;
			sheet_draw(&P.shPopup, p->frame, hx(p->x), hy(p->y), KH, WHITE, AL(p->alpha));
		}
	}
	draw_healthbar();
	if (!P.hideOpp) draw_strum(1);
	draw_strum(0);
	if (P.countdownAlpha > 0.01f && P.shCountdown.loaded)
		sheet_draw(&P.shCountdown, P.countdownFrame, 200, 120, KH * 0.9f, WHITE, AL(P.countdownAlpha));
	gfx_depth(0);
	if (P.dlg) dlg_draw();
	if (P.pauseA > 0.01f) gfx_rect(-16, 0, SW + 16, SH, BLACK, AL(P.pauseA));
}
