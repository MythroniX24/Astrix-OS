/*
 * Astrix OS - on-screen touch keyboard layout.
 *
 * Layout only; astrix_term's UI code does the drawing and the hit-testing.
 */

#include "astrix_kbd.h"

#include <stddef.h>

/*
 * Row 0: numbers        Row 1: qwerty         Row 2: asdf
 * Row 3: zxcvb + comma  Row 4: modifiers + space + enter
 *
 * Layer 1 is the shifted/symbols layer, reached with the ^ key. It is what
 * makes "(" and "$" reachable without a hardware keyboard.
 */
static const struct astrix_kbd_key row0[] = {
	{ "1", ASTRIX_KBD_CHAR, '1', 1, 0 },  { "2", ASTRIX_KBD_CHAR, '2', 1, 0 },
	{ "3", ASTRIX_KBD_CHAR, '3', 1, 0 },  { "4", ASTRIX_KBD_CHAR, '4', 1, 0 },
	{ "5", ASTRIX_KBD_CHAR, '5', 1, 0 },  { "6", ASTRIX_KBD_CHAR, '6', 1, 0 },
	{ "7", ASTRIX_KBD_CHAR, '7', 1, 0 },  { "8", ASTRIX_KBD_CHAR, '8', 1, 0 },
	{ "9", ASTRIX_KBD_CHAR, '9', 1, 0 },  { "0", ASTRIX_KBD_CHAR, '0', 1, 0 },
	{ "-", ASTRIX_KBD_CHAR, '-', 1, 0 },  { "/", ASTRIX_KBD_SLASH, '/', 1, 0 },
	{ "@", ASTRIX_KBD_AT, '@', 1, 0 },    { "|", ASTRIX_KBD_PIPE, '|', 1, 0 },
};
static const struct astrix_kbd_key row0_hi[] = {
	{ "!", ASTRIX_KBD_CHAR, '!', 1, 1 }, { "@", ASTRIX_KBD_CHAR, '@', 1, 1 },
	{ "#", ASTRIX_KBD_CHAR, '#', 1, 1 }, { "$", ASTRIX_KBD_CHAR, '$', 1, 1 },
	{ "%", ASTRIX_KBD_CHAR, '%', 1, 1 }, { "^", ASTRIX_KBD_CHAR, '^', 1, 1 },
	{ "&", ASTRIX_KBD_CHAR, '&', 1, 1 }, { "*", ASTRIX_KBD_CHAR, '*', 1, 1 },
	{ "(", ASTRIX_KBD_CHAR, '(', 1, 1 }, { ")", ASTRIX_KBD_CHAR, ')', 1, 1 },
	{ "_", ASTRIX_KBD_CHAR, '_', 1, 1 }, { "+", ASTRIX_KBD_CHAR, '+', 1, 1 },
	{ "{", ASTRIX_KBD_CHAR, '{', 1, 1 }, { "}", ASTRIX_KBD_CHAR, '}', 1, 1 },
};

static const struct astrix_kbd_key row1[] = {
	{ "q", ASTRIX_KBD_CHAR, 'q', 1, 0 }, { "w", ASTRIX_KBD_CHAR, 'w', 1, 0 },
	{ "e", ASTRIX_KBD_CHAR, 'e', 1, 0 }, { "r", ASTRIX_KBD_CHAR, 'r', 1, 0 },
	{ "t", ASTRIX_KBD_CHAR, 't', 1, 0 }, { "y", ASTRIX_KBD_CHAR, 'y', 1, 0 },
	{ "u", ASTRIX_KBD_CHAR, 'u', 1, 0 }, { "i", ASTRIX_KBD_CHAR, 'i', 1, 0 },
	{ "o", ASTRIX_KBD_CHAR, 'o', 1, 0 }, { "p", ASTRIX_KBD_CHAR, 'p', 1, 0 },
	{ "~", ASTRIX_KBD_TILDE, '~', 1, 0 },
};
static const struct astrix_kbd_key row1_hi[] = {
	{ "Q", ASTRIX_KBD_CHAR, 'Q', 1, 1 }, { "W", ASTRIX_KBD_CHAR, 'W', 1, 1 },
	{ "E", ASTRIX_KBD_CHAR, 'E', 1, 1 }, { "R", ASTRIX_KBD_CHAR, 'R', 1, 1 },
	{ "T", ASTRIX_KBD_CHAR, 'T', 1, 1 }, { "Y", ASTRIX_KBD_CHAR, 'Y', 1, 1 },
	{ "U", ASTRIX_KBD_CHAR, 'U', 1, 1 }, { "I", ASTRIX_KBD_CHAR, 'I', 1, 1 },
	{ "O", ASTRIX_KBD_CHAR, 'O', 1, 1 }, { "P", ASTRIX_KBD_CHAR, 'P', 1, 1 },
	{ "^", ASTRIX_KBD_CTRL, 0, 1.4f, 1 },
	{ "\x7e", ASTRIX_KBD_TILDE, '~', 1, 1 },
};

static const struct astrix_kbd_key row2[] = {
	{ "a", ASTRIX_KBD_CHAR, 'a', 1, 0 }, { "s", ASTRIX_KBD_CHAR, 's', 1, 0 },
	{ "d", ASTRIX_KBD_CHAR, 'd', 1, 0 }, { "f", ASTRIX_KBD_CHAR, 'f', 1, 0 },
	{ "g", ASTRIX_KBD_CHAR, 'g', 1, 0 }, { "h", ASTRIX_KBD_CHAR, 'h', 1, 0 },
	{ "j", ASTRIX_KBD_CHAR, 'j', 1, 0 }, { "k", ASTRIX_KBD_CHAR, 'k', 1, 0 },
	{ "l", ASTRIX_KBD_CHAR, 'l', 1, 0 },
};
static const struct astrix_kbd_key row2_hi[] = {
	{ "A", ASTRIX_KBD_CHAR, 'A', 1, 1 }, { "S", ASTRIX_KBD_CHAR, 'S', 1, 1 },
	{ "D", ASTRIX_KBD_CHAR, 'D', 1, 1 }, { "F", ASTRIX_KBD_CHAR, 'F', 1, 1 },
	{ "G", ASTRIX_KBD_CHAR, 'G', 1, 1 }, { "H", ASTRIX_KBD_CHAR, 'H', 1, 1 },
	{ "J", ASTRIX_KBD_CHAR, 'J', 1, 1 }, { "K", ASTRIX_KBD_CHAR, 'K', 1, 1 },
	{ "L", ASTRIX_KBD_CHAR, 'L', 1, 1 },
};

static const struct astrix_kbd_key row3[] = {
	{ "z", ASTRIX_KBD_CHAR, 'z', 1, 0 }, { "x", ASTRIX_KBD_CHAR, 'x', 1, 0 },
	{ "c", ASTRIX_KBD_CHAR, 'c', 1, 0 }, { "v", ASTRIX_KBD_CHAR, 'v', 1, 0 },
	{ "b", ASTRIX_KBD_CHAR, 'b', 1, 0 }, { "n", ASTRIX_KBD_CHAR, 'n', 1, 0 },
	{ "m", ASTRIX_KBD_CHAR, 'm', 1, 0 }, { ",", ASTRIX_KBD_CHAR, ',', 1, 0 },
	{ ".", ASTRIX_KBD_CHAR, '.', 1, 0 }, { "?", ASTRIX_KBD_SLASH, 0, 1, 0 },
};
static const struct astrix_kbd_key row3_hi[] = {
	{ "Z", ASTRIX_KBD_CHAR, 'Z', 1, 1 }, { "X", ASTRIX_KBD_CHAR, 'X', 1, 1 },
	{ "C", ASTRIX_KBD_CHAR, 'C', 1, 1 }, { "V", ASTRIX_KBD_CHAR, 'V', 1, 1 },
	{ "B", ASTRIX_KBD_CHAR, 'B', 1, 1 }, { "N", ASTRIX_KBD_CHAR, 'N', 1, 1 },
	{ "M", ASTRIX_KBD_CHAR, 'M', 1, 1 }, { ";", ASTRIX_KBD_CHAR, ';', 1, 1 },
	{ ":", ASTRIX_KBD_CHAR, ':', 1, 1 }, { "?", ASTRIX_KBD_CHAR, '?', 1, 1 },
};

static const struct astrix_kbd_key row4[] = {
	{ "Esc", ASTRIX_KBD_ESC, 0, 1.2f, 0 },   { "Tab", ASTRIX_KBD_TAB, 0, 1.2f, 0 },
	{ "Ctrl", ASTRIX_KBD_CTRL, 0, 1.2f, 0 }, { "^", ASTRIX_KBD_CTRL, 0, 1.2f, 0 },
	{ " ", ASTRIX_KBD_CHAR, ' ', 5.0f, 0 },  { "\xe2\x86\x90", ASTRIX_KBD_LEFT, 0, 1, 0 },
	{ "\xe2\x86\x93", ASTRIX_KBD_DOWN, 0, 1, 0 },  { "\xe2\x86\x91", ASTRIX_KBD_UP, 0, 1, 0 },
	{ "\xe2\x86\x92", ASTRIX_KBD_RIGHT, 0, 1, 0 }, { "\xe2\x8c\xab", ASTRIX_KBD_ENTER, 0, 1.6f, 0 },
};
static const struct astrix_kbd_key row4_hi[] = {
	{ "Esc", ASTRIX_KBD_ESC, 0, 1.2f, 1 },   { "Tab", ASTRIX_KBD_TAB, 0, 1.2f, 1 },
	{ "Ctrl", ASTRIX_KBD_CTRL, 0, 1.2f, 1 }, { "\xe2\x8c\xab", ASTRIX_KBD_ENTER, 0, 2.2f, 1 },
	{ " ", ASTRIX_KBD_CHAR, ' ', 5.0f, 1 },  { "\x08", ASTRIX_KBD_BACKSPACE, 0, 1.6f, 1 },
	{ "\xe2\x86\x90", ASTRIX_KBD_LEFT, 0, 1, 1 },  { "\xe2\x86\x93", ASTRIX_KBD_DOWN, 0, 1, 1 },
	{ "\xe2\x86\x91", ASTRIX_KBD_UP, 0, 1, 1 },  { "\xe2\x86\x92", ASTRIX_KBD_RIGHT, 0, 1, 1 },
};

static const struct astrix_kbd_key *const rows[ASTRIX_KBD_ROWS][2] = {
	{ row0, row0_hi },
	{ row1, row1_hi },
	{ row2, row2_hi },
	{ row3, row3_hi },
	{ row4, row4_hi },
};
static const int row_counts[ASTRIX_KBD_ROWS][2] = {
	{ (int)(sizeof(row0) / sizeof(row0[0])),
	  (int)(sizeof(row0_hi) / sizeof(row0_hi[0])) },
	{ (int)(sizeof(row1) / sizeof(row1[0])),
	  (int)(sizeof(row1_hi) / sizeof(row1_hi[0])) },
	{ (int)(sizeof(row2) / sizeof(row2[0])),
	  (int)(sizeof(row2_hi) / sizeof(row2_hi[0])) },
	{ (int)(sizeof(row3) / sizeof(row3[0])),
	  (int)(sizeof(row3_hi) / sizeof(row3_hi[0])) },
	{ (int)(sizeof(row4) / sizeof(row4[0])),
	  (int)(sizeof(row4_hi) / sizeof(row4_hi[0])) },
};

/* --- state --------------------------------------------------------------- */

static bool ctrl_active = false;
static bool kbd_visible = true;

bool astrix_kbd_ctrl_active(void) {
	return ctrl_active;
}

void astrix_kbd_ctrl_set(bool active) {
	ctrl_active = active;
}

void astrix_kbd_set_visible(bool visible) {
	kbd_visible = visible;
}

bool astrix_kbd_visible(void) {
	return kbd_visible;
}

const struct astrix_kbd_key *astrix_kbd_row(int row, int layer, int *count) {
	if (row < 0 || row >= ASTRIX_KBD_ROWS) {
		*count = 0;
		return NULL;
	}
	if (layer) {
		layer = 1;
	}
	*count = row_counts[row][layer];
	return rows[row][layer];
}

int astrix_kbd_height(int height) {
	/* 5 rows on a 720x1600 panel is ~300px: large enough to hit, small
	 * enough to leave the terminal readable. */
	int h = height * 3 / 16;
	if (h < 150) {
		h = 150;
	}
	if (h > height / 2) {
		h = height / 2;
	}
	return h;
}
