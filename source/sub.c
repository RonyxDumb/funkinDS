// Bottom screen: a retained display list. The menus draw into it only when
// something changes (like the DS version drew into a bitmap); the GPU replays
// it every frame. An opaque fill removes the items it covers, so the list
// stays small when fields are redrawn over and over.
#include "engine.h"

enum { SI_FILL, SI_GRAD, SI_TEXT, SI_SPRITE, SI_BUTTON };
typedef struct {
	u8 kind, font;
	s16 x, y, w, h;
	u32 c0, c1;
	u8 alpha;
	u16 text;               // offset in the text pool
	Sheet *sheet;
	int frame;
	float scale;
	bool flip;
} SubItem;

#define MAX_ITEMS 400
#define POOL 12288
static SubItem items[MAX_ITEMS];
static int nItems;
static char pool[POOL];
static int poolUsed;
static u32 bgColor;
static SubHook hook;

void sub_init(void) { sub_clear(0); }
void sub_present(void) {}
void sub_set_hook(SubHook h) { hook = h; }

void sub_clear(u32 color) {
	nItems = 0;
	poolUsed = 0;
	bgColor = color;
	hook = NULL;
}

// drop the items fully inside an opaque rectangle
static void cover(int x, int y, int w, int h) {
	int o = 0;
	for (int i = 0; i < nItems; i++) {
		SubItem *it = &items[i];
		bool inside = it->x >= x && it->y >= y && it->x + it->w <= x + w && it->y + it->h <= y + h;
		if (!inside) items[o++] = *it;
	}
	nItems = o;
	if (!nItems) poolUsed = 0;
	if (x <= 0 && y <= 0 && x + w >= BW && y + h >= BH) poolUsed = 0;
}

static SubItem *add(int kind, int x, int y, int w, int h) {
	if (nItems >= MAX_ITEMS) {
		// full: keep the newest half (old items are most likely covered)
		memmove(items, items + MAX_ITEMS / 2, sizeof(SubItem) * (MAX_ITEMS - MAX_ITEMS / 2));
		nItems = MAX_ITEMS - MAX_ITEMS / 2;
	}
	SubItem *it = &items[nItems++];
	memset(it, 0, sizeof(*it));
	it->kind = kind;
	it->x = x; it->y = y; it->w = w; it->h = h;
	it->alpha = 255;
	return it;
}

void sub_fill(int x, int y, int w, int h, u32 color) {
	if (w <= 0 || h <= 0) return;
	if (x <= 0 && y <= 0 && x + w >= BW && y + h >= BH) { sub_clear(color); return; }
	cover(x, y, w, h);
	add(SI_FILL, x, y, w, h)->c0 = color;
}

void sub_fill_blend(int x, int y, int w, int h, u32 color, int a) {
	if (w <= 0 || h <= 0 || a <= 0) return;
	if (a >= 255) { sub_fill(x, y, w, h, color); return; }
	SubItem *it = add(SI_FILL, x, y, w, h);
	it->c0 = color;
	it->alpha = a;
}

void sub_gradient(int x, int y, int w, int h, u32 top, u32 bottom) {
	if (w <= 0 || h <= 0) return;
	cover(x, y, w, h);
	SubItem *it = add(SI_GRAD, x, y, w, h);
	it->c0 = top;
	it->c1 = bottom;
}

void sub_rect(int x, int y, int w, int h, u32 color) {
	sub_fill(x, y, w, 1, color);
	sub_fill(x, y + h - 1, w, 1, color);
	sub_fill(x, y, 1, h, color);
	sub_fill(x + w - 1, y, 1, h, color);
}

void sub_button(int x, int y, int w, int h, bool sel) {
	cover(x, y, w, h);
	SubItem *it = add(SI_BUTTON, x, y, w, h);
	it->c0 = sel ? 1 : 0;
}

void sub_text(int font, int x, int y, const char *s, u32 color) {
	int len = strlen(s);
	if (!len) return;
	if (poolUsed + len + 1 > POOL) {
		// compact the pool
		static char np[POOL];
		int used = 0;
		for (int i = 0; i < nItems; i++) {
			if (items[i].kind != SI_TEXT) continue;
			const char *t = pool + items[i].text;
			int l = strlen(t) + 1;
			memcpy(np + used, t, l);
			items[i].text = used;
			used += l;
		}
		memcpy(pool, np, used);
		poolUsed = used;
		if (poolUsed + len + 1 > POOL) return;
	}
	font_load(font);
	SubItem *it = add(SI_TEXT, x, y, font_width(font, s), font_height(font));
	it->font = font;
	it->c0 = color;
	it->text = poolUsed;
	memcpy(pool + poolUsed, s, len + 1);
	poolUsed += len + 1;
}

void sub_text_centered(int font, int cx, int y, const char *s, u32 color) {
	sub_text(font, cx - font_width(font, s) / 2, y, s, color);
}

void sub_text_right(int font, int rx, int y, const char *s, u32 color) {
	sub_text(font, rx - font_width(font, s), y, s, color);
}

void sub_sprite(Sheet *sh, int frame, float x, float y, float scale, bool flip) {
	if (!sh || !sh->loaded) return;
	float x0, y0, x1, y1;
	sheet_frame_bounds(sh, frame, &x0, &y0, &x1, &y1);
	SubItem *it = add(SI_SPRITE, (int)(x + x0 * scale), (int)(y + y0 * scale), (int)((x1 - x0) * scale), (int)((y1 - y0) * scale));
	it->sheet = sh;
	it->frame = frame;
	it->scale = scale;
	it->flip = flip;
	it->c0 = (u32)(int)x;   // origin
	it->c1 = (u32)(int)y;
}

static void draw_button(const SubItem *it) {
	bool sel = it->c0;
	float x0 = it->x, y0 = it->y, x1 = it->x + it->w, y1 = it->y + it->h;
	u32 top = sel ? RGB(0xFF, 0xE2, 0x7A) : RGB(0x5A, 0x3E, 0x92);
	u32 bot = sel ? RGB(0xF2, 0xB8, 0x2E) : RGB(0x38, 0x24, 0x62);
	u32 edge = sel ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1A, 0x10, 0x30);
	// rounded corners (2 px)
	gfx_rect(x0 + 2, y0, x1 - 2, y1, edge, 255);
	gfx_rect(x0, y0 + 2, x1, y1 - 2, edge, 255);
	gfx_rect(x0 + 1, y0 + 1, x1 - 1, y1 - 1, edge, 255);
	gfx_gradient(x0 + 2, y0 + 2, x1 - 2, y1 - 2, top, bot, 255);
	gfx_rect(x0 + 2, y0 + 2, x1 - 2, y0 + 3, WHITE, sel ? 140 : 40);
}

void sub_render(void) {
	gfx_screen(SCREEN_BOTTOM);
	gfx_rect(0, 0, BW, BH, bgColor, 255);
	for (int i = 0; i < nItems; i++) {
		const SubItem *it = &items[i];
		switch (it->kind) {
		case SI_FILL: gfx_rect(it->x, it->y, it->x + it->w, it->y + it->h, it->c0, it->alpha); break;
		case SI_GRAD: gfx_gradient(it->x, it->y, it->x + it->w, it->y + it->h, it->c0, it->c1, 255); break;
		case SI_BUTTON: draw_button(it); break;
		case SI_TEXT: font_draw(it->font, it->x, it->y, pool + it->text, it->c0, 255); break;
		case SI_SPRITE:
			if (it->sheet->loaded) {
				float x = (int)it->c0, y = (int)it->c1;
				if (it->flip) {
					float a, b, c, d;
					sheet_frame_bounds(it->sheet, it->frame, &a, &b, &c, &d);
					sheet_draw_flip(it->sheet, it->frame, x + (a + c) * it->scale, y, it->scale, it->scale, 1, WHITE, 255);
				} else sheet_draw(it->sheet, it->frame, x, y, it->scale, WHITE, 255);
			}
			break;
		}
	}
	if (hook) hook();
}
