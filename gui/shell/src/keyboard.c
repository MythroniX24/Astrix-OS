/*
 * Astrix OS - system on-screen keyboard.
 *
 * Layout, hit-testing and the key queue. No Wayland, no drawing, no I/O: the
 * whole file is pure logic over the shell model so the host test suite can
 * drive it directly ("tap the key at 30,600, expect 'q'") without a
 * compositor, a keymap or an application to type into.
 *
 * The rendering lives in render.c and the delivery in main.c. Splitting it
 * that way is what makes this testable; a keyboard that only exists as a
 * drawing is a keyboard nobody can verify.
 */

#include "keyboard.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * The layout is five rows:
 *
 *   row 0   1..0 - = /   \   (numbers, always visible)
 *   row 1   q w e r t y u i o p
 *   row 2   a s d f g h j k l
 *   row 3   z x c v b n m
 *   row 4   shift  symbols  backspace  space  enter  hide
 *
 * Ten columns for rows 1-3 and a bottom row of weighted keys. Weighted widths
 * are why each key carries width_units: "space" is four units wide and "hide"
 * one, which is what makes the row look like a keyboard instead of a grid.
 */

static const char *const ROW_NUMBERS = "1234567890-=";
static const char *const ROW_QWERTY   = "qwertyuiop";
static const char *const ROW_ASDF     = "asdfghjkl";
static const char *const ROW_ZXCV     = "zxcvbnm";

static const char *const SYM_NUMBERS = "!@#$%^&*()_+";
static const char *const SYM_QWERTY   = "[]{}()\\|;:\"'";
static const char *const SYM_ASDF     = "~`<>.,?/$%^";
static const char *const SYM_ZXCV     = "";

/* The bottom row: label, action, weight. */
struct mod_key {
	const char *label;
	enum astrix_kbd_action action;
	int units;
};

static const struct mod_key MOD_ROW[] = {
	{ "shift",  ASTRIX_KBD_SHIFT,     2 },
	{ "?123",   ASTRIX_KBD_SYMBOLS,   2 },
	{ "bksp",   ASTRIX_KBD_BACKSPACE, 2 },
	{ "space",  ASTRIX_KBD_SPACE,     4 },
	{ "return", ASTRIX_KBD_ENTER,     2 },
	{ "hide",   ASTRIX_KBD_HIDE,      2 },
};
#define MOD_ROW_LEN ((int)(sizeof(MOD_ROW) / sizeof(MOD_ROW[0])))

/*
 * How much of the screen the keyboard takes. A third is enough for four rows
 * plus the number row on a 5-6.5" panel and leaves most of the screen for the
 * thing being typed into. On a very wide screen (the 1024x768 QEMU panel) it
 * would look absurd, so it is also capped by an absolute height.
 */
struct astrix_rect astrix_kbd_area(int width, int height) {
	/*
	 * A third of the screen, bounded below by something tappable and above
	 * by half the panel.
	 *
	 * An absolute pixel cap was wrong here: it made the keyboard look
	 * right on the 1024x768 QEMU panel and absurdly small on a real
	 * 1080x2400 phone, because the keyboard has to scale with the device.
	 * Half the panel is the cap because past that the keyboard stops
	 * being something you look past.
	 */
	int h = height / 3;
	if (h > height / 2) {
		h = height / 2;
	}
	if (h < 140) {
		h = 140;
	}
	return (struct astrix_rect){ 0, height - h, width, h };
}

static int row_count(const struct astrix_shell *sh) {
	/* 5 rows: numbers, three letter rows, modifiers. */
	(void)sh;
	return 5;
}

static const char *row_text(const struct astrix_shell *sh, int row) {
	bool sym = sh && sh->kbd_symbols;
	switch (row) {
	case 0: return sym ? SYM_NUMBERS : ROW_NUMBERS;
	case 1: return sym ? SYM_QWERTY : ROW_QWERTY;
	case 2: return sym ? SYM_ASDF : ROW_ASDF;
	case 3: return sym ? SYM_ZXCV : ROW_ZXCV;
	default: return NULL;
	}
}

static int row_len(const struct astrix_shell *sh, int row) {
	const char *t = row_text(sh, row);
	return t ? (int)strlen(t) : 0;
}

/*
 * Apply shift to a label. With the symbol layer up, shift selects the upper
 * half of each symbol pair instead of upper-casing, which is how a phone
 * keyboard reaches the second character of a key.
 */
static uint32_t apply_case(const struct astrix_shell *sh, const char *t, int i) {
	char c = t[i];
	bool upper = sh->kbd_caps ||
	             (sh->kbd_shift && !sh->kbd_symbols);
	if (sh->kbd_symbols) {
		/* Shift picks the pair above: digits' symbols, letters' second. */
		static const char *const UPPER_NUM = "!@#$%^&*(";
		if (i < 10 && !upper) {
			c = UPPER_NUM[i];
		}
	}
	if (upper && c >= 'a' && c <= 'z') {
		c = (char)toupper((unsigned char)c);
	}
	return (uint32_t)(unsigned char)c;
}

int astrix_kbd_layout(const struct astrix_shell *sh, int width, int height,
                      struct astrix_kbd_key *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	struct astrix_rect area = astrix_kbd_area(width, height);
	int rows = row_count(sh);
	int pad = width / 100 + 2;
	int gap = pad;
	int row_h = (area.h - gap * (rows + 1)) / rows;
	if (row_h < 12) {
		row_h = 12;
	}
	int n = 0;

	for (int r = 0; r < rows && n < max; r++) {
		int y = area.y + gap + r * (row_h + gap);

		if (r == rows - 1) {
			/* Modifier row: weighted keys across the same width. */
			int total = 0;
			for (int i = 0; i < MOD_ROW_LEN; i++) {
				total += MOD_ROW[i].units;
			}
			int x = gap;
			for (int i = 0; i < MOD_ROW_LEN && n < max; i++) {
				int w = (width - 2 * gap) * MOD_ROW[i].units / total;
				out[n].rect = (struct astrix_rect){ x, y, w - gap, row_h };
				out[n].action = MOD_ROW[i].action;
				out[n].codepoint = 0;
				out[n].width_units = MOD_ROW[i].units;
				snprintf(out[n].label, sizeof(out[n].label), "%s", MOD_ROW[i].label);
				x += w;
				n++;
			}
			continue;
		}

		const char *t = row_text(sh, r);
		int len = row_len(sh, r);
		if (len <= 0) {
			/* A layer can leave a row empty (the symbol layer has no
			 * zxcvbnm row). Dividing by its length would be a SIGFPE,
			 * so the row is simply not drawn - which is also what it
			 * looks like on a real keyboard in that state. */
			continue;
		}
		int cw = (width - 2 * gap) / len;
		int x = gap;
		for (int i = 0; i < len && n < max; i++) {
			out[n].rect = (struct astrix_rect){ x, y, cw - gap, row_h };
			out[n].action = ASTRIX_KBD_CHAR;
			out[n].codepoint = apply_case(sh, t, i);
			out[n].width_units = 1;
			out[n].label[0] = (char)out[n].codepoint;
			out[n].label[1] = '\0';
			x += cw;
			n++;
		}
	}
	return n;
}

int astrix_kbd_hit(const struct astrix_shell *sh, int width, int height,
                   int x, int y, struct astrix_kbd_key *out) {
	struct astrix_kbd_key keys[ASTRIX_KBD_MAX_KEYS];
	int n = astrix_kbd_layout(sh, width, height, keys, ASTRIX_KBD_MAX_KEYS);
	/* Back to front: the modifier row is last, so it wins an overlap. */
	for (int i = n - 1; i >= 0; i--) {
		if (astrix_rect_contains(keys[i].rect, x, y)) {
			if (out) {
				*out = keys[i];
			}
			return i;
		}
	}
	return -1;
}

bool astrix_kbd_contains(const struct astrix_shell *sh, int width, int height,
                         int x, int y) {
	struct astrix_rect area = astrix_kbd_area(width, height);
	if (!astrix_rect_contains(area, x, y)) {
		return false;
	}
	/* The gap above the top row belongs to the keyboard too, so a tap that
	 * lands in it closes the keyboard instead of hitting the app behind. */
	return true;
}

const char *astrix_kbd_label(const struct astrix_shell *sh, int row, int col) {
	static char label[8];
	const char *t = row_text(sh, row);
	if (!t || col < 0 || col >= (int)strlen(t)) {
		return "";
	}
	label[0] = (char)apply_case(sh, t, col);
	label[1] = '\0';
	return label;
}

static void queue_push(struct astrix_shell *sh, enum astrix_kbd_action action,
                       uint32_t codepoint) {
	if (sh->kbd_queue_len >= ASTRIX_KBD_QUEUE_LEN) {
		/* Drop the oldest rather than refusing the newest: the user's most
		 * recent key is the one they are waiting to see. */
		memmove(&sh->kbd_queue[0], &sh->kbd_queue[1],
		        sizeof(sh->kbd_queue[0]) * (ASTRIX_KBD_QUEUE_LEN - 1));
		sh->kbd_queue_len = ASTRIX_KBD_QUEUE_LEN - 1;
	}
	sh->kbd_queue[sh->kbd_queue_len].action = action;
	sh->kbd_queue[sh->kbd_queue_len].codepoint = codepoint;
	sh->kbd_queue_len++;
}

bool astrix_kbd_pop(struct astrix_shell *sh, enum astrix_kbd_action *action,
                    uint32_t *codepoint) {
	if (!sh || sh->kbd_queue_len == 0) {
		return false;
	}
	if (action) {
		*action = sh->kbd_queue[0].action;
	}
	if (codepoint) {
		*codepoint = sh->kbd_queue[0].codepoint;
	}
	memmove(&sh->kbd_queue[0], &sh->kbd_queue[1],
	        sizeof(sh->kbd_queue[0]) * (size_t)(sh->kbd_queue_len - 1));
	sh->kbd_queue_len--;
	return true;
}

struct astrix_rect astrix_kbd_toggle_rect(const struct astrix_shell *sh) {
	/* Sits to the right of the clock, which is where a phone puts its only
	 * always-visible control. The width is scaled from the status bar height
	 * so it stays a legal touch target at any panel size. */
	int w = sh->theme->status_h;
	int x = 12 + astrix_text_width(sh->status_time) + 14;
	return (struct astrix_rect){ x, 2, w, sh->theme->status_h - 4 };
}

bool astrix_kbd_toggle(struct astrix_shell *sh, int x, int y) {
	if (!sh) {
		return false;
	}
	if (!astrix_rect_contains(astrix_kbd_toggle_rect(sh), x, y)) {
		return false;
	}
	sh->kbd_visible = !sh->kbd_visible;
	if (!sh->kbd_visible) {
		sh->kbd_press_x = -1;
		sh->kbd_press_y = -1;
	}
	sh->needs_redraw = true;
	return true;
}

bool astrix_kbd_tap(struct astrix_shell *sh, int x, int y) {
	if (!sh || !sh->kbd_visible) {
		return false;
	}
	if (!astrix_kbd_contains(sh, sh->width, sh->height, x, y)) {
		return false;
	}
	struct astrix_kbd_key key;
	int idx = astrix_kbd_hit(sh, sh->width, sh->height, x, y, &key);
	sh->kbd_press_x = idx >= 0 ? x : -1;
	sh->kbd_press_y = idx >= 0 ? y : -1;
	if (idx < 0) {
		/* Inside the keyboard but in the gap: swallow the tap. */
		sh->needs_redraw = true;
		return true;
	}

	switch (key.action) {
	case ASTRIX_KBD_SHIFT:
		if (sh->kbd_caps) {
			sh->kbd_caps = false;
			sh->kbd_shift = false;
		} else if (sh->kbd_shift) {
			sh->kbd_shift = false;
			sh->kbd_caps = true;
		} else {
			sh->kbd_shift = true;
		}
		break;
	case ASTRIX_KBD_SYMBOLS:
		sh->kbd_symbols = !sh->kbd_symbols;
		break;
	case ASTRIX_KBD_HIDE:
		sh->kbd_visible = false;
		sh->kbd_press_x = -1;
		sh->kbd_press_y = -1;
		break;
	case ASTRIX_KBD_BACKSPACE:
	case ASTRIX_KBD_ENTER:
	case ASTRIX_KBD_SPACE:
		queue_push(sh, key.action, 0);
		break;
	case ASTRIX_KBD_CHAR:
	default:
		queue_push(sh, ASTRIX_KBD_CHAR, key.codepoint);
		/* One-shot shift: a real keyboard does not stay shifted after one
		 * letter, and a phone keyboard that does is unusable. */
		if (sh->kbd_shift && !sh->kbd_caps) {
			sh->kbd_shift = false;
		}
		break;
	}
	sh->needs_redraw = true;
	return true;
}