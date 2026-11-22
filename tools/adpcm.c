// IMA ADPCM encoder for the 3DS audio streamer (see source/audio.c).
// usage: adpcm in.raw out.adp rate channels     (in: s16le, interleaved)
// Output: "IMA2", u32 rate, u16 channels, u16 0, u32 samples (per channel),
// then blocks of 2048 samples; in each block, per channel: s16 predictor,
// u8 step index, u8 0, then 1024 bytes of nibbles (low nibble first). The
// decoder state at the start of every block is stored, so blocks can be
// decoded independently.
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

static void put32(FILE *o, unsigned v) {
	unsigned char b[4] = { v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24 };
	fwrite(b, 1, 4, o);
}

int main(int argc, char **argv) {
	if (argc < 3) return 1;
	unsigned rate = argc > 3 ? (unsigned)atoi(argv[3]) : 32000;
	int ch = argc > 4 ? atoi(argv[4]) : 1;
	if (ch < 1 || ch > 2) ch = 1;
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END);
	long n = ftell(f) / 2 / ch;
	fseek(f, 0, SEEK_SET);
	short *s = calloc((n + BLOCK) * ch, 2);
	if (fread(s, 2, n * ch, f) != (size_t)(n * ch)) return 1;
	fclose(f);
	FILE *o = fopen(argv[2], "wb");
	if (!o) return 1;
	fwrite("IMA2", 1, 4, o);
	put32(o, rate);
	unsigned char c2[4] = { (unsigned char)ch, 0, 0, 0 };
	fwrite(c2, 1, 4, o);
	put32(o, (unsigned)n);
	int pred[2] = { 0, 0 }, idx[2] = { 0, 0 };
	for (long b = 0; b < n; b += BLOCK) {
		for (int c = 0; c < ch; c++) {
			unsigned char hdr[4] = { pred[c] & 255, (pred[c] >> 8) & 255, (unsigned char)idx[c], 0 };
			fwrite(hdr, 1, 4, o);
			unsigned char out[BLOCK / 2];
			memset(out, 0, sizeof(out));
			int p = pred[c], ix = idx[c];
			for (int i = 0; i < BLOCK; i++) {
				int x = s[(b + i) * ch + c];
				int step = steps[ix];
				int diff = x - p;
				int code = 0;
				if (diff < 0) { code = 8; diff = -diff; }
				int delta = step >> 3;
				if (diff >= step) { code |= 4; diff -= step; delta += step; }
				if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
				if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
				p += (code & 8) ? -delta : delta;
				if (p > 32767) p = 32767;
				if (p < -32768) p = -32768;
				ix += idxTab[code];
				if (ix < 0) ix = 0;
				if (ix > 88) ix = 88;
				out[i >> 1] |= code << ((i & 1) * 4);
			}
			pred[c] = p;
			idx[c] = ix;
			fwrite(out, 1, BLOCK / 2, o);
		}
	}
	fclose(o);
	return 0;
}
