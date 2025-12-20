#include <assert.h>
#include <err.h>
#include <freetype/freetype.h>
#include <inttypes.h>
#include <stdio.h>

#define ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))

#define DISPLAYS_X 24
/* How many 7-segment displays wide/tall a character is. */
#define CHAR_DISPLAYS_X 5
#define CHAR_DISPLAYS_Y 6
/* Maximum coordinates of a display segment. */
#define SEGMENT_MAX_X 126
#define SEGMENT_MAX_Y 190
/* Percent (as an integer <- [0, 100]) of bitmap pixels in a segment
 * that must be "1" for the segment to be considered enabled. */
#define ON_THRESHOLD 50

struct metrics {
	unsigned advance;
	unsigned bearing_y;
	unsigned below_baseline;
};

struct bm {
	unsigned char *pixels;
	unsigned pixels_width;
	unsigned pixels_height;
	int pitch;
	/* size of a bounding box to fit all glyph bitmaps */
	unsigned box_width;
	unsigned box_height;
	/* bearing of `pixels` within bounding box */
	unsigned top;
	unsigned left;
};

/* Positions of each segment within the bounding box of a single display. */
static const struct {
	/* X coordinate of top left corner. Within [0, SEGMENT_MAX_X]. */
	unsigned tl_x;
	/* Y coordinate of top left corner. Within [0, SEGMENT_MAX_Y]. */
	unsigned tl_y;
	/* Within [0, SEGMENT_MAX_X]. */
	unsigned width;
	/* Within [0, SEGMENT_MAX_Y]. */
	unsigned height;
} segpos[] = {
	/* A */ { 50, 26, 48, 20 },
	/* B */ { 91, 25, 20, 70 },
	/* C */ { 91, 90, 20, 70 },
	/* D */ { 32, 150, 48, 20 },
	/* E */ { 23, 90, 20, 70 },
	/* F */ { 34, 87, 57, 20 },
	/* G */ { 24, 25, 20, 70 },
};

static const char *ft_errstr(FT_Error e) {
#undef FTERRORS_H_
#define FT_ERROR_START_LIST
#define FT_ERROR_END_LIST
#define FT_ERRORDEF(name, code, str) if (e == code) return str;
#include <freetype/fterrors.h>
	return "unknown FreeType error";
}

static void head(void) {
	printf(
		"#include <stdint.h>\n"
		"#include <avr/pgmspace.h>\n"
		"\n"
		"#define DISPLAYS_X %u\n"
		"#define CHAR_DISPLAYS_X %u\n"
		"#define CHAR_DISPLAYS_Y %u\n"
		"#define COLON_DISPLAYS_X %u\n"
		"\n"
		"static const uint8_t font[][CHAR_DISPLAYS_Y][CHAR_DISPLAYS_X] PROGMEM = {\n"
		, DISPLAYS_X
		, CHAR_DISPLAYS_X
		, CHAR_DISPLAYS_Y
		, DISPLAYS_X - 4 * CHAR_DISPLAYS_X
	);
}

static void foot(void) {
	puts("};");
}

static _Bool getpixel(const struct bm *bm, size_t x, size_t y) {
	assert(x < bm->box_width);
	assert(y < bm->box_height);

	if (x < bm->left || x >= bm->left + bm->pixels_width)
		return 0;
	if (y < bm->top || y >= bm->top + bm->pixels_height)
		return 0;

	x -= bm->left;
	y -= bm->top;

	size_t bidx = y * bm->pitch + x / 8;
	if (x % 8)
		bidx++;
	unsigned char byte = bm->pixels[bidx];
	size_t bitpos = x % 8;

	return !!(byte & (1 << (7 - bitpos)));
}

static uint8_t display_bits(
	unsigned dispidx_x,
	unsigned dispidx_y,
	const struct bm *bm
) {
	/* position of our view into the bitmap for this display */
	unsigned window_x = (bm->box_width * dispidx_x) / CHAR_DISPLAYS_X;
	unsigned window_y = (bm->box_height * dispidx_y) / CHAR_DISPLAYS_Y;
	unsigned window_width = bm->box_width / CHAR_DISPLAYS_X;
	unsigned window_height = bm->box_height / CHAR_DISPLAYS_Y;

	uint8_t bits = 0;

	for (size_t si = 0; si < ARRAY_LEN(segpos); si++) {
		/* segment position scaled to the size of the window */
		unsigned tl_x = window_x +
			(window_width * segpos[si].tl_x) / SEGMENT_MAX_X;
		unsigned tl_y = window_y +
			(window_height * segpos[si].tl_y) / SEGMENT_MAX_Y;
		unsigned width = (window_width * segpos[si].width) / SEGMENT_MAX_X;
		unsigned height = (window_height * segpos[si].height) / SEGMENT_MAX_Y;

		size_t pixels_on = 0;

		for (size_t y = tl_y; y < tl_y + height; y++) {
			for (size_t x = tl_x; x < tl_x + width; x++) {
				pixels_on += getpixel(bm, x, y);
			}
		}

		if (pixels_on >= (width * height * ON_THRESHOLD) / 100)
			bits |= 1 << si;
	}

	return bits;
}

static void bake_char(FT_Face face, const struct metrics *maxm, char ch) {
	FT_Error e;

	if ((e = FT_Load_Char(face, ch, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO)))
		errx(1, "FT_Load_Char: %s", ft_errstr(e));
	FT_GlyphSlot g = face->glyph;
	if (g->bitmap.pitch < 0) {
		g->bitmap.pitch *= -1;
		g->bitmap.buffer -= g->bitmap.pitch * g->bitmap.rows;
	}
	long bearx = g->metrics.horiBearingX >> 6;
	long beary = g->metrics.horiBearingY >> 6;

	struct bm bm = {
		.pixels = g->bitmap.buffer,
		.pixels_height = g->bitmap.rows,
		.pixels_width = g->bitmap.width,
		.pitch = g->bitmap.pitch,
		.box_width = maxm->advance,
		.box_height = maxm->bearing_y + maxm->below_baseline,
		.top = maxm->bearing_y - beary,
		.left = bearx,
	};

	puts("\t{");
	for (unsigned dispidx_y = 0; dispidx_y < CHAR_DISPLAYS_Y; dispidx_y++) {
		printf("\t\t{ ");
		for (unsigned dispidx_x = 0; dispidx_x < CHAR_DISPLAYS_X; dispidx_x++) {
			uint8_t b = display_bits(
				dispidx_x,
				dispidx_y,
				&bm
			);
			printf("0x%.2"PRIX8", ", b);
		}
		printf("},\n");
	}
	puts("\t},");
}

static void get_max_metrics(FT_Face face, const char *chars, struct metrics *maxm) {
	FT_Error e;

	maxm->advance = 0;
	maxm->bearing_y = 0;
	maxm->below_baseline = 0;

	for (size_t i = 0; chars[i]; i++) {
		if ((e = FT_Load_Char(face, chars[i], 0)))
			errx(1, "FT_Load_Char: %s", ft_errstr(e));
		FT_Glyph_Metrics *m = &face->glyph->metrics;

		long advance = m->horiAdvance >> 6;
		long bearx = m->horiBearingX >> 6;
		long beary = m->horiBearingY >> 6;
		long width = m->width >> 6;
		long height = m->height >> 6;
		long below_baseline = height - beary;

		if (advance < bearx + width)
			advance = bearx + width;

		if (advance > maxm->advance)
			maxm->advance = advance;

		if (beary > maxm->bearing_y)
			maxm->bearing_y = beary;

		if (below_baseline > maxm->below_baseline)
			maxm->below_baseline = below_baseline;
	}
}

static void bake_font(FT_Face face, const char *chars) {
	FT_Error e;

	assert(FT_IS_SCALABLE(face));

	if ((e = FT_Set_Pixel_Sizes(face, 500, 0)))
		errx(1, "FT_Set_Pixel_Sizes: %s", ft_errstr(e));

	struct metrics maxm;
	get_max_metrics(face, chars, &maxm);

	head();
	for (size_t i = 0; chars[i]; i++)
		bake_char(face, &maxm, chars[i]);
	foot();
}

int main(int argc, char **argv) {
	FT_Library ft;
	FT_Face face;
	FT_Error e;

	if (argc != 2)
		errx(1, "usage: %s font.ttf", argv[0]);

	if ((e = FT_Init_FreeType(&ft)))
		errx(1, "FT_Init_FreeType: %s", ft_errstr(e));

	if ((e = FT_New_Face(ft, argv[1], 0, &face)))
		errx(1, "FT_New_Face: %s", ft_errstr(e));

	bake_font(face, "O123456789");
}
