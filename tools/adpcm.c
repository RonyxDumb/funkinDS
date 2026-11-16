// IMA ADPCM encoder for the DS audio streamer (see source/audio.c).
// usage: adpcm in.raw out.adp     (in: s16le mono)
// Output: blocks of 2048 samples: s16 predictor, u8 step index, u8 0,
// then 1024 bytes of nibbles (low nibble first). The decoder state at the
// start of every block is stored, so blocks can be decoded independently.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 2048

static const int steps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
	1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
	7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int idxTab[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

int main(int argc, char **argv) {
	if (argc < 3) return 1;
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END);
	long n = ftell(f) / 2;
	fseek(f, 0, SEEK_SET);
	short *s = calloc(n + BLOCK, 2);
	if (fread(s, 2, n, f) != (size_t)n) return 1;
	fclose(f);
	FILE *o = fopen(argv[2], "wb");
	int pred = 0, idx = 0;
	for (long b = 0; b < n; b += BLOCK) {
		unsigned char hdr[4] = { pred & 255, (pred >> 8) & 255, (unsigned char)idx, 0 };
		fwrite(hdr, 1, 4, o);
		unsigned char out[BLOCK / 2];
		memset(out, 0, sizeof(out));
		for (int i = 0; i < BLOCK; i++) {
			int x = s[b + i];
			int step = steps[idx];
			int diff = x - pred;
			int code = 0;
			if (diff < 0) { code = 8; diff = -diff; }
			int delta = step >> 3;
			if (diff >= step) { code |= 4; diff -= step; delta += step; }
			if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
			if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
			pred += (code & 8) ? -delta : delta;
			if (pred > 32767) pred = 32767;
			if (pred < -32768) pred = -32768;
			idx += idxTab[code];
			if (idx < 0) idx = 0;
			if (idx > 88) idx = 88;
			out[i >> 1] |= code << ((i & 1) * 4);
		}
		fwrite(out, 1, BLOCK / 2, o);
	}
	fclose(o);
	return 0;
}
