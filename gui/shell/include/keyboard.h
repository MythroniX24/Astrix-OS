/*
 * Astrix OS - system on-screen keyboard (layout, hit-testing, key model).
 *
 * Why the keyboard is part of the shell
 * -------------------------------------
 * A touch OS without an on-screen keyboard cannot type, and everything else -
 * the terminal, Settings, a PIN lock screen - is unusable without typing. This
 * is the single largest gap between Astrix and something you would recognise
 * as a phone OS, so it lives in the shell rather than inside one app: the
 * terminal already had a private keyboard, which meant two ways to type and no
 * way to type into Settings.
 *
 * How the keys reach the focused application
 * -----------------------------------------
 * A Wayland client cannot inject input into another client - only the
 * compositor owns the seat. So the shell does not fake it: it owns a virtual
 * keyboard (wlr-virtual-keyboard-unstable-v1) and asks the compositor to
 * deliver real key events to whatever has focus. The key queue below is the
 * seam: this module decides *what* was pressed and hands it over as a
 * codepoint; main.c turns a codepoint into an xkb keycode and pushes it
 * through the virtual keyboard. Nothing here needs a compositor, which is what
 * makes it unit-testable on the host.
 */

#ifndef ASTRIX_SHELL_KEYBOARD_H
#define ASTRIX_SHELL_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "astrix_ui.h"
#include "astrix_shell.h"

/*
 * enum astrix_kbd_action is declared in astrix_shell.h, next to the queue it
 * describes. This header is the layout and the hit-testing.
 */
struct astrix_kbd_key {
	struct astrix_rect rect;
	enum astrix_kbd_action action;
	char label[8];
	uint32_t codepoint;   /* valid when action == ASTRIX_KBD_CHAR */
	int width_units;      /* relative width; 1 = one key column */
};

/* The keyboard occupies the bottom of the screen and scales with it. */
struct astrix_rect astrix_kbd_area(int width, int height);

/*
 * Fill `out` with the keys for the current layer, returning how many there
 * are. The caller sizes the array; ASTRIX_KBD_MAX_KEYS is always enough.
 */
#define ASTRIX_KBD_MAX_KEYS 48
int astrix_kbd_layout(const struct astrix_shell *sh, int width, int height,
                      struct astrix_kbd_key *out, int max);

/* Which key is under (x, y)? -1 for none. Back-to-front, like the switcher. */
int astrix_kbd_hit(const struct astrix_shell *sh, int width, int height,
                   int x, int y, struct astrix_kbd_key *out);

/* Is (x, y) inside the keyboard, including the gap above its top row? */
bool astrix_kbd_contains(const struct astrix_shell *sh, int width, int height,
                         int x, int y);

/*
 * Handle a tap. Returns true when the keyboard consumed it, in which case the
 * caller must not treat it as a gesture: a tap on "space" is not a tap on the
 * home screen behind the keyboard.
 */
bool astrix_kbd_tap(struct astrix_shell *sh, int x, int y);

/* The label a key should show right now (accounts for shift/symbols). */
const char *astrix_kbd_label(const struct astrix_shell *sh, int row, int col);

/* Pull the next queued key. Returns false when the queue is empty. */
bool astrix_kbd_pop(struct astrix_shell *sh, enum astrix_kbd_action *action,
                    uint32_t *codepoint);

/*
 * The status-bar button that opens and closes the keyboard.
 *
 * render.c draws it and input.c hit-tests it, so the geometry lives here and
 * only here. A status-bar target that is drawn in one file and hit-tested in
 * another is how a button ends up in the wrong place; that exact bug cost a
 * gesture class once already (bug 43), so the rect is shared rather than
 * written twice.
 */
struct astrix_rect astrix_kbd_toggle_rect(const struct astrix_shell *sh);

/* Toggle the keyboard when (x, y) is the button. True when it consumed it. */
bool astrix_kbd_toggle(struct astrix_shell *sh, int x, int y);

#endif /* ASTRIX_SHELL_KEYBOARD_H */