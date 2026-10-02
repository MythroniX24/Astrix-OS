/*
 * Astrix OS - Astrix UI toolkit
 *
 * A small, dependency-light widget/drawing layer used by the Astrix Shell and
 * the Astrix system applications. It is deliberately *not* a general UI
 * toolkit: it targets a phone form factor (portrait, touch-first) and keeps
 * the resident set small.
 *
 * Design notes
 * ------------
 *  - The toolkit renders into a caller-supplied ARGB8888 buffer. The compositor
 *    uploads it through wl_shm. Keeping our own buffer means the shell draws
 *    with plain memory writes and does not depend on a GL context being
 *    available, which matters on older ARM64 GPUs under QEMU.
 *  - Text is rendered from a built-in bitmap font (see astrix_font.c) so that
 *    the shell has no runtime dependency on fontconfig/FreeType at boot. The
 *    font is fixed-width, which is sufficient for a status bar and labels.
 *  - Everything is immediate-mode: the shell's state lives in structs, and
 *    each frame is redrawn. This keeps the code small and predictable, at the
 *    cost of redrawing on damage only, which the compositor already schedules.
 */

#ifndef ASTRIX_UI_H
#define ASTRIX_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- geometry ------------------------------------------------------------ */

struct astrix_rect {
	int x, y, w, h;
};

struct astrix_color {
	uint8_t r, g, b, a;
};

static inline struct astrix_color astrix_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	struct astrix_color c = { r, g, b, a };
	return c;
}

static inline bool astrix_rect_contains(struct astrix_rect r, int x, int y) {
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static inline struct astrix_rect astrix_rect_inset(struct astrix_rect r, int dx, int dy) {
	return (struct astrix_rect){ r.x + dx, r.y + dy, r.w - 2 * dx, r.h - 2 * dy };
}

/* --- theme --------------------------------------------------------------- */

/*
 * Colours are defined here rather than hard-coded at call sites so that a
 * light/dark switch is a single struct swap. Values are ARGB8888 to match the
 * buffer format directly.
 */
struct astrix_theme {
	struct astrix_color background;
	struct astrix_color surface;         /* cards, sheets            */
	struct astrix_color surface_alt;     /* elevated / hover         */
	struct astrix_color primary;
	struct astrix_color on_primary;
	struct astrix_color text;
	struct astrix_color text_dim;
	struct astrix_color divider;
	struct astrix_color success;
	struct astrix_color warning;
	struct astrix_color danger;
	struct astrix_color overlay;         /* scrim behind dialogs     */
	/* Numeric design tokens (see docs/DESIGN.md). */
	int radius;
	int spacing;      /* base unit, 4dp-equivalent */
	int touch_target; /* minimum tappable size     */
	int status_h;
	int navbar_h;
};

const struct astrix_theme *astrix_theme_light(void);
const struct astrix_theme *astrix_theme_dark(void);

/* --- canvas -------------------------------------------------------------- */

struct astrix_canvas {
	uint32_t *pixels;   /* ARGB8888, may be NULL during layout-only passes */
	int width;
	int height;
	int stride;         /* in pixels, not bytes */
};

/* Fill helpers operate on the pixel buffer and bounds-check everything. */
void astrix_fill_rect(struct astrix_canvas *c, struct astrix_rect r, struct astrix_color col);
void astrix_fill_rect_rounded(struct astrix_canvas *c, struct astrix_rect r, int radius,
                              struct astrix_color col);
void astrix_stroke_rect(struct astrix_canvas *c, struct astrix_rect r, int thickness,
                        struct astrix_color col);
void astrix_stroke_rect_rounded(struct astrix_canvas *c, struct astrix_rect r, int radius,
                                int thickness, struct astrix_color col);
/* Linear vertical gradient between two colours, used for wallpaper and sheets. */
void astrix_fill_rect_gradient_v(struct astrix_canvas *c, struct astrix_rect r,
                                struct astrix_color top, struct astrix_color bottom);
void astrix_blend_pixel(struct astrix_canvas *c, int x, int y, struct astrix_color col);
void astrix_clear(struct astrix_canvas *c, struct astrix_color col);

/* Colour utilities */
struct astrix_color astrix_color_lerp(struct astrix_color a, struct astrix_color b, float t);
struct astrix_color astrix_color_with_alpha(struct astrix_color c, uint8_t a);

/* --- text ---------------------------------------------------------------- */

/*
 * The built-in font is 6x11 with 1px spacing, giving a 6px advance. Glyphs
 * cover ASCII 0x20-0x7E which is all the shell needs for labels.
 */
#define ASTRIX_FONT_W 6
#define ASTRIX_FONT_H 11
#define ASTRIX_FONT_ADVANCE 6

/* Returns the advance width of the string in pixels. */
int astrix_text_width(const char *utf8);
/* Draws text at (x, y) as the top-left of the first glyph. Returns end x. */
int astrix_draw_text(struct astrix_canvas *c, int x, int y, const char *utf8,
                     struct astrix_color col);
/* Draws text centred horizontally within `r`. Returns the drawn width. */
int astrix_draw_text_centered(struct astrix_canvas *c, struct astrix_rect r, int baseline_y,
                              const char *utf8, struct astrix_color col);
/* Truncates into `out` adding an ellipsis so the result fits `max_w` pixels. */
void astrix_text_ellipsize(const char *utf8, int max_w, char *out, size_t out_size);
/* Greedy UTF-8 safe word wrap; returns the number of lines written. */
int astrix_text_wrap(const char *utf8, int max_w, char lines[][256], int max_lines);

/* --- input --------------------------------------------------------------- */

/*
 * Astrix is touch-first. A pointer event carries a normalised pressure-free
 * touch point; the shell's gesture recogniser turns a stream of these into
 * swipe/tap/long-press. Keyboard input is still supported (a phone keyboard
 * or a USB/BT keyboard) but is a secondary path.
 */
enum astrix_input_kind {
	ASTRIX_INPUT_TOUCH_DOWN,
	ASTRIX_INPUT_TOUCH_MOTION,
	ASTRIX_INPUT_TOUCH_UP,
	ASTRIX_INPUT_POINTER_MOTION,
	ASTRIX_INPUT_POINTER_BUTTON,
	ASTRIX_INPUT_SCROLL,
	ASTRIX_INPUT_KEY,
};

struct astrix_input_event {
	enum astrix_input_kind kind;
	int x, y;          /* pointer position in surface coordinates */
	int delta;         /* scroll: positive = away from user           */
	int scroll_x, scroll_y;
	double timestamp;  /* seconds, monotonic                      */
	uint32_t key;      /* ASTRIX_INPUT_KEY: evdev keycode         */
	uint32_t state;    /* evdev key state: bit 0 = pressed        */
	uint32_t button;   /* evdev button code                       */
};

#define ASTRIX_KEY_PRESSED 1u

/* --- animation ----------------------------------------------------------- */

/*
 * A tiny critically-damped-ish easing helper. Animations are driven from the
 * compositor frame loop rather than a timer thread, which keeps the system
 * free of extra wakeups when nothing is moving.
 */
struct astrix_anim {
	float value;
	float target;
	float velocity;
	bool  active;
};

void astrix_anim_init(struct astrix_anim *a, float value);
void astrix_anim_set_target(struct astrix_anim *a, float target);
bool astrix_anim_step(struct astrix_anim *a, float dt);

/* Easing functions */
float astrix_ease_out_cubic(float t);
float astrix_ease_in_out_cubic(float t);
float astrix_ease_out_back(float t);

#endif /* ASTRIX_UI_H */
