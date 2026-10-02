/*
 * Astrix OS - unit tests for the Astrix UI toolkit.
 *
 * These run on the build host (no Wayland needed) so the drawing code can be
 * verified without booting the OS. The text/geometry tests assert real
 * invariants; the PPM dump lets a human confirm glyphs are legible.
 *
 * Build & run:  tests/run-ui-tests.sh
 */

#include "astrix_ui.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                                                     \
	do {                                                                                    \
		checks++;                                                                           \
		if (!(cond)) {                                                                      \
			failures++;                                                                     \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                           \
			fprintf(stderr, __VA_ARGS__);                                                  \
			fprintf(stderr, "\n");                                                          \
		}                                                                                   \
	} while (0)

/* --- helpers ------------------------------------------------------------- */

static uint32_t *alloc_canvas(struct astrix_canvas *c, int w, int h) {
	c->pixels = calloc((size_t)w * h, sizeof(uint32_t));
	c->width = w;
	c->height = h;
	c->stride = w;
	return c->pixels;
}

static void free_canvas(struct astrix_canvas *c) {
	free(c->pixels);
	c->pixels = NULL;
}

/* Count non-transparent pixels; used to prove drawing actually happened. */
static int count_drawn(const struct astrix_canvas *c) {
	int n = 0;
	for (int i = 0; i < c->width * c->height; i++) {
		if ((c->pixels[i] >> 24) != 0) {
			n++;
		}
	}
	return n;
}

static void write_ppm(const char *path, const struct astrix_canvas *c, int scale) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", c->width * scale, c->height * scale);
	for (int y = 0; y < c->height; y++) {
		for (int sy = 0; sy < scale; sy++) {
			for (int x = 0; x < c->width; x++) {
				uint32_t p = c->pixels[(size_t)y * c->stride + x];
				for (int sx = 0; sx < scale; sx++) {
					fputc((p >> 16) & 0xFF, f); /* r */
					fputc((p >> 8) & 0xFF, f);  /* g */
					fputc(p & 0xFF, f);         /* b */
				}
			}
		}
	}
	fclose(f);
}

/* --- tests --------------------------------------------------------------- */

static void test_fill_rect(void) {
	struct astrix_canvas c;
	alloc_canvas(&c, 20, 20);
	astrix_clear(&c, astrix_rgba(0, 0, 0, 0));
	astrix_fill_rect(&c, (struct astrix_rect){ 5, 5, 10, 10 },
	                 astrix_rgba(255, 0, 0, 255));
	CHECK(count_drawn(&c) == 100, "expected 100 filled px, got %d", count_drawn(&c));
	/* Pixel at centre is opaque red. */
	uint32_t p = c.pixels[10 * c.stride + 10];
	CHECK(((p >> 24) & 0xFF) == 255, "alpha should be 255");
	CHECK(((p >> 16) & 0xFF) == 255, "red channel should be 255");
	free_canvas(&c);
}

static void test_fill_rect_clips(void) {
	struct astrix_canvas c;
	alloc_canvas(&c, 10, 10);
	astrix_clear(&c, astrix_rgba(0, 0, 0, 0));
	/* Deliberately out of bounds: must not write out of the buffer. */
	astrix_fill_rect(&c, (struct astrix_rect){ -5, -5, 100, 100 },
	                 astrix_rgba(255, 255, 255, 255));
	CHECK(count_drawn(&c) == 100, "clipped fill should cover exactly the canvas, got %d",
	      count_drawn(&c));
	free_canvas(&c);
}

static void test_alpha_blend(void) {
	struct astrix_canvas c;
	alloc_canvas(&c, 4, 4);
	astrix_clear(&c, astrix_rgba(0, 0, 0, 255));      /* opaque black */
	astrix_fill_rect(&c, (struct astrix_rect){ 0, 0, 4, 4 },
	                 astrix_rgba(255, 255, 255, 128)); /* 50% white */
	uint32_t p = c.pixels[0];
	uint8_t r = (p >> 16) & 0xFF;
	/* 50% white over black should land near 127-128. */
	CHECK(r > 120 && r < 136, "50%% blend should be ~128, got %u", r);
	free_canvas(&c);
}

static void test_rounded_rect_corners_transparent(void) {
	struct astrix_canvas c;
	alloc_canvas(&c, 40, 40);
	astrix_clear(&c, astrix_rgba(0, 0, 0, 0));
	struct astrix_rect r = { 0, 0, 40, 40 };
	astrix_fill_rect_rounded(&c, r, 12, astrix_rgba(255, 255, 255, 255));
	/* The extreme corner pixel must remain transparent on a rounded rect. */
	uint32_t corner = c.pixels[0];
	CHECK(((corner >> 24) & 0xFF) == 0, "rounded corner should be transparent, got alpha %u",
	      (corner >> 24) & 0xFF);
	/* The centre must be filled. */
	uint32_t mid = c.pixels[20 * c.stride + 20];
	CHECK(((mid >> 24) & 0xFF) == 255, "rounded rect centre should be opaque");
	free_canvas(&c);
}

static void test_gradient(void) {
	struct astrix_canvas c;
	alloc_canvas(&c, 10, 32);
	astrix_clear(&c, astrix_rgba(0, 0, 0, 255));
	astrix_fill_rect_gradient_v(&c, (struct astrix_rect){ 0, 0, 10, 32 },
	                            astrix_rgba(0, 0, 0, 255), astrix_rgba(255, 255, 255, 255));
	uint8_t top = c.pixels[0] & 0xFF;
	uint8_t bottom = c.pixels[(31 * c.stride) + 0] & 0xFF;
	CHECK(top < 10, "gradient top should be dark, got %u", top);
	CHECK(bottom > 245, "gradient bottom should be light, got %u", bottom);
	free_canvas(&c);
}

static void test_text_width(void) {
	/* Advance is 6px per glyph, minus the trailing spacing. */
	CHECK(astrix_text_width("") == 0, "empty string has zero width");
	CHECK(astrix_text_width("A") == ASTRIX_FONT_W, "single glyph should be font width");
	CHECK(astrix_text_width("AB") == ASTRIX_FONT_W + ASTRIX_FONT_ADVANCE,
	      "two glyphs should be w + advance");
	CHECK(astrix_text_width("Hello") == 5 * ASTRIX_FONT_ADVANCE - (ASTRIX_FONT_ADVANCE - ASTRIX_FONT_W),
	      "five glyphs width wrong");
}

static void test_text_renders_pixels(void) {
	struct astrix_canvas c;
	alloc_canvas(&c, 200, 20);
	astrix_clear(&c, astrix_rgba(0, 0, 0, 0));
	astrix_draw_text(&c, 0, 0, "Astrix 123", astrix_rgba(255, 255, 255, 255));
	int n = count_drawn(&c);
	/* A full string must light up a plausible number of pixels. */
	CHECK(n > 100, "expected text to draw a decent number of pixels, got %d", n);
	/* Space must draw nothing. */
	struct astrix_canvas c2;
	alloc_canvas(&c2, 20, 20);
	astrix_clear(&c2, astrix_rgba(0, 0, 0, 0));
	astrix_draw_text(&c2, 0, 0, " ", astrix_rgba(255, 255, 255, 255));
	CHECK(count_drawn(&c2) == 0, "space should draw no pixels");
	free_canvas(&c2);
	free_canvas(&c);
}

static void test_every_glyph_visible(void) {
	/* Every printable ASCII glyph except space must produce ink. A blank or
	 * corrupted font table would otherwise only show up as unreadable UI. */
	struct astrix_canvas c;
	alloc_canvas(&c, 10, ASTRIX_FONT_H);
	int bad = 0;
	char missing[256] = { 0 };
	for (char ch = 0x21; ch <= 0x7E; ch++) {
		astrix_clear(&c, astrix_rgba(0, 0, 0, 0));
		char s[2] = { ch, 0 };
		astrix_draw_text(&c, 0, 0, s, astrix_rgba(255, 255, 255, 255));
		if (count_drawn(&c) == 0) {
			bad++;
			size_t l = strlen(missing);
			missing[l] = ch;
			missing[l + 1] = 0;
		}
	}
	CHECK(bad == 0, "%d glyph(s) render blank: %s", bad, missing);
	free_canvas(&c);
}

static void test_ellipsize(void) {
	char out[64];
	astrix_text_ellipsize("short", 1000, out, sizeof(out));
	CHECK(strcmp(out, "short") == 0, "short text should pass through unchanged, got '%s'", out);

	astrix_text_ellipsize("a very long application name indeed", 60, out, sizeof(out));
	CHECK(strstr(out, "...") != NULL, "long text should be ellipsized, got '%s'", out);
	CHECK((int)strlen(out) < 30, "ellipsized text should be short, got '%s'", out);
}

static void test_wrap(void) {
	char lines[8][256];
	int n = astrix_text_wrap("the quick brown fox jumps over the lazy dog", 90, lines, 8);
	CHECK(n > 1, "long text should wrap into multiple lines, got %d", n);
	for (int i = 0; i < n; i++) {
		CHECK(strlen(lines[i]) > 0, "wrapped line %d is empty", i);
	}
}

static void test_utf8_handling(void) {
	struct astrix_canvas c;
	alloc_canvas(&c, 200, 20);
	astrix_clear(&c, astrix_rgba(0, 0, 0, 0));
	/* Multi-byte UTF-8 must advance correctly and not corrupt the buffer. */
	astrix_draw_text(&c, 0, 0, "caf\xc3\xa9", astrix_rgba(255, 255, 255, 255));
	int w = astrix_text_width("caf\xc3\xa9");
	CHECK(w == ASTRIX_FONT_W + 3 * ASTRIX_FONT_ADVANCE, "utf-8 width wrong: %d", w);
	/* 'c','a','f' plus the 2-byte 'é' (rendered as a blank cell, since the
	 * built-in font is ASCII) must still lay out at 4 advances. */
	CHECK(count_drawn(&c) > 20, "utf-8 text should draw pixels, got %d", count_drawn(&c));
	free_canvas(&c);
}

static void test_anim_converges(void) {
	struct astrix_anim a;
	astrix_anim_init(&a, 0.0f);
	astrix_anim_set_target(&a, 1.0f);
	int steps = 0;
	/* Stepping at 60fps should converge quickly and never overshoot. */
	while (astrix_anim_step(&a, 1.0f / 60.0f) && steps < 1000) {
		steps++;
		CHECK(a.value <= 1.0f && a.value >= 0.0f, "anim overshot: %f", a.value);
	}
	CHECK(a.value > 0.99f, "anim should reach its target, got %f", a.value);
	CHECK(steps < 200, "anim should converge in under 200 frames, took %d", steps);
}

static void test_anim_idle_is_cheap(void) {
	struct astrix_anim a;
	astrix_anim_init(&a, 0.5f);
	/* A settled animation must report "nothing to do" so the shell does not
	 * schedule frames forever when idle. */
	CHECK(astrix_anim_step(&a, 1.0f / 60.0f) == false, "idle anim should not animate");
}

static void test_anim_frame_rate_independence(void) {
	struct astrix_anim a, b;
	astrix_anim_init(&a, 0.0f);
	astrix_anim_init(&b, 0.0f);
	astrix_anim_set_target(&a, 1.0f);
	astrix_anim_set_target(&b, 1.0f);
	/* 1 second at 60fps vs 20 steps of 50ms: both should land in the same
	 * place, proving the easing is time-based rather than per-frame. */
	for (int i = 0; i < 60; i++) {
		astrix_anim_step(&a, 1.0f / 60.0f);
	}
	for (int i = 0; i < 20; i++) {
		astrix_anim_step(&b, 0.05f);
	}
	CHECK(fabsf(a.value - b.value) < 0.01f, "anim is frame-rate dependent: %f vs %f", a.value,
	      b.value);
}

static void test_easing_bounds(void) {
	CHECK(astrix_ease_out_cubic(0.0f) == 0.0f, "ease_out_cubic(0) should be 0");
	CHECK(fabsf(astrix_ease_out_cubic(1.0f) - 1.0f) < 0.0001f, "ease_out_cubic(1) should be 1");
	CHECK(astrix_ease_in_out_cubic(0.0f) == 0.0f, "ease_in_out_cubic(0) should be 0");
	CHECK(fabsf(astrix_ease_in_out_cubic(1.0f) - 1.0f) < 0.0001f, "ease_in_out_cubic(1) should be 1");
	/* Out-of-range input must clamp, not extrapolate. */
	CHECK(astrix_ease_out_cubic(-5.0f) == 0.0f, "ease should clamp below 0");
	CHECK(fabsf(astrix_ease_out_cubic(5.0f) - 1.0f) < 0.0001f, "ease should clamp above 1");
}

static void test_rect_geometry(void) {
	struct astrix_rect r = { 10, 20, 30, 40 };
	CHECK(astrix_rect_contains(r, 10, 20), "top-left is inside");
	CHECK(astrix_rect_contains(r, 39, 59), "bottom-right-1 is inside");
	CHECK(!astrix_rect_contains(r, 40, 59), "right edge is outside (half-open)");
	CHECK(!astrix_rect_contains(r, 10, 60), "bottom edge is outside (half-open)");
	struct astrix_rect i = astrix_rect_inset(r, 5, 5);
	CHECK(i.x == 15 && i.y == 25 && i.w == 20 && i.h == 30, "inset math wrong");
}

static void test_theme_contrast(void) {
	/* The shell must remain legible: verify text vs background differ enough
	 * in both themes. A mistake here makes the OS unreadable. */
	const struct astrix_theme *lt = astrix_theme_light();
	const struct astrix_theme *dk = astrix_theme_dark();
	int lt_diff = abs((int)lt->text.r - (int)lt->background.r);
	int dk_diff = abs((int)dk->text.r - (int)dk->background.r);
	CHECK(lt_diff > 100, "light theme text/background contrast too low: %d", lt_diff);
	CHECK(dk_diff > 100, "dark theme text/background contrast too low: %d", dk_diff);
	CHECK(lt->touch_target >= 44, "touch target should be at least 44px");
}

/* Render a sample screen so a human can eyeball the font. */
static void dump_font_sheet(void) {
	const int W = 420, H = 200;
	struct astrix_canvas c;
	alloc_canvas(&c, W, H);
	const struct astrix_theme *t = astrix_theme_dark();
	astrix_clear(&c, t->background);
	int y = 6;
	astrix_draw_text(&c, 6, y, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", t->text);
	y += 16;
	astrix_draw_text(&c, 6, y, "abcdefghijklmnopqrstuvwxyz", t->text);
	y += 16;
	astrix_draw_text(&c, 6, y, "0123456789", t->text);
	y += 16;
	astrix_draw_text(&c, 6, y, "!\"#$%&'()*+,-./:;<=>?@", t->text);
	y += 16;
	astrix_draw_text(&c, 6, y, "[\\]^_`{|}~", t->text);
	y += 18;
	astrix_draw_text(&c, 6, y, "Astrix OS - 3B5BDB", t->primary);
	y += 16;
	astrix_draw_text(&c, 6, y, "Wi-Fi  BT  87%  12:45", t->success);
	y += 16;
	astrix_fill_rect_rounded(&c, (struct astrix_rect){ 6, y - 2, 180, 20 }, 8, t->surface);
	astrix_draw_text(&c, 14, y + 2, "rounded card surface", t->text);
	write_ppm("build/font-sheet.ppm", &c, 2);
	free_canvas(&c);
}

int main(void) {
	test_fill_rect();
	test_fill_rect_clips();
	test_alpha_blend();
	test_rounded_rect_corners_transparent();
	test_gradient();
	test_text_width();
	test_text_renders_pixels();
	test_every_glyph_visible();
	test_ellipsize();
	test_wrap();
	test_utf8_handling();
	test_anim_converges();
	test_anim_idle_is_cheap();
	test_anim_frame_rate_independence();
	test_easing_bounds();
	test_rect_geometry();
	test_theme_contrast();

	/* Only write the visual artefact if the assertions passed. */
	if (failures == 0) {
		dump_font_sheet();
		printf("wrote build/font-sheet.ppm (open it to inspect glyphs)\n");
	}

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
