// Audio: one maxmod stream fed from NitroFS. Two IMA ADPCM streamed tracks
// (music/inst, voices) are decoded and mixed in software, with pitch
// (resampling) support. Short sound effects come from a soundbank.
#include "engine.h"
#include <maxmod9.h>
#include "soundbank_bin.h"

#define RING 16384              // samples per track ring buffer
#define RING_MASK (RING - 1)
#define BUFLEN 1024             // maxmod stream buffer (samples)
#define TICK_HZ ((float)TICK_FREQ)

typedef struct {
	FILE *f;
	s16 *ring;
	volatile u32 w, r;          // ring positions (samples)
	volatile u32 frac;          // 16.16 sub-sample position
	volatile u32 step;          // 16.16 increment (pitch)
	volatile int vol;           // 0..256
	volatile bool active, paused;
	bool loop, eof, open;
	volatile u32 consumed;      // samples consumed since start (for timing)
} Track;

static Track tr[TR_COUNT];
static volatile bool songMode;
static volatile u32 cbTicks;
static float musicVol = 1, voicesVol = 1;
static float fadeFrom, fadeTo, fadeDur, fadeT;
static bool fading;

static inline u32 ticks_now(void) { return (u32)tickGetCount(); }
u32 time_ticks(void) { return ticks_now(); }

static mm_word stream_cb(mm_word length, mm_addr dest, mm_stream_formats format) {
	s16 *out = dest;
	static s32 mix[BUFLEN * 2];
	for (u32 i = 0; i < length; i++) mix[i] = 0;
	bool any = false;
	for (int k = 0; k < TR_COUNT; k++) {
		Track *t = &tr[k];
		if (!t->active || t->paused) continue;
		any = true;
		int v = t->vol;
		u32 step = t->step;
		if (step == 0x10000) {
			u32 avail = t->w - t->r;
			u32 n = length < avail ? length : avail;
			for (u32 i = 0; i < n; i++) mix[i] += (t->ring[(t->r + i) & RING_MASK] * v) >> 8;
			t->r += n;
			t->consumed += n;
		} else {
			u32 frac = t->frac;
			u32 r = t->r;
			u32 i;
			for (i = 0; i < length; i++) {
				if (t->w - r < 2) break;
				s32 a = t->ring[r & RING_MASK], b = t->ring[(r + 1) & RING_MASK];
				s32 s = a + (((b - a) * (s32)(frac >> 4)) >> 12);
				mix[i] += (s * v) >> 8;
				frac += step;
				r += frac >> 16;
				t->consumed += frac >> 16;
				frac &= 0xFFFF;
			}
			t->r = r;
			t->frac = frac;
		}
	}
	for (u32 i = 0; i < length; i++) {
		s32 s = mix[i];
		out[i] = s > 32767 ? 32767 : (s < -32768 ? -32768 : s);
	}
	(void)any;
	if (songMode) cbTicks = ticks_now();
	return length;
}

static void track_close(Track *t) {
	t->active = false;
	if (t->f) fclose(t->f);
	t->f = NULL;
	t->open = false;
	t->w = t->r = 0;
	t->frac = 0;
	t->eof = false;
	t->paused = false;
	t->consumed = 0;
}

static bool track_open(Track *t, const char *path, bool loop, float vol, float pitch) {
	track_close(t);
	t->f = fopen(path, "rb");
	if (!t->f) return false;
	setvbuf(t->f, NULL, _IONBF, 0);
	t->loop = loop;
	t->vol = (int)(clampf(vol, 0, 4) * 256);
	t->step = (u32)(pitch * 65536);
	t->open = true;
	return true;
}

// IMA ADPCM blocks (tools/adpcm.c): s16 predictor, u8 index, u8 0, 1024 bytes
#define ADP_SAMPLES 2048
#define ADP_BLOCK (4 + ADP_SAMPLES / 2)
static const s16 adpSteps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
	1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
	7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const s8 adpIdx[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static void adpcm_block(const u8 *b, s16 *out) {
	int pred = (s16)(b[0] | (b[1] << 8));
	int idx = b[2];
	if (idx > 88) idx = 88;
	const u8 *d = b + 4;
	for (int i = 0; i < ADP_SAMPLES; i++) {
		int code = (d[i >> 1] >> ((i & 1) * 4)) & 15;
		int step = adpSteps[idx];
		int delta = step >> 3;
		if (code & 4) delta += step;
		if (code & 2) delta += step >> 1;
		if (code & 1) delta += step >> 2;
		pred += (code & 8) ? -delta : delta;
		if (pred > 32767) pred = 32767;
		if (pred < -32768) pred = -32768;
		idx += adpIdx[code];
		if (idx < 0) idx = 0;
		if (idx > 88) idx = 88;
		out[i] = pred;
	}
}

static void track_fill(Track *t, int maxChunk) {
	if (!t->open || t->eof) return;
	static u8 blk[ADP_BLOCK];
	static s16 pcm[ADP_SAMPLES];
	while (maxChunk-- > 0) {
		u32 space = RING - (t->w - t->r) - 4;
		if (space < ADP_SAMPLES) break;
		size_t got = fread(blk, 1, ADP_BLOCK, t->f);
		if (got < ADP_BLOCK) {
			if (t->loop && got == 0) { fseek(t->f, 0, SEEK_SET); continue; }
			if (got < 4) { t->eof = true; break; }
			memset(blk + got, 0, ADP_BLOCK - got);
			if (!t->loop) t->eof = true;
			else fseek(t->f, 0, SEEK_SET);
		}
		adpcm_block(blk, pcm);
		u32 n = got >= ADP_BLOCK ? ADP_SAMPLES : (got - 4) * 2;
		for (u32 i = 0; i < n; i++) t->ring[(t->w + i) & RING_MASK] = pcm[i];
		t->w += n;
		if (t->eof) break;
	}
}

void audio_init(void) {
	mmInitDefaultMem((mm_addr)soundbank_bin);
	for (int i = 0; i < MSL_NSAMPS; i++) mmLoadEffect(i);
	for (int i = 0; i < TR_COUNT; i++) {
		tr[i].ring = malloc(RING * 2);
		tr[i].step = 0x10000;
	}
	static mm_stream st;
	st.sampling_rate = AUDIO_RATE;
	st.buffer_length = BUFLEN;
	st.callback = stream_cb;
	st.format = MM_STREAM_16BIT_MONO;
	st.timer = MM_TIMER0;
	st.manual = false;
	mmStreamOpen(&st);
}

static bool track_done(Track *t) { return t->open && t->eof && t->r + 1 >= t->w; }

void audio_update(void) {
	for (int i = 0; i < TR_COUNT; i++) {
		Track *t = &tr[i];
		if (!t->open) continue;
		track_fill(t, 1);
	}
	if (fading) {
		static u32 last;
		u32 now = ticks_now();
		float dt = last ? (now - last) / TICK_HZ : 0;
		last = now;
		fadeT += dt;
		float k = fadeDur > 0 ? clampf(fadeT / fadeDur, 0, 1) : 1;
		audio_set_volume(lerpf(fadeFrom, fadeTo, k));
		if (k >= 1) { fading = false; last = 0; }
	}
}

void audio_set_volume(float v) {
	musicVol = clampf(v, 0, 2);
	tr[TR_MUSIC].vol = (int)(musicVol * 256);
	tr[TR_VOICES].vol = songMode ? (int)(musicVol * voicesVol * 256) : tr[TR_VOICES].vol;
}

float audio_get_volume(void) { return musicVol; }

void audio_fade_music(float to, float dur) {
	fadeFrom = musicVol;
	fadeTo = to;
	fadeDur = dur;
	fadeT = 0;
	fading = true;
}

void audio_set_voices(float v) {
	voicesVol = clampf(v, 0, 1);
	tr[TR_VOICES].vol = (int)(musicVol * voicesVol * 256);
}

void audio_set_pitch(float p) {
	tr[TR_MUSIC].step = (u32)(clampf(p, 0.1f, 4) * 65536);
}

void audio_stop_music(void) {
	fading = false;
	songMode = false;
	track_close(&tr[TR_MUSIC]);
	track_close(&tr[TR_VOICES]);
}

void audio_stop(void) {
	audio_stop_music();
}

void audio_play_music(const char *path, bool loop, float volume) {
	audio_stop_music();
	musicVol = volume;
	if (!track_open(&tr[TR_MUSIC], path, loop, volume, 1)) return;
	track_fill(&tr[TR_MUSIC], 8);
	tr[TR_MUSIC].active = true;
}

bool audio_music_playing(void) { return tr[TR_MUSIC].open && !track_done(&tr[TR_MUSIC]); }

void audio_play_song(const char *inst, const char *voices) {
	audio_stop_music();
	musicVol = 1;
	voicesVol = 1;
	if (!track_open(&tr[TR_MUSIC], inst, false, 1, 1)) return;
	track_fill(&tr[TR_MUSIC], 8);
	if (voices && track_open(&tr[TR_VOICES], voices, false, 1, 1)) track_fill(&tr[TR_VOICES], 8);
	cbTicks = ticks_now();
	songMode = true;
	// start both at the same sample
	int old = enterCriticalSection();
	tr[TR_MUSIC].active = true;
	if (tr[TR_VOICES].open) tr[TR_VOICES].active = true;
	leaveCriticalSection(old);
}

void audio_pause_song(bool paused) {
	int old = enterCriticalSection();
	tr[TR_MUSIC].paused = paused;
	tr[TR_VOICES].paused = paused;
	leaveCriticalSection(old);
	if (!paused) cbTicks = ticks_now();
}

bool audio_song_finished(void) { return track_done(&tr[TR_MUSIC]); }

float audio_song_time(void) {
	u32 s = tr[TR_MUSIC].consumed;
	float base = ((float)s - BUFLEN) * 1000.0f / AUDIO_RATE;
	if (!songMode || tr[TR_MUSIC].paused) return base;
	u32 dt = ticks_now() - cbTicks;
	float extra = dt * (1000.0f / TICK_HZ);
	float chunk = BUFLEN * 500.0f / AUDIO_RATE;   // callbacks happen every half buffer
	if (extra > chunk * 2) extra = chunk * 2;
	return base + extra;
}

float audio_music_time(void) {
	return (float)tr[TR_MUSIC].consumed * 1000.0f / AUDIO_RATE;
}

void audio_sfx_pitch(int id, float v, float pitch) {
	mm_sound_effect e;
	e.id = id;
	e.rate = (mm_hword)(clampf(pitch, 0.1f, 4) * 1024);
	e.handle = 0;
	e.volume = (mm_byte)(clampf(v, 0, 1) * 255);
	e.panning = 128;
	mmEffectEx(&e);
}

void audio_sfx(int id, float v) { audio_sfx_pitch(id, v, 1); }


void audio_debug(char *buf, int n) {
	Track *v = &tr[TR_VOICES], *m = &tr[TR_MUSIC];
	snprintf(buf, n, "inst %d/%d voc o%d a%d v%d fill %d c%d", (int)(m->w - m->r), (int)m->consumed / 1000,
		v->open, v->active, v->vol, (int)(v->w - v->r), (int)v->consumed / 1000);
}
