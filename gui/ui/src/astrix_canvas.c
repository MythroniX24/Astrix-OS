/*
 * Astrix OS - canvas raster operations, themes, colour and animation.
 *
 * All writes are bounds-checked. The canvas pixel format is ARGB8888 in
 * native-endian 32-bit words, which is exactly what wl_shm expects for
 * WL_SHM_FORMAT_ARGB8888.
 */

#include "astrix_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- colour -------------------------------------------------------------- */

struct astrix_color astrix_color_lerp(struct astrix_color a, struct astrix_color b, float t) {
	if (t < 0.0f) {
		t = 0.0f;
	}
	if (t > 1.0f) {
		t = 1.0f;
	}
	struct astrix_color o;
	o.r = (uint8_t)(a.r + (float)(b.r - a.r) * t);
	o.g = (uint8_t)(a.g + (float)(b.g - a.g) * t);
	o.b = (uint8_t)(a.b + (float)(b.b - a.b) * t);
	o.a = (uint8_t)(a.a + (float)(b.a - a.a) * t);
	return o;
}

struct astrix_color astrix_color_with_alpha(struct astrix_color c, uint8_t a) {
	c.a = a;
	return c;
}

/* --- pixel ops ----------------------------------------------------------- */

/*
 * Source-over alpha blend of `col` onto the destination pixel. We blend in
 * 8-bit integer space with an integer fast path for a == 255, because the
 * shell does tens of thousands of these per frame and float math shows up in
 * the profile.
 */
void astrix_blend_pixel(struct astrix_canvas *c, int x, int y, struct astrix_color col) {
	if (!c || !c->pixels || x < 0 || y < 0 || x >= c->width || y >= c->height || col.a == 0) {
		return;
	}
	uint32_t *p = &c->pixels[(size_t)y * c->stride + x];
	if (col.a == 255) {
		*p = ((uint32_t)col.a << 24) | ((uint32_t)col.r << 16) | ((uint32_t)col.g << 8) | col.b;
		return;
	}
	uint32_t d = *p;
	uint32_t da = (d >> 24) & 0xFF;
	if (da == 0) {
		*p = ((uint32_t)col.a << 24) | ((uint32_t)col.r << 16) | ((uint32_t)col.g << 8) | col.b;
		return;
	}
	uint32_t sa = col.a;
	/* out_a = sa + da*(255-sa)/255 */
	uint32_t out_a = sa + (da * (255 - sa) + 127) / 255;
	if (out_a == 0) {
		*p = 0;
		return;
	}
	uint32_t inv = 255 - sa;
	uint32_t out_r = (col.r * sa + ((d >> 16) & 0xFF) * da * inv / 255) / (out_a ? out_a : 1);
	uint32_t out_g = (col.g * sa + ((d >> 8) & 0xFF) * da * inv / 255) / (out_a ? out_a : 1);
	uint32_t out_b = (col.b * sa + (d & 0xFF) * da * inv / 255) / (out_a ? out_a : 1);
	if (out_r > 255) {
		out_r = 255;
	}
	if (out_g > 255) {
		out_g = 255;
	}
	if (out_b > 255) {
		out_b = 255;
	}
	*p = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

void astrix_clear(struct astrix_canvas *c, struct astrix_color col) {
	if (!c || !c->pixels) {
		return;
	}
	uint32_t v = ((uint32_t)col.a << 24) | ((uint32_t)col.r << 16) | ((uint32_t)col.g << 8) | col.b;
	for (int y = 0; y < c->height; y++) {
		uint32_t *row = &c->pixels[(size_t)y * c->stride];
		for (int x = 0; x < c->width; x++) {
			row[x] = v;
		}
	}
}

void astrix_fill_rect(struct astrix_canvas *c, struct astrix_rect r, struct astrix_color col) {
	if (!c || !c->pixels || col.a == 0) {
		return;
	}
	/* Clip. */
	if (r.x < 0) {
		r.w += r.x;
		r.x = 0;
	}
	if (r.y < 0) {
		r.h += r.y;
		r.y = 0;
	}
	if (r.x + r.w > c->width) {
		r.w = c->width - r.x;
	}
	if (r.y + r.h > c->height) {
		r.h = c->height - r.y;
	}
	if (r.w <= 0 || r.h <= 0) {
		return;
	}
	/* Fast path: fully opaque run fills. */
	if (col.a == 255) {
		uint32_t v = ((uint32_t)255 << 24) | ((uint32_t)col.r << 16) | ((uint32_t)col.g << 8) | col.b;
		for (int y = r.y; y < r.y + r.h; y++) {
			uint32_t *row = &c->pixels[(size_t)y * c->stride];
			for (int x = r.x; x < r.x + r.w; x++) {
				row[x] = v;
			}
		}
		return;
	}
	for (int y = r.y; y < r.y + r.h; y++) {
		for (int x = r.x; x < r.x + r.w; x++) {
			astrix_blend_pixel(c, x, y, col);
		}
	}
}

void astrix_fill_rect_gradient_v(struct astrix_canvas *c, struct astrix_rect r,
                                struct astrix_color top, struct astrix_color bottom) {
	if (!c || !c->pixels || r.h <= 0) {
		return;
	}
	for (int y = 0; y < r.h; y++) {
		float t = (r.h > 1) ? (float)y / (float)(r.h - 1) : 0.0f;
		struct astrix_color col = astrix_color_lerp(top, bottom, t);
		astrix_fill_rect(c, (struct astrix_rect){ r.x, r.y + y, r.w, 1 }, col);
	}
}

/* --- rounded rects ------------------------------------------------------- */

/*
 * Rounded corners via the standard "inside the corner circle" test. We
 * compute an 8x8 coverage mask per corner so edges look antialiased enough
 * without a full scanline rasteriser.
 */
static int corner_coverage(int dx, int dy) {
	/* dx,dy are 0..radius-1 measured from the corner vertex. */
	static const uint8_t cov[8] = { 0, 40, 96, 150, 200, 232, 250, 255 };
	if (dx < 0 || dy < 0 || dx > 7 || dy > 7) {
		return 255;
	}
	/* Distance-based blend between the two axis coverages. */
	int cx = cov[dx], cy = cov[dy];
	return (cx + cy) / 2;
}

void astrix_fill_rect_rounded(struct astrix_canvas *c, struct astrix_rect r, int radius,
                              struct astrix_color col) {
	if (!c || !c->pixels || col.a == 0) {
		return;
	}
	if (radius <= 0) {
		astrix_fill_rect(c, r, col);
		return;
	}
	if (radius * 2 > r.h) {
		radius = r.h / 2;
	}
	if (radius * 2 > r.w) {
		radius = r.w / 2;
	}
	/* Middle band (full width, no corners). */
	astrix_fill_rect(c, (struct astrix_rect){ r.x, r.y + radius, r.w, r.h - 2 * radius }, col);
	/* Top and bottom bands, spanning the inner width. */
	astrix_fill_rect(c, (struct astrix_rect){ r.x + radius, r.y, r.w - 2 * radius, radius }, col);
	astrix_fill_rect(c, (struct astrix_rect){ r.x + radius, r.y + r.h - radius, r.w - 2 * radius, radius },
	                 col);
	/* Corners. */
	const int rc[4][2] = { { r.x, r.y }, { r.x + r.w - 1, r.y },
		                   { r.x, r.y + r.h - 1 }, { r.x + r.w - 1, r.y + r.h - 1 } };
	for (int i = 0; i < 4; i++) {
		for (int dy = 0; dy < radius; dy++) {
			for (int dx = 0; dx < radius; dx++) {
				int a = corner_coverage(dx, dy);
				if (a == 0) {
					continue;
				}
				struct astrix_color cc = col;
				cc.a = (uint8_t)((col.a * a) / 255);
				astrix_blend_pixel(c, rc[i][0] - (i & 1 ? radius - 1 - dx : dx),
				                   rc[i][1] - (i & 2 ? radius - 1 - dy : dy), cc);
			}
		}
	}
}

void astrix_stroke_rect(struct astrix_canvas *c, struct astrix_rect r, int thickness,
                        struct astrix_color col) {
	if (!c || !c->pixels || thickness <= 0) {
		return;
	}
	astrix_fill_rect(c, (struct astrix_rect){ r.x, r.y, r.w, thickness }, col);
	astrix_fill_rect(c, (struct astrix_rect){ r.x, r.y + r.h - thickness, r.w, thickness }, col);
	astrix_fill_rect(c, (struct astrix_rect){ r.x, r.y + thickness, thickness, r.h - 2 * thickness },
	                 col);
	astrix_fill_rect(c,
	                 (struct astrix_rect){ r.x + r.w - thickness, r.y + thickness, thickness,
	                                       r.h - 2 * thickness },
	                 col);
}

void astrix_stroke_rect_rounded(struct astrix_canvas *c, struct astrix_rect r, int radius,
                                int thickness, struct astrix_color col) {
	if (!c || !c->pixels || thickness <= 0) {
		return;
	}
	if (radius <= 0) {
		astrix_stroke_rect(c, r, thickness, col);
		return;
	}
	if (radius * 2 > r.h) {
		radius = r.h / 2;
	}
	if (radius * 2 > r.w) {
		radius = r.w / 2;
	}
	/* Straight edges only span the non-corner region. */
	astrix_fill_rect(c, (struct astrix_rect){ r.x + radius, r.y, r.w - 2 * radius, thickness },
	                 col);
	astrix_fill_rect(c,
	                 (struct astrix_rect){ r.x + radius, r.y + r.h - thickness, r.w - 2 * radius,
	                                       thickness },
	                 col);
	astrix_fill_rect(c, (struct astrix_rect){ r.x, r.y + radius, thickness, r.h - 2 * radius },
	                 col);
	astrix_fill_rect(c,
	                 (struct astrix_rect){ r.x + r.w - thickness, r.y + radius, thickness,
	                                       r.h - 2 * radius },
	                 col);
	/* Corner arcs. */
	for (int corner = 0; corner < 4; corner++) {
		int ox = (corner & 1) ? r.x + r.w - 1 : r.x;
		int oy = (corner & 2) ? r.y + r.h - 1 : r.y;
		int sgx = (corner & 1) ? -1 : 1;
		int sgy = (corner & 2) ? -1 : 1;
		for (int dy = 0; dy < radius; dy++) {
			for (int dx = 0; dx < radius; dx++) {
				int a = corner_coverage(dx, dy);
				if (a < 128) {
					continue;
				}
				/* Draw the arc: near the edge of the radius circle. */
				int px = ox + sgx * dx;
				int py = oy + sgy * dy;
				/* Only paint the outermost `thickness` pixels. */
				int inset = (radius - 1) - dx;
				int inset_y = (radius - 1) - dy;
				int d = inset < inset_y ? inset : inset_y;
				if (d >= thickness) {
					continue;
				}
				struct astrix_color cc = col;
				cc.a = (uint8_t)((col.a * a) / 255);
				astrix_blend_pixel(c, px, py, cc);
			}
		}
	}
}

/* --- themes -------------------------------------------------------------- */

/*
 * Colours follow a deep-indigo/teal accent scheme. Kept deliberately
 * saturated but not neon, with text/background contrast above WCAG AA for
 * body text in both modes.
 */
static const struct astrix_theme THEME_LIGHT = {
	.background = { 0xF2, 0xF4, 0xF8, 0xFF },
	.surface = { 0xFF, 0xFF, 0xFF, 0xFF },
	.surface_alt = { 0xE8, 0xEC, 0xF4, 0xFF },
	.primary = { 0x3B, 0x5B, 0xDB, 0xFF },
	.on_primary = { 0xFF, 0xFF, 0xFF, 0xFF },
	.text = { 0x14, 0x18, 0x22, 0xFF },
	.text_dim = { 0x5A, 0x63, 0x75, 0xFF },
	.divider = { 0xD6, 0xDC, 0xE8, 0xFF },
	.success = { 0x1B, 0x8A, 0x4B, 0xFF },
	.warning = { 0xB2, 0x6A, 0x00, 0xFF },
	.danger = { 0xC0, 0x2B, 0x2B, 0xFF },
	.overlay = { 0x00, 0x00, 0x00, 0x99 },
	.radius = 16,
	.spacing = 8,
	.touch_target = 48,
	.status_h = 28,
	.navbar_h = 56,
};

static const struct astrix_theme THEME_DARK = {
	.background = { 0x0E, 0x11, 0x18, 0xFF },
	.surface = { 0x1A, 0x1F, 0x2A, 0xFF },
	.surface_alt = { 0x24, 0x2B, 0x38, 0xFF },
	.primary = { 0x6E, 0x8B, 0xFF, 0xFF },
	.on_primary = { 0x0E, 0x11, 0x18, 0xFF },
	.text = { 0xF0, 0xF3, 0xF9, 0xFF },
	.text_dim = { 0x9A, 0xA4, 0xB8, 0xFF },
	.divider = { 0x2C, 0x34, 0x44, 0xFF },
	.success = { 0x35, 0xC4, 0x6E, 0xFF },
	.warning = { 0xE5, 0xA5, 0x2B, 0xFF },
	.danger = { 0xFF, 0x5C, 0x5C, 0xFF },
	.overlay = { 0x00, 0x00, 0x00, 0xCC },
	.radius = 16,
	.spacing = 8,
	.touch_target = 48,
	.status_h = 28,
	.navbar_h = 56,
};

const struct astrix_theme *astrix_theme_light(void) {
	return &THEME_LIGHT;
}

const struct astrix_theme *astrix_theme_dark(void) {
	return &THEME_DARK;
}

/* --- animation ----------------------------------------------------------- */

void astrix_anim_init(struct astrix_anim *a, float value) {
	if (!a) {
		return;
	}
	a->value = value;
	a->target = value;
	a->velocity = 0.0f;
	a->active = false;
}

void astrix_anim_set_target(struct astrix_anim *a, float target) {
	if (!a) {
		return;
	}
	a->target = target;
	a->active = true;
}

/*
 * Exponential smoothing with a fixed time constant. This is frame-rate
 * independent (unlike naive lerp-by-constant-fraction) and cannot overshoot,
 * which matters for gestures that can be cancelled mid-flight.
 */
bool astrix_anim_step(struct astrix_anim *a, float dt) {
	if (!a) {
		return false;
	}
	if (!a->active) {
		return false;
	}
	if (dt < 0.0f) {
		dt = 0.0f;
	}
	if (dt > 0.1f) {
		dt = 0.1f; /* clamp after a stall so animations do not jump */
	}
	const float tau = 0.12f; /* seconds to ~63% of the way */
	float alpha = 1.0f - expf(-dt / tau);
	a->value += (a->target - a->value) * alpha;
	if (fabsf(a->target - a->value) < 0.001f) {
		a->value = a->target;
		a->active = false;
		return false;
	}
	return true; /* still animating: caller should request another frame */
}

float astrix_ease_out_cubic(float t) {
	if (t < 0.0f) {
		t = 0.0f;
	}
	if (t > 1.0f) {
		t = 1.0f;
	}
	float f = 1.0f - t;
	return 1.0f - f * f * f;
}

float astrix_ease_in_out_cubic(float t) {
	if (t < 0.0f) {
		t = 0.0f;
	}
	if (t > 1.0f) {
		t = 1.0f;
	}
	if (t < 0.5f) {
		return 4.0f * t * t * t;
	}
	float f = -2.0f * t + 2.0f;
	return 1.0f - f * f * f / 2.0f;
}

float astrix_ease_out_back(float t) {
	if (t < 0.0f) {
		t = 0.0f;
	}
	if (t > 1.0f) {
		t = 1.0f;
	}
	const float c1 = 1.70158f, c3 = c1 + 1.0f;
	float f = t - 1.0f;
	return 1.0f + c3 * f * f * f + c1 * f * f;
}
