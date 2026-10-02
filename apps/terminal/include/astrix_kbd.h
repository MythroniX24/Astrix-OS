/*
 * Astrix OS - on-screen touch keyboard.
 *
 * A terminal emulator on a phone is useless without a way to type. This is a
 * simple QWERTY layout with the keys a shell actually needs (Ctrl, Esc, Tab,
 * arrows, pipe, tilde) that Astrix Terminal draws itself. It is not a general
 * text-input service: there is no IME, no autocorrect and no handwriting. That
 * is a deliberate scope choice, and it means the keyboard is fully testable
 * without an input method framework.
 *
 * The layout is data, not drawing code, so a different keyboard (numeric pad
 * for the dialer, a symbol layer) is a data change.
 */

#ifndef ASTRIX_KBD_H
#define ASTRIX_KBD_H

#include <stdbool.h>

#define ASTRIX_KBD_ROWS 5
#define ASTRIX_KBD_MAX_KEYS 14

/* What a key sends to the PTY. */
enum astrix_kbd_action {
	ASTRIX_KBD_CHAR,   /* send `ch` literally                          */
	ASTRIX_KBD_SYM,    /* send `ch` but with a sticky modifier applied */
	ASTRIX_KBD_CTRL,   /* toggle sticky Ctrl for the next key           */
	ASTRIX_KBD_ENTER,
	ASTRIX_KBD_BACKSPACE,
	ASTRIX_KBD_TAB,
	ASTRIX_KBD_ESC,
	ASTRIX_KBD_UP,
	ASTRIX_KBD_DOWN,
	ASTRIX_KBD_LEFT,
	ASTRIX_KBD_RIGHT,
	ASTRIX_KBD_PIPE,   /* | - the character people actually want       */
	ASTRIX_KBD_TILDE,  /* ~ - path expansion, home, sudo              */
	ASTRIX_KBD_MINUS,  /* - the same key on a physical keyboard       */
	ASTRIX_KBD_SLASH,
	ASTRIX_KBD_AT,
};

struct astrix_kbd_key {
	const char *label;
	enum astrix_kbd_action action;
	char ch;         /* for ASTRIX_KBD_CHAR / _SYM */
	float weight;    /* relative width within the row */
	int layer;       /* 0 = letters, 1 = shifted/symbols */
};

/* True while a sticky Ctrl is armed: the next character becomes C-<c>. */
bool astrix_kbd_ctrl_active(void);
void astrix_kbd_ctrl_set(bool active);

/* The keys of one row/layer, and how many there are. */
const struct astrix_kbd_key *astrix_kbd_row(int row, int layer, int *count);

/* Whether the keyboard should be on screen. */
void astrix_kbd_set_visible(bool visible);
bool astrix_kbd_visible(void);

/* Total height the keyboard occupies at the bottom of a `height`-tall screen. */
int astrix_kbd_height(int height);

#endif /* ASTRIX_KBD_H */
