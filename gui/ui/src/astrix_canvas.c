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

/* ======================================================================== */
/* Depth, gradients, typography and glyphs                                  */
/*                                                                          */
/* Everything below exists because the shell looked like a build placeholder:*/
/* flat colour swatches stamped on a flat background, with letters standing  */
/* in for icons. Modern mobile UI is not "more colours", it is depth         */
/* (shadow/glass), a type hierarchy with weight, and shapes that read at a    */
/* glance. These are the primitives that produce those three things.         */
/* ======================================================================== */

struct astrix_color astrix_color_scale(struct astrix_color c, float factor) {
	struct astrix_color o = c;
	o.r = (uint8_t)(c.r * factor > 255.0f ? 255.0f : c.r * factor);
	o.g = (uint8_t)(c.g * factor > 255.0f ? 255.0f : c.g * factor);
	o.b = (uint8_t)(c.b * factor > 255.0f ? 255.0f : c.b * factor);
	return o;
}

struct astrix_color astrix_color_mix(struct astrix_color a, struct astrix_color b, float t) {
	return astrix_color_lerp(a, b, t);
}

/* --- soft shadow --------------------------------------------------------- */

void astrix_shadow_rounded(struct astrix_canvas *c, struct astrix_rect r, int radius,
                           int spread, int layers, struct astrix_color col) {
	if (!c || !c->pixels || layers <= 0 || spread <= 0) {
		return;
	}
	/*
	 * Outermost pass first, so inner (darker, tighter) passes land on top.
	 * Alpha falls off quadratically, which is roughly how light actually
	 * falls off and is what separates a shadow from a grey outline.
	 */
	for (int i = layers; i >= 1; i--) {
		float f = (float)i / (float)layers;
		int grow = (int)(spread * f);
		struct astrix_color pass = col;
		pass.a = (uint8_t)(col.a * (1.0f - f) * (1.0f - f));
		if (pass.a == 0) {
			continue;
		}
		struct astrix_rect rr = { r.x - grow, r.y - grow, r.w + 2 * grow, r.h + 2 * grow };
		int rad = radius + grow;
		if (rad * 2 > rr.h) {
			rad = rr.h / 2;
		}
		astrix_fill_rect_rounded(c, rr, rad, pass);
	}
}

/* --- diagonal gradient --------------------------------------------------- */

void astrix_fill_rect_gradient_diag(struct astrix_canvas *c, struct astrix_rect r,
                                    struct astrix_color from, struct astrix_color to) {
	if (!c || !c->pixels || r.w <= 0 || r.h <= 0) {
		return;
	}
	/*
	 * Diagonal lerp computed per pixel rather than per row: a per-row ramp
	 * is only correct for a pure vertical gradient, and the whole point of a
	 * diagonal is that the highlight sits in a corner.
	 */
	float span = (float)(r.w + r.h);
	for (int y = 0; y < r.h; y++) {
		for (int x = 0; x < r.w; x++) {
			float t = (span > 1.0f) ? (float)(x + y) / (span - 1.0f) : 0.0f;
			astrix_blend_pixel(c, r.x + x, r.y + y, astrix_color_lerp(from, to, t));
		}
	}
}

/* --- card ---------------------------------------------------------------- */

/*
 * A vertical gradient clipped to a rounded rect.
 *
 * astrix_fill_rect_gradient_v paints a square, which would square off the
 * corners of a card - the exact detail that makes a card look like a sticker
 * instead of a surface. So each row is clipped by the corner-circle equation
 * instead of relying on a later pass to redraw the corners, which cannot work:
 * the corner pixels already hold the wrong colour.
 */
static void fill_rounded_gradient_v(struct astrix_canvas *c, struct astrix_rect r, int radius,
                                    struct astrix_color top, struct astrix_color bottom) {
	if (!c || !c->pixels || r.w <= 0 || r.h <= 0) {
		return;
	}
	int rad = radius;
	if (rad * 2 > r.h) {
		rad = r.h / 2;
	}
	if (rad * 2 > r.w) {
		rad = r.w / 2;
	}
	for (int y = 0; y < r.h; y++) {
		/* dy is the distance from the nearer horizontal edge. */
		int dy = y < rad ? rad - y : y >= r.h - rad ? rad - (r.h - 1 - y) : 0;
		int inset = 0;
		if (dy > 0 && rad > 0) {
			float k = (float)(rad * rad - dy * dy);
			inset = rad - (int)(sqrtf(k > 0.0f ? k : 0.0f) + 0.5f);
			if (inset < 0) {
				inset = 0;
			}
		}
		float t = (r.h > 1) ? (float)y / (float)(r.h - 1) : 0.0f;
		struct astrix_color col = astrix_color_lerp(top, bottom, t);
		astrix_fill_rect(c, (struct astrix_rect){ r.x + inset, r.y + y,
		                                         r.w - 2 * inset, 1 },
		                 col);
	}
}

void astrix_draw_card(struct astrix_canvas *c, struct astrix_rect r, int radius,
                      struct astrix_color top, struct astrix_color bottom,
                      struct astrix_color shadow) {
	if (!c || !c->pixels) {
		return;
	}
	if (shadow.a > 0) {
		astrix_shadow_rounded(c, r, radius, r.h / 6 + 4, 5, shadow);
	}
	fill_rounded_gradient_v(c, r, radius, top, bottom);
}

/* --- glass --------------------------------------------------------------- */

void astrix_fill_glass(struct astrix_canvas *c, struct astrix_rect r, int radius,
                       struct astrix_color tint, uint8_t alpha) {
	if (!c || !c->pixels) {
		return;
	}
	astrix_fill_rect_rounded(c, r, radius, astrix_color_with_alpha(tint, alpha));
	/*
	 * The top hairline is the whole trick. Backdrop blur is not available
	 * without a GPU, but a bright edge along the top of a translucent
	 * surface is the cue the eye uses to read "this is above that", and it
	 * costs one row of pixels.
	 */
	int hx = r.x + radius;
	int hw = r.w - 2 * radius;
	if (hw > 0) {
		astrix_fill_rect(c, (struct astrix_rect){ hx, r.y + 1, hw, 1 },
		                 astrix_color_with_alpha(astrix_color_scale(tint, 1.35f), 0x70));
	}
}

/* --- typography ---------------------------------------------------------- */

int astrix_draw_text_bold(struct astrix_canvas *c, int x, int y, const char *utf8,
                          struct astrix_color col) {
	if (!utf8) {
		return x;
	}
	int end = astrix_draw_text(c, x, y, utf8, col);
	/* 1px right + 1px down is a smear at 6x11; anything more turns to mud. */
	astrix_draw_text(c, x + 1, y, utf8, col);
	return end;
}

int astrix_text_width_tracked(const char *utf8, int tracking) {
	int n = astrix_text_width(utf8);
	int chars = 0;
	for (const char *p = utf8; p && *p; p++) {
		if ((*p & 0xC0) != 0x80) {
			chars++;
		}
	}
	if (chars > 1) {
		n += tracking * (chars - 1);
	}
	return n;
}

int astrix_draw_text_tracked(struct astrix_canvas *c, int x, int y, const char *utf8,
                             struct astrix_color col, int tracking) {
	if (!utf8) {
		return x;
	}
	int cx = x;
	/* The font is byte-indexed and ASCII-only, so decode just enough to know
	 * where each glyph starts. */
	for (const unsigned char *p = (const unsigned char *)utf8; *p;) {
		int adv = ASTRIX_FONT_ADVANCE;
		if (*p >= 0xF0) {
			adv = ASTRIX_FONT_ADVANCE * 2;
			p += 4;
		} else if (*p >= 0xE0) {
			adv = ASTRIX_FONT_ADVANCE;
			p += 3;
		} else if (*p >= 0xC0) {
			adv = ASTRIX_FONT_ADVANCE;
			p += 2;
		} else {
			p++;
		}
		cx += adv + tracking;
	}
	return cx;
}

/* --- glyphs -------------------------------------------------------------- */

/*
 * Icons as signed distance fields.
 *
 * Each glyph is a function from a unit square to a distance: negative inside,
 * positive outside. That buys three things a hand-plotted rectangle cannot:
 * correct antialiasing from the sign alone (coverage = clamp(0.5 - d)), free
 * scaling to any panel density, and composition - union is min(), and a hole
 * is max(d, -d_hole) - so a gear or a crescent is a subtraction rather than a
 * special case.
 *
 * u,v are in [0,1] with v pointing down. Stroke half-widths are folded into
 * the distances, so callers only pick a size and a colour.
 */
static float gsd_circle(float u, float v, float cx, float cy, float r) {
	float dx = u - cx, dy = v - cy;
	return sqrtf(dx * dx + dy * dy) - r;
}

static float gsd_box(float u, float v, float cx, float cy, float hw, float hh, float r) {
	float qx = fabsf(u - cx) - (hw - r);
	float qy = fabsf(v - cy) - (hh - r);
	float ax = qx > 0.0f ? qx : 0.0f;
	float ay = qy > 0.0f ? qy : 0.0f;
	float outside = sqrtf(ax * ax + ay * ay);
	float inside = (qx > qy ? qx : qy);
	if (inside > 0.0f) {
		inside = 0.0f;
	}
	return outside + inside - r;
}

static float gsd_seg(float u, float v, float ax, float ay, float bx, float by, float w) {
	float pax = u - ax, pay = v - ay;
	float bax = bx - ax, bay = by - ay;
	float len2 = bax * bax + bay * bay;
	float h = len2 > 0.0f ? (pax * bax + pay * bay) / len2 : 0.0f;
	h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
	float dx = pax - bax * h, dy = pay - bay * h;
	return sqrtf(dx * dx + dy * dy) - w;
}

/* Outline of a rounded box: a ring around the box itself. */
static float gsd_box_outline(float u, float v, float cx, float cy, float hw, float hh,
                             float r, float w) {
	return fabsf(gsd_box(u, v, cx, cy, hw, hh, r)) - w * 0.5f;
}

static float gsd_ellipse(float u, float v, float cx, float cy, float rx, float ry) {
	float dx = (u - cx) / rx, dy = (v - cy) / ry;
	float d = sqrtf(dx * dx + dy * dy) - 1.0f;
	return d * rx;
}

static float glyph_sdf(enum astrix_glyph g, float u, float v) {
	switch (g) {
	case ASTRIX_GLYPH_HOME: {
		float roof = gsd_seg(u, v, 0.08f, 0.48f, 0.50f, 0.12f, 0.045f);
		roof = fminf(roof, gsd_seg(u, v, 0.50f, 0.12f, 0.92f, 0.48f, 0.045f));
		float body = gsd_box_outline(u, v, 0.50f, 0.68f, 0.30f, 0.26f, 0.05f, 0.05f);
		float door = gsd_box(u, v, 0.50f, 0.82f, 0.08f, 0.14f, 0.0f);
		float d = fminf(roof, body);
		/* Punch the doorway so the house reads as a house, not a box. */
		return fmaxf(d, -door);
	}
	case ASTRIX_GLYPH_FOLDER: {
		float tab = gsd_box(u, v, 0.30f, 0.30f, 0.17f, 0.06f, 0.03f);
		float body = gsd_box(u, v, 0.50f, 0.58f, 0.42f, 0.28f, 0.06f);
		return fminf(tab, body);
	}
	case ASTRIX_GLYPH_TERMINAL: {
		float frame = gsd_box_outline(u, v, 0.50f, 0.50f, 0.46f, 0.34f, 0.08f, 0.055f);
		float chev = gsd_seg(u, v, 0.28f, 0.36f, 0.44f, 0.52f, 0.042f);
		chev = fminf(chev, gsd_seg(u, v, 0.44f, 0.52f, 0.28f, 0.68f, 0.042f));
		float line = gsd_seg(u, v, 0.48f, 0.70f, 0.74f, 0.70f, 0.038f);
		return fminf(frame, fminf(chev, line));
	}
	case ASTRIX_GLYPH_SETTINGS: {
		float ring = fabsf(gsd_circle(u, v, 0.50f, 0.50f, 0.26f)) - 0.035f;
		float hub = gsd_circle(u, v, 0.50f, 0.50f, 0.09f);
		float d = fminf(ring, hub);
		/* Eight teeth: an axial cross plus the diagonals. */
		static const float ax[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		static const float ay[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
		for (int i = 0; i < 4; i++) {
			float cx = 0.50f + (ax[i] - 0.5f) * 0.72f;
			float cy = 0.50f + (ay[i] - 0.5f) * 0.72f;
			float nx = 0.50f + (ax[i] - 0.5f) * 0.42f;
			float ny = 0.50f + (ay[i] - 0.5f) * 0.42f;
			d = fminf(d, gsd_seg(u, v, cx, cy, nx, ny, 0.045f));
			/* Diagonal tooth, at 45 degrees. */
			float sgnx = (ax[i] == ay[i]) ? 1.0f : -1.0f;
			float ox = 0.50f + sgnx * 0.255f, oy = 0.50f + sgnx * 0.255f;
			float ix = 0.50f + sgnx * 0.148f, iy = 0.50f + sgnx * 0.148f;
			d = fminf(d, gsd_seg(u, v, ox, oy, ix, iy, 0.042f));
		}
		return d;
	}
	case ASTRIX_GLYPH_STORE: {
		float bag = gsd_box(u, v, 0.50f, 0.64f, 0.32f, 0.24f, 0.05f);
		float handle = gsd_seg(u, v, 0.38f, 0.42f, 0.42f, 0.26f, 0.036f);
		handle = fminf(handle, gsd_seg(u, v, 0.42f, 0.26f, 0.58f, 0.26f, 0.036f));
		handle = fminf(handle, gsd_seg(u, v, 0.58f, 0.26f, 0.62f, 0.42f, 0.036f));
		return fminf(bag, handle);
	}
	case ASTRIX_GLYPH_ANDROID: {
		/* Head: the top half of a disc. */
		float head = fmaxf(gsd_circle(u, v, 0.50f, 0.52f, 0.32f), v - 0.52f);
		float body = gsd_box(u, v, 0.50f, 0.70f, 0.30f, 0.16f, 0.03f);
		float ant1 = gsd_seg(u, v, 0.36f, 0.28f, 0.28f, 0.12f, 0.022f);
		float ant2 = gsd_seg(u, v, 0.64f, 0.28f, 0.72f, 0.12f, 0.022f);
		float d = fminf(fminf(head, body), fminf(ant1, ant2));
		/* Eyes are holes: subtract, do not draw over. */
		float eye = fminf(gsd_circle(u, v, 0.40f, 0.44f, 0.045f),
		                  gsd_circle(u, v, 0.60f, 0.44f, 0.045f));
		return fmaxf(d, -eye);
	}
	case ASTRIX_GLYPH_INFO: {
		float ring = fabsf(gsd_circle(u, v, 0.50f, 0.30f, 0.24f)) - 0.032f;
		float stem = gsd_box(u, v, 0.50f, 0.66f, 0.038f, 0.16f, 0.0f);
		float dot = gsd_circle(u, v, 0.50f, 0.42f, 0.05f);
		return fminf(ring, fminf(stem, dot));
	}
	case ASTRIX_GLYPH_GRID: {
		float d = 1e9f;
		static const float cxs[4] = { 0.29f, 0.71f, 0.29f, 0.71f };
		static const float cys[4] = { 0.29f, 0.29f, 0.71f, 0.71f };
		for (int i = 0; i < 4; i++) {
			d = fminf(d, gsd_box(u, v, cxs[i], cys[i], 0.19f, 0.19f, 0.06f));
		}
		return d;
	}
	case ASTRIX_GLYPH_BACK: {
		float chev = gsd_seg(u, v, 0.62f, 0.16f, 0.34f, 0.50f, 0.055f);
		chev = fminf(chev, gsd_seg(u, v, 0.34f, 0.50f, 0.62f, 0.84f, 0.055f));
		float stem = gsd_seg(u, v, 0.36f, 0.50f, 0.80f, 0.50f, 0.055f);
		return fminf(chev, stem);
	}
	case ASTRIX_GLYPH_POWER: {
		float ring = fabsf(gsd_circle(u, v, 0.50f, 0.54f, 0.28f)) - 0.035f;
		/* Cut the top of the ring so the stem can stand in the gap. */
		float gap = gsd_box(u, v, 0.50f, 0.02f, 0.11f, 0.16f, 0.0f);
		ring = fmaxf(ring, -gap);
		float stem = gsd_seg(u, v, 0.50f, 0.16f, 0.50f, 0.52f, 0.06f);
		return fminf(ring, stem);
	}
	case ASTRIX_GLYPH_BATTERY: {
		float frame = gsd_box_outline(u, v, 0.44f, 0.50f, 0.34f, 0.22f, 0.05f, 0.05f);
		float nub = gsd_box(u, v, 0.82f, 0.50f, 0.045f, 0.07f, 0.0f);
		float fill = gsd_box(u, v, 0.30f, 0.50f, 0.16f, 0.12f, 0.02f);
		return fminf(frame, fminf(nub, fill));
	}
	case ASTRIX_GLYPH_WIFI: {
		/*
		 * Arcs as short polylines rather than a clipped ring. Clipping a ring
		 * by two half-planes is the clever way and it is also the way that
		 * quietly produces broken stubs at small sizes; eleven segments per
		 * arc is indistinguishable at any icon size this OS draws and cannot
		 * come out wrong.
		 */
		float d = gsd_circle(u, v, 0.50f, 0.72f, 0.085f);
		static const float radii[3] = { 0.20f, 0.36f, 0.52f };
		static const float half_w[3] = { 0.034f, 0.036f, 0.038f };
		for (int i = 0; i < 3; i++) {
			float px = 0.0f, py = 0.0f;
			for (int s = 0; s <= 10; s++) {
				float a = (200.0f + 140.0f * (float)s / 10.0f) * 3.14159265f / 180.0f;
				float ax = 0.50f + radii[i] * cosf(a);
				float ay = 0.72f + radii[i] * sinf(a);
				if (s > 0) {
					d = fminf(d, gsd_seg(u, v, px, py, ax, ay, half_w[i]));
				}
				px = ax;
				py = ay;
			}
		}
		return d;
	}
	case ASTRIX_GLYPH_KEYBOARD: {
		float frame = gsd_box_outline(u, v, 0.50f, 0.52f, 0.42f, 0.26f, 0.06f, 0.05f);
		float dots = 1e9f;
		for (int i = 0; i < 3; i++) {
			dots = fminf(dots, gsd_circle(u, v, 0.34f + 0.16f * i, 0.44f, 0.042f));
		}
		float bar = gsd_box(u, v, 0.50f, 0.66f, 0.20f, 0.030f, 0.0f);
		return fminf(frame, fminf(dots, bar));
	}
	case ASTRIX_GLYPH_CLOSE: {
		float a = gsd_seg(u, v, 0.26f, 0.26f, 0.74f, 0.74f, 0.062f);
		float b = gsd_seg(u, v, 0.74f, 0.26f, 0.26f, 0.74f, 0.062f);
		return fminf(a, b);
	}
	case ASTRIX_GLYPH_SEARCH: {
		float ring = fabsf(gsd_circle(u, v, 0.44f, 0.44f, 0.24f)) - 0.034f;
		float handle = gsd_seg(u, v, 0.62f, 0.62f, 0.82f, 0.82f, 0.075f);
		return fminf(ring, handle);
	}
	case ASTRIX_GLYPH_MOON: {
		float outer = gsd_circle(u, v, 0.52f, 0.46f, 0.32f);
		float bite = gsd_circle(u, v, 0.68f, 0.34f, 0.28f);
		return fmaxf(outer, -bite);
	}
	case ASTRIX_GLYPH_GLOBE: {
		float ring = fabsf(gsd_circle(u, v, 0.50f, 0.50f, 0.34f)) - 0.030f;
		float meridian = fabsf(gsd_ellipse(u, v, 0.50f, 0.50f, 0.15f, 0.34f)) - 0.026f;
		float equator = gsd_seg(u, v, 0.16f, 0.50f, 0.84f, 0.50f, 0.038f);
		return fminf(ring, fminf(meridian, equator));
	}
	case ASTRIX_GLYPH_MEMORY: {
		float frame = gsd_box_outline(u, v, 0.50f, 0.54f, 0.30f, 0.22f, 0.03f, 0.05f);
		float d = frame;
		for (int i = 0; i < 4; i++) {
			float x = 0.30f + 0.13f * i;
			d = fminf(d, gsd_seg(u, v, x, 0.22f, x, 0.32f, 0.030f));
			d = fminf(d, gsd_seg(u, v, x, 0.76f, x, 0.86f, 0.030f));
		}
		return d;
	}
	case ASTRIX_GLYPH_CHECK: {
		float short_arm = gsd_seg(u, v, 0.22f, 0.52f, 0.42f, 0.72f, 0.070f);
		float long_arm = gsd_seg(u, v, 0.42f, 0.72f, 0.80f, 0.28f, 0.070f);
		return fminf(short_arm, long_arm);
	}
	case ASTRIX_GLYPH_COUNT:
	default:
		return 1e9f;
	}
}

void astrix_draw_glyph(struct astrix_canvas *c, struct astrix_rect r, enum astrix_glyph g,
                       struct astrix_color col) {
	if (!c || !c->pixels || g >= ASTRIX_GLYPH_COUNT) {
		return;
	}
	int size = r.w < r.h ? r.w : r.h;
	if (size <= 2) {
		return;
	}
	int ox = r.x + (r.w - size) / 2;
	int oy = r.y + (r.h - size) / 2;
	for (int y = 0; y < size; y++) {
		float v = ((float)y + 0.5f) / (float)size;
		for (int x = 0; x < size; x++) {
			float u = ((float)x + 0.5f) / (float)size;
			/* Unit-space distance scaled to pixels gives coverage directly:
			 * inside by half a pixel is fully opaque, outside is not drawn. */
			float d = glyph_sdf(g, u, v) * (float)size;
			if (d >= 0.5f) {
				continue;
			}
			float cov = 0.5f - d;
			if (cov <= 0.0f) {
				continue;
			}
			struct astrix_color px = col;
			px.a = (uint8_t)((float)col.a * (cov > 1.0f ? 1.0f : cov));
			astrix_blend_pixel(c, ox + x, oy + y, px);
		}
	}
}
