// Audio (NDSP): two streamed IMA ADPCM tracks (music / instrumental and the
// voices, mono or stereo) decoded by a high priority thread woken by the DSP,
// plus the sound effects (PCM16 WAVs kept in memory).
//
// .adp file (tools/adpcm.c): "IMA2", u32 rate, u16 channels, u16 0,
// u32 samples, then blocks of 2048 samples: per channel s16 predictor,
// u8 index, u8 0, 1024 bytes of nibbles.
#include "engine.h"

#define ADP_SAMPLES 2048
#define ADP_BLOCK (4 + ADP_SAMPLES / 2)
#define BUF_FRAMES (ADP_SAMPLES * 2)
#define NBUF 4
#define CH_MUSIC 0
#define CH_VOICES 1
#define CH_SFX0 4
#define CH_SFX_N 20

typedef struct {
	FILE *f;
	int ch, channels;
	u32 rate, total;
	ndspWaveBuf wb[NBUF];
	s16 *pcm[NBUF];
	bool counted[NBUF];
	int next;
	bool open, loop, eof, paused;
	u64 played;             // frames of the completed buffers
	u32 dec;                // frames decoded since the start of the file
	float vol, pitch;
	u8 *blk;
} Track;

static Track tr[TR_COUNT];
static bool ok;
static Thread thr;
static LightEvent ev;
static LightLock lock;
static volatile bool quit;
static float musicVol = 1, voicesVol = 1;
static float fadeFrom, fadeTo, fadeDur, fadeT;
static bool fading;
static u32 fadeLast;
// fallback clock when the DSP is not available
static u32 fakeStart, fakePausedAt;
static bool fakePaused;

bool audio_ok(void) { return ok; }

// ---------------------------------------------------------------------------
// IMA ADPCM
// ---------------------------------------------------------------------------
static const s16 adpSteps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
	1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
	7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const s8 adpIdx[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

// decodes one block into out[i * stride]
static void adpcm_block(const u8 *b, s16 *out, int stride) {
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
		out[i * stride] = pred;
	}
}

// fills one wave buffer, returns the number of frames (0 at the end)
static int decode(Track *t, s16 *out) {
	int frames = 0;
	int bsz = ADP_BLOCK * t->channels;
	int restarts = 0;
	while (frames + ADP_SAMPLES <= BUF_FRAMES) {
		size_t got = fread(t->blk, 1, bsz, t->f);
		if (got < (size_t)(4 * t->channels)) {
			if (t->loop && restarts++ < 2 && t->dec > 0) { fseek(t->f, 16, SEEK_SET); t->dec = 0; continue; }
			t->eof = true;
			break;
		}
		if (got < (size_t)bsz) memset(t->blk + got, 0, bsz - got);
		int n = ADP_SAMPLES;
		if (t->total && t->dec + n > t->total) n = t->total > t->dec ? (int)(t->total - t->dec) : 0;
		if (n > 0) {
			if (n == ADP_SAMPLES) {
				for (int c = 0; c < t->channels; c++) adpcm_block(t->blk + c * ADP_BLOCK, out + frames * t->channels + c, t->channels);
			} else {
				static s16 tmp[ADP_SAMPLES * 2];
				for (int c = 0; c < t->channels; c++) adpcm_block(t->blk + c * ADP_BLOCK, tmp + c, t->channels);
				memcpy(out + frames * t->channels, tmp, n * 2 * t->channels);
			}
		}
		frames += n;
		t->dec += n;
		if (n < ADP_SAMPLES || got < (size_t)bsz) {
			// end of the stream
			if (t->loop) { fseek(t->f, 16, SEEK_SET); t->dec = 0; }
			else { t->eof = true; break; }
		}
	}
	return frames;
}

static void refill(Track *t) {
	if (!t->open) return;
	for (int k = 0; k < NBUF; k++) {
		ndspWaveBuf *w = &t->wb[t->next];
		if (w->status != NDSP_WBUF_FREE && w->status != NDSP_WBUF_DONE) break;
		if (w->status == NDSP_WBUF_DONE && !t->counted[t->next]) {
			t->played += w->nsamples;
			t->counted[t->next] = true;
		}
		if (t->eof) break;
		int n = decode(t, t->pcm[t->next]);
		if (n <= 0) break;
		w->data_vaddr = t->pcm[t->next];
		w->nsamples = n;
		w->looping = false;
		DSP_FlushDataCache(t->pcm[t->next], n * 2 * t->channels);
		t->counted[t->next] = false;
		ndspChnWaveBufAdd(t->ch, w);
		t->next = (t->next + 1) % NBUF;
	}
}

static void thread_main(void *arg) {
	while (!quit) {
		LightEvent_Wait(&ev);
		if (quit) break;
		LightLock_Lock(&lock);
		for (int k = 0; k < TR_COUNT; k++) refill(&tr[k]);
		LightLock_Unlock(&lock);
	}
}

static void dsp_cb(void *arg) { LightEvent_Signal(&ev); }

// ---------------------------------------------------------------------------
// tracks
// ---------------------------------------------------------------------------
static void apply_mix(Track *t) {
	float mix[12] = { 0 };
	mix[0] = mix[1] = clampf(t->vol, 0, 2);
	ndspChnSetMix(t->ch, mix);
}

static void track_close(Track *t) {
	if (ok) ndspChnWaveBufClear(t->ch);
	if (t->f) fclose(t->f);
	t->f = NULL;
	t->open = false;
	t->eof = false;
	t->paused = false;
	t->played = 0;
	t->dec = 0;
	t->next = 0;
	t->total = 0;
	for (int i = 0; i < NBUF; i++) {
		memset(&t->wb[i], 0, sizeof(ndspWaveBuf));
		t->wb[i].status = NDSP_WBUF_FREE;
		t->counted[i] = true;
	}
}

static bool track_open(Track *t, const char *path, bool loop, float vol) {
	track_close(t);
	if (!ok) return false;
	t->f = fopen(path, "rb");
	if (!t->f) return false;
	u8 h[16];
	if (fread(h, 1, 16, t->f) != 16 || memcmp(h, "IMA2", 4)) { fclose(t->f); t->f = NULL; return false; }
	t->rate = h[4] | (h[5] << 8) | (h[6] << 16) | ((u32)h[7] << 24);
	t->channels = h[8] == 2 ? 2 : 1;
	t->total = h[12] | (h[13] << 8) | (h[14] << 16) | ((u32)h[15] << 24);
	t->loop = loop;
	t->vol = vol;
	t->pitch = 1;
	t->open = true;
	ndspChnReset(t->ch);
	ndspChnSetInterp(t->ch, NDSP_INTERP_POLYPHASE);
	ndspChnSetRate(t->ch, t->rate);
	ndspChnSetFormat(t->ch, t->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	apply_mix(t);
	return true;
}

static bool track_done(Track *t) {
	if (!t->open || !t->eof) return false;
	for (int i = 0; i < NBUF; i++)
		if (t->wb[i].status == NDSP_WBUF_QUEUED || t->wb[i].status == NDSP_WBUF_PLAYING) return false;
	return true;
}

// frames played: completed buffers + the position in the playing one
static u64 track_pos(Track *t) {
	u64 p = t->played;
	for (int i = 0; i < NBUF; i++)
		if (t->wb[i].status == NDSP_WBUF_DONE && !t->counted[i]) p += t->wb[i].nsamples;
	if (ndspChnIsPlaying(t->ch)) p += ndspChnGetSamplePos(t->ch);
	return p;
}

// ---------------------------------------------------------------------------
// sound effects
// ---------------------------------------------------------------------------
typedef struct { s16 *data; u32 frames, rate; int channels; } Sfx;
static Sfx sfx[SFX_COUNT];
static ndspWaveBuf sfxWb[CH_SFX_N];
static int sfxNext;

static void sfx_load(int id, const char *path) {
	u32 size;
	u8 *b = file_load(path, &size);
	if (!b) return;
	u32 p = 12, rate = 32000;
	int ch = 1;
	while (p + 8 <= size) {
		u32 len = b[p + 4] | (b[p + 5] << 8) | (b[p + 6] << 16) | ((u32)b[p + 7] << 24);
		if (!memcmp(b + p, "fmt ", 4)) {
			ch = b[p + 10];
			rate = b[p + 12] | (b[p + 13] << 8) | (b[p + 14] << 16);
		} else if (!memcmp(b + p, "data", 4)) {
			if (len > size - p - 8) len = size - p - 8;
			sfx[id].data = linearAlloc(len);
			if (sfx[id].data) {
				memcpy(sfx[id].data, b + p + 8, len);
				DSP_FlushDataCache(sfx[id].data, len);
				sfx[id].frames = len / 2 / ch;
				sfx[id].rate = rate;
				sfx[id].channels = ch;
			}
			break;
		}
		p += 8 + len + (len & 1);
	}
	free(b);
}

void audio_sfx_pitch(int id, float v, float pitch) {
	if (!ok || id < 0 || id >= SFX_COUNT || !sfx[id].data) return;
	int ch = CH_SFX0 + sfxNext;
	ndspWaveBuf *w = &sfxWb[sfxNext];
	sfxNext = (sfxNext + 1) % CH_SFX_N;
	ndspChnWaveBufClear(ch);
	ndspChnReset(ch);
	ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
	ndspChnSetRate(ch, sfx[id].rate * clampf(pitch, 0.1f, 4));
	ndspChnSetFormat(ch, sfx[id].channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	float mix[12] = { 0 };
	mix[0] = mix[1] = clampf(v, 0, 1);
	ndspChnSetMix(ch, mix);
	memset(w, 0, sizeof(*w));
	w->data_vaddr = sfx[id].data;
	w->nsamples = sfx[id].frames;
	ndspChnWaveBufAdd(ch, w);
}

void audio_sfx(int id, float v) { audio_sfx_pitch(id, v, 1); }

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
void audio_init(void) {
	for (int i = 0; i < TR_COUNT; i++) {
		tr[i].ch = i == TR_MUSIC ? CH_MUSIC : CH_VOICES;
		tr[i].blk = malloc(ADP_BLOCK * 2);
		track_close(&tr[i]);
	}
	fakeStart = time_ticks();
	if (R_FAILED(ndspInit())) return;    // no DSP firmware dump: silent
	ok = true;
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	for (int i = 0; i < TR_COUNT; i++)
		for (int k = 0; k < NBUF; k++) tr[i].pcm[k] = linearAlloc(BUF_FRAMES * 4);
	for (int i = 0; i < SFX_COUNT; i++) sfx_load(i, sfx_files[i]);
	LightLock_Init(&lock);
	LightEvent_Init(&ev, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	thr = threadCreate(thread_main, NULL, 32 * 1024, prio - 1, -2, false);
	ndspSetCallback(dsp_cb, NULL);
}

void audio_exit(void) {
	if (!ok) return;
	quit = true;
	LightEvent_Signal(&ev);
	if (thr) { threadJoin(thr, U64_MAX); threadFree(thr); }
	for (int i = 0; i < TR_COUNT; i++) track_close(&tr[i]);
	ndspExit();
	ok = false;
}

void audio_update(void) {
	if (fading) {
		u32 now = time_ticks();
		float dt = fadeLast ? (now - fadeLast) / TICK_FREQ : 0;
		fadeLast = now;
		fadeT += dt;
		float k = fadeDur > 0 ? clampf(fadeT / fadeDur, 0, 1) : 1;
		audio_set_volume(lerpf(fadeFrom, fadeTo, k));
		if (k >= 1) { fading = false; fadeLast = 0; }
	}
}

// ---------------------------------------------------------------------------
// control
// ---------------------------------------------------------------------------
static void set_vols(void) {
	tr[TR_MUSIC].vol = musicVol;
	tr[TR_VOICES].vol = musicVol * voicesVol;
	if (!ok) return;
	if (tr[TR_MUSIC].open) apply_mix(&tr[TR_MUSIC]);
	if (tr[TR_VOICES].open) apply_mix(&tr[TR_VOICES]);
}

void audio_set_volume(float v) { musicVol = clampf(v, 0, 2); set_vols(); }
float audio_get_volume(void) { return musicVol; }
void audio_set_voices(float v) { voicesVol = clampf(v, 0, 1); set_vols(); }

void audio_fade_music(float to, float dur) {
	fadeFrom = musicVol;
	fadeTo = to;
	fadeDur = dur;
	fadeT = 0;
	fadeLast = 0;
	fading = true;
}

void audio_set_pitch(float p) {
	Track *t = &tr[TR_MUSIC];
	t->pitch = clampf(p, 0.1f, 4);
	if (ok && t->open) ndspChnSetRate(t->ch, t->rate * t->pitch);
}

void audio_stop_music(void) {
	fading = false;
	if (ok) LightLock_Lock(&lock);
	track_close(&tr[TR_MUSIC]);
	track_close(&tr[TR_VOICES]);
	if (ok) LightLock_Unlock(&lock);
}

void audio_stop(void) { audio_stop_music(); }

void audio_play_music(const char *path, bool loop, float volume) {
	audio_stop_music();
	musicVol = volume;
	fakeStart = time_ticks();
	fakePaused = false;
	if (!ok) return;
	LightLock_Lock(&lock);
	if (track_open(&tr[TR_MUSIC], path, loop, volume)) refill(&tr[TR_MUSIC]);
	LightLock_Unlock(&lock);
}

bool audio_music_playing(void) {
	if (!ok) return false;
	return tr[TR_MUSIC].open && !track_done(&tr[TR_MUSIC]);
}

void audio_play_song(const char *inst, const char *voices) {
	audio_stop_music();
	musicVol = 1;
	voicesVol = 1;
	fakeStart = time_ticks();
	fakePaused = false;
	if (!ok) return;
	LightLock_Lock(&lock);
	Track *m = &tr[TR_MUSIC], *v = &tr[TR_VOICES];
	if (track_open(m, inst, false, 1)) {
		ndspChnSetPaused(m->ch, true);
		refill(m);
		if (voices && track_open(v, voices, false, 1)) {
			ndspChnSetPaused(v->ch, true);
			refill(v);
		}
		// start both at the same time
		ndspChnSetPaused(m->ch, false);
		if (v->open) ndspChnSetPaused(v->ch, false);
	}
	LightLock_Unlock(&lock);
}

void audio_pause_song(bool paused) {
	if (!ok) {
		if (paused && !fakePaused) fakePausedAt = time_ticks();
		if (!paused && fakePaused) fakeStart += time_ticks() - fakePausedAt;
		fakePaused = paused;
		return;
	}
	for (int k = 0; k < TR_COUNT; k++) {
		tr[k].paused = paused;
		if (tr[k].open) ndspChnSetPaused(tr[k].ch, paused);
	}
}

// without audio (no DSP firmware, missing file) the song runs on the clock
// and PlayState ends it after the last note
bool audio_song_finished(void) {
	if (!ok || !tr[TR_MUSIC].open) return false;
	return track_done(&tr[TR_MUSIC]);
}

static float fake_time(void) {
	u32 now = fakePaused ? fakePausedAt : time_ticks();
	return (now - fakeStart) * 1000.0f / TICK_FREQ;
}

float audio_song_time(void) {
	Track *t = &tr[TR_MUSIC];
	if (!ok || !t->open || !t->rate) return fake_time();
	LightLock_Lock(&lock);
	u64 p = track_pos(t);
	LightLock_Unlock(&lock);
	return (float)p * 1000.0f / t->rate;
}

float audio_music_time(void) {
	Track *t = &tr[TR_MUSIC];
	if (!ok || !t->open || !t->rate) return fake_time();
	LightLock_Lock(&lock);
	u64 p = track_pos(t);
	LightLock_Unlock(&lock);
	if (t->loop && t->total) p %= t->total;
	return (float)p * 1000.0f / t->rate;
}

float audio_song_length(void) {
	Track *t = &tr[TR_MUSIC];
	return t->open && t->rate ? t->total * 1000.0f / t->rate : 0;
}
