/*
 * Astrix OS - Astrix Shell input handling.
 *
 * Binds the gesture recogniser to navigation. This is where a touch OS earns
 * its keep: swipe up to go home, swipe up and hold for the app switcher, swipe
 * down for the notification shade, swipe from the edge to go back.
 *
 * Navigation follows a stack: whatever opened the current screen is remembered
 * in screen_from, so "back" always returns somewhere sensible.
 */

#include "astrix_shell.h"
#include "keyboard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Layout constants shared with render.c. Kept here as the hit-testing needs
 * the same numbers the drawing uses. */
#define GRID_COLS 4
#define GRID_TOP_PAD 44

static int grid_cell_width(const struct astrix_shell *sh) {
	return sh->width / (GRID_COLS + 1);
}

static int grid_top(const struct astrix_shell *sh) {
	return sh->theme->status_h + GRID_TOP_PAD;
}

/*
 * The dock is a single row of ASTRIX_DOCK_COLS icons pinned just above the
 * navigation bar - a completely different place on the screen from the app
 * grid, which starts at the top. Its hit test therefore cannot reuse the
 * grid geometry, and it did not: icons were drawn in the dock and
 * hit-tested with grid coordinates near the top of the screen, so a real tap
 * on a dock icon missed it. render.c is the authority for both numbers; this
 * mirrors it, in the same way the grid geometry above already does, and the
 * column count comes from astrix_shell.h so the two cannot drift.
 *
 * Returns the index of the pinned app under (x, y), or -1. Mirrors
 * hit_test_app_icon's convention on purpose: the tap handler should not have
 * to know that the dock is a different shape from the grid.
 */
static int hit_test_dock_icon(const struct astrix_shell *sh, int x, int y) {
	if (x < 0 || y < 0 || x >= sh->width || y >= sh->height) {
		return -1;
	}
	if (sh->screen != ASTRIX_SCREEN_HOME || sh->home_page != 0) {
		return -1;
	}
	int cell = grid_cell_width(sh);
	int dock_h = cell + 24;
	int nav_top = sh->height - sh->theme->navbar_h;
	int x0 = (sh->width - ASTRIX_DOCK_COLS * (cell + 10)) / 2;
	int y0 = nav_top - dock_h;

	int on_page = 0;
	for (int i = 0; i < sh->app_count; i++) {
		if (!sh->apps[i].pinned) {
			continue;
		}
		if (on_page >= ASTRIX_DOCK_COLS) {
			break;
		}
		struct astrix_rect r = { x0 + on_page * (cell + 10), y0, cell, dock_h };
		if (astrix_rect_contains(r, x, y)) {
			return i;
		}
		on_page++;
	}
	return -1;
}

/*
 * Hit-test the app grid. Returns the app index under (x, y) on the given
 * screen, or -1. Mirrors the geometry in render.c.
 */
static int hit_test_app_icon(const struct astrix_shell *sh, enum astrix_screen screen, int x,
                             int y) {
	if (x < 0 || y < 0 || x >= sh->width || y >= sh->height) {
		return -1;
	}
	if (screen != ASTRIX_SCREEN_HOME && screen != ASTRIX_SCREEN_LAUNCHER) {
		return -1;
	}
	int top = grid_top(sh);
	if (screen == ASTRIX_SCREEN_LAUNCHER) {
		top += 32;
	}
	if (y < top) {
		return -1;
	}

	int cell_w = grid_cell_width(sh);
	int cell_h = cell_w + 12;
	int origin_x = (sh->width - GRID_COLS * cell_w) / 2;

	int col = (x - origin_x) / cell_w;
	if (col < 0 || col >= GRID_COLS) {
		return -1;
	}
	int row = (y - top) / cell_h;
	if (row < 0) {
		return -1;
	}
	int cell = row * GRID_COLS + col;

	/* On the home screen only this page's apps are drawn. */
	if (screen == ASTRIX_SCREEN_HOME) {
		/*
		 * The dock is checked first and against its own geometry: on
		 * page 0 the pinned icons are drawn at the bottom of the screen
		 * while the grid's coordinates describe the top of it. Testing
		 * the grid here would accept taps in the wrong place and reject
		 * real taps on the dock.
		 */
		int dock = hit_test_dock_icon(sh, x, y);
		if (dock >= 0) {
			return dock;
		}
		int on_page = 0;
		for (int i = 0; i < sh->app_count; i++) {
			int page = sh->apps[i].pinned ? 0 : sh->apps[i].page;
			if (page != sh->home_page) {
				continue;
			}
			if (on_page == cell) {
				return i;
			}
			on_page++;
		}
		return -1;
	}
	/* The drawer shows every app in order (scrolling is handled separately). */
	int idx = cell;
	if (idx < 0 || idx >= sh->app_count) {
		return -1;
	}
	return idx;
}

/*
 * Hit-test a recents card in the app switcher.
 *
 * Mirrors the layout in render.c's draw_app_switcher(): the first three
 * running apps, overlapping to the right, each card_w wide.
 *
 * Without this there was no way for a user to close a running app at all:
 * astrix_shell_close_app() existed but nothing in the input path could
 * reach it, so a launched app stayed alive until the whole session
 * ended. "The shell can terminate an app" and "a user can dismiss an app"
 * are different claims.
 */
static int hit_test_switcher_card(const struct astrix_shell *sh, int x, int y) {
	if (x < 0 || y < 0 || x >= sh->width || y >= sh->height) {
		return -1;
	}
	if (sh->screen != ASTRIX_SCREEN_APP_SWITCHER) {
		return -1;
	}
	int card_w = sh->width / 3 + 40;
	int card_h = (int)(sh->height * 0.52f);
	int step = card_w / 2;
	int cx = (sh->width - 2 * card_w) / 2;
	int cy = sh->height / 2 - card_h / 2 + 20;

	/*
	 * Walk the cards back to front. They overlap and the later ones are
	 * drawn on top, so a point inside two cards belongs to the one the
	 * user actually sees - the last one drawn. Testing front to back
	 * would return the card that is hidden underneath.
	 */
	int hit = -1;
	int idx = 0;
	for (int i = 0; i < sh->app_count; i++) {
		if (!sh->apps[i].running) {
			continue;
		}
		if (idx >= 3) {
			break;
		}
		struct astrix_rect card = { cx + idx * step, cy, card_w, card_h };
		if (astrix_rect_contains(card, x, y)) {
			hit = i;
		}
		idx++;
	}
	return hit;
}

static bool in_quick_settings_tile(const struct astrix_shell *sh, int x, int y) {
	/* Mirrors the tile grid in render.c. Only hits count for tiles whose
	 * hardware is present, matching what is actually drawn. */
	if (x < 0 || y < 0 || x >= sh->width || y >= sh->height) {
		return false;
	}
	int h = (int)(sh->height * 0.82f);
	int tile_w = (sh->width - 42) / 2, tile_h = 74;
	int ty = h - 210;

	struct {
		bool supported;
	} tiles[] = {
		{ sh->wifi_available },        /* Wi-Fi            */
		{ sh->bt_available },          /* Bluetooth        */
		{ true },                      /* Do Not Disturb   */
		{ sh->wifi_available || sh->bt_available }, /* Airplane */
		{ true },                      /* Sound            */
		{ true },                      /* Rotation         */
		{ sh->location_available },    /* Location         */
		{ true },                      /* Battery Saver    */
		{ sh->mobile_available },      /* Mobile Data      */
	};
	int drawn = 0;
	for (int i = 0; i < (int)(sizeof(tiles) / sizeof(tiles[0])); i++) {
		if (!tiles[i].supported) {
			continue;
		}
		int col = drawn % 2, row = drawn / 2;
		struct astrix_rect tr = { 14 + col * (tile_w + 14), ty + row * (tile_h + 12), tile_w,
		                          tile_h };
		if (astrix_rect_contains(tr, x, y)) {
			return true;
		}
		drawn++;
	}
	return false;
}

static int quick_settings_tile_index(const struct astrix_shell *sh, int x, int y) {
	int h = (int)(sh->height * 0.82f);
	int tile_w = (sh->width - 42) / 2, tile_h = 74;
	int ty = h - 210;

	struct {
		bool supported;
	} tiles[] = {
		{ sh->wifi_available },
		{ sh->bt_available },
		{ true },
		{ sh->wifi_available || sh->bt_available },
		{ true },
		{ true },
		{ sh->location_available },
		{ true },
		{ sh->mobile_available },
	};
	int drawn = 0;
	for (int i = 0; i < (int)(sizeof(tiles) / sizeof(tiles[0])); i++) {
		if (!tiles[i].supported) {
			continue;
		}
		int col = drawn % 2, row = drawn / 2;
		struct astrix_rect tr = { 14 + col * (tile_w + 14), ty + row * (tile_h + 12), tile_w,
		                          tile_h };
		if (astrix_rect_contains(tr, x, y)) {
			return i;
		}
		drawn++;
	}
	return -1;
}

/* Apply a quick-settings toggle. Returns the label toggled, for logging. */
static const char *toggle_setting(struct astrix_shell *sh, int tile) {
	switch (tile) {
	case 0:
		if (!sh->wifi_available) {
			return NULL;
		}
		sh->wifi_on = !sh->wifi_on;
		return "Wi-Fi";
	case 1:
		if (!sh->bt_available) {
			return NULL;
		}
		sh->bt_on = !sh->bt_on;
		return "Bluetooth";
	case 2:
		sh->do_not_disturb = !sh->do_not_disturb;
		return "Do Not Disturb";
	case 3:
		if (!(sh->wifi_available || sh->bt_available)) {
			return NULL;
		}
		sh->airplane_mode = !sh->airplane_mode;
		/* Airplane mode implies radios off. */
		if (sh->airplane_mode) {
			sh->wifi_on = false;
			sh->bt_on = false;
			sh->mobile_on = false;
		}
		return "Airplane Mode";
	case 4:
		sh->sound_on = !sh->sound_on;
		return "Sound";
	case 5:
		sh->rotation_locked = !sh->rotation_locked;
		return "Rotation Lock";
	case 6:
		if (!sh->location_available) {
			return NULL;
		}
		sh->location_on = !sh->location_on;
		return "Location";
	case 7:
		sh->battery_saver = !sh->battery_saver;
		return "Battery Saver";
	case 8:
		if (!sh->mobile_available) {
			return NULL;
		}
		sh->mobile_on = !sh->mobile_on;
		return "Mobile Data";
	}
	return NULL;
}

/* --- navigation helpers -------------------------------------------------- */

static void go_back(struct astrix_shell *sh) {
	/* Back unwinds to the screen we came from, falling back to home. */
	switch (sh->screen) {
	case ASTRIX_SCREEN_LAUNCHER:
	case ASTRIX_SCREEN_APP_SWITCHER:
	case ASTRIX_SCREEN_NOTIFICATION:
	case ASTRIX_SCREEN_QUICK_SETTINGS:
		sh->screen = sh->screen_from;
		sh->status_shade_open = false;
		sh->shade_progress = 0.0f;
		break;
	case ASTRIX_SCREEN_POWER_MENU:
		sh->screen = sh->screen_from;
		break;
	default:
		astrix_shell_go_home(sh);
		break;
	}
	sh->needs_redraw = true;
}

/* --- gesture dispatch ---------------------------------------------------- */

/*
 * Screen names for the navigation log.
 *
 * This exists for one reason: a touch OS is only "verified" when a real
 * gesture has been driven through the whole stack and can be seen to change
 * something. Without a log line saying "swipe up -> launcher", the evidence
 * is only that input arrived - not that the OS responded to it. A table is
 * used rather than a switch so an added screen cannot be silently forgotten:
 * the default arm reads "<N>", which is visibly wrong rather than absent.
 */
static const char *screen_name(enum astrix_screen s) {
	switch (s) {
	case ASTRIX_SCREEN_HOME:
		return "home";
	case ASTRIX_SCREEN_LAUNCHER:
		return "launcher";
	case ASTRIX_SCREEN_NOTIFICATION:
		return "notifications";
	case ASTRIX_SCREEN_QUICK_SETTINGS:
		return "quick-settings";
	case ASTRIX_SCREEN_APP_SWITCHER:
		return "app-switcher";
	case ASTRIX_SCREEN_POWER_MENU:
		return "power-menu";
	case ASTRIX_SCREEN_LOCK:
		return "lock";
	case ASTRIX_SCREEN_APP:
		return "app";
	default:
		return "<unknown>";
	}
}

static void handle_gesture(struct astrix_shell *sh, enum astrix_gesture g, int x, int y) {
	enum astrix_screen before = sh->screen;

	/*
	 * The keyboard button in the status bar, checked before anything else.
	 * It is the only control that must work from every screen, so it gets
	 * first refusal on a tap; otherwise a tap near the clock on the home
	 * screen would open something else and the keyboard would look broken.
	 */
	if (g == ASTRIX_GESTURE_TAP && astrix_kbd_toggle(sh, x, y)) {
		fprintf(stderr, "astrix-shell: keyboard %s\n",
		        sh->kbd_visible ? "opened" : "closed");
		return;
	}

	/* The lock screen intercepts everything except the unlock swipe. */
	if (sh->locked) {
		if (g == ASTRIX_GESTURE_SWIPE_UP || g == ASTRIX_GESTURE_TAP) {
			astrix_shell_toggle_lock(sh);
		}
		return;
	}

	switch (g) {
	case ASTRIX_GESTURE_SWIPE_UP:
		if (sh->screen == ASTRIX_SCREEN_APP_SWITCHER) {
			/*
			 * Up on a recents card closes that app, which is how a
			 * phone dismisses one. Off a card it goes home. The
			 * close is a request (kill_pending); whoever owns the
			 * compositor connection sends the signal.
			 *
			 * The card is resolved from where the finger went DOWN,
			 * not where it ended up. A swipe up always ends above
			 * the card it started on, so hit-testing the end
			 * position would make this gesture close nothing.
			 */
			int card = hit_test_switcher_card(sh, sh->gestures.start_x, sh->gestures.start_y);
			{
				int running = 0;
				for (int i = 0; i < sh->app_count; i++) {
					running += sh->apps[i].running ? 1 : 0;
				}
				fprintf(stderr,
				        "astrix-shell: switcher swipe-up from %d,%d hit card %d (%d app(s) running)\n",
				        sh->gestures.start_x, sh->gestures.start_y, card, running);
			}
			if (card >= 0) {
				astrix_shell_close_app(sh, card);
				/* The last app left: leave the switcher rather than
				 * showing an empty recents screen. */
				bool any = false;
				for (int i = 0; i < sh->app_count; i++) {
					if (sh->apps[i].running) {
						any = true;
						break;
					}
				}
				if (!any) {
					sh->screen = ASTRIX_SCREEN_HOME;
				}
				break;
			}
			astrix_shell_go_home(sh);
			break;
		}
		if (sh->screen == ASTRIX_SCREEN_HOME) {
			/* Up on home opens the app drawer, like a real phone. */
			sh->screen_from = sh->screen;
			sh->screen = ASTRIX_SCREEN_LAUNCHER;
		} else {
			astrix_shell_go_home(sh);
		}
		break;

	case ASTRIX_GESTURE_SWIPE_UP_HOLD:
		sh->screen_from = sh->screen;
		sh->screen = ASTRIX_SCREEN_APP_SWITCHER;
		break;

	case ASTRIX_GESTURE_SWIPE_DOWN:
		sh->screen_from = sh->screen;
		sh->screen = ASTRIX_SCREEN_NOTIFICATION;
		sh->status_shade_open = true;
		break;

	case ASTRIX_GESTURE_EDGE_SWIPE:
		go_back(sh);
		break;

	case ASTRIX_GESTURE_DOUBLE_TAP:
		astrix_shell_go_home(sh);
		break;

	case ASTRIX_GESTURE_TAP: {
		if (sh->screen == ASTRIX_SCREEN_APP_SWITCHER) {
			/* Tapping a card brings that app back to the front. */
			int card = hit_test_switcher_card(sh, x, y);
			if (card >= 0) {
				sh->foreground_app = card;
				sh->screen = ASTRIX_SCREEN_APP;
				break;
			}
			/* Tapping outside a card dismisses the switcher. */
			go_back(sh);
			break;
		}
		if (sh->screen == ASTRIX_SCREEN_POWER_MENU) {
			sh->screen = sh->screen_from;
			break;
		}
		if (sh->screen == ASTRIX_SCREEN_NOTIFICATION ||
		    sh->screen == ASTRIX_SCREEN_QUICK_SETTINGS) {
			int tile = quick_settings_tile_index(sh, x, y);
			if (tile >= 0) {
				const char *label = toggle_setting(sh, tile);
				sh->needs_redraw = true;
				(void)label;
				break;
			}
			/* Tapping the scrim closes the shade. */
			if (y < sh->theme->status_h) {
				go_back(sh);
			}
			break;
		}
		int app = hit_test_app_icon(sh, sh->screen, x, y);
		if (app >= 0) {
			sh->screen_from = sh->screen;
			astrix_shell_open_app(sh, app);
		}
		break;
	}

	case ASTRIX_GESTURE_LONG_PRESS:
		/* A long press on the home screen opens the power menu, which is
		 * where a phone puts its power controls. */
		if (sh->screen == ASTRIX_SCREEN_HOME) {
			sh->screen_from = sh->screen;
			sh->screen = ASTRIX_SCREEN_POWER_MENU;
		}
		break;

	case ASTRIX_GESTURE_SWIPE_LEFT:
	case ASTRIX_GESTURE_SWIPE_RIGHT:
		/* Horizontal swipes change launcher pages on the home screen. */
		if (sh->screen == ASTRIX_SCREEN_HOME) {
			if (g == ASTRIX_GESTURE_SWIPE_LEFT && sh->home_page < 2) {
				sh->home_page++;
			} else if (g == ASTRIX_GESTURE_SWIPE_RIGHT && sh->home_page > 0) {
				sh->home_page--;
			}
		}
		break;

	default:
		break;
	}
	sh->needs_redraw = true;
	/*
	 * One line per recognised gesture, on stderr, which the shell's unit
	 * sends to journal+console. This is the only way to see from outside
	 * the VM that a real touch event travelled
	 *   virtio input device -> evdev -> libseat -> libinput -> compositor
	 *   -> wl_pointer/wl_touch -> this process -> navigation
	 * and actually moved the UI. Everything else only proves the pieces
	 * started.
	 */
	/*
	 * The recogniser's start point is printed alongside the end point
	 * because a gesture is classified from the *vector* between them, and
	 * a wrong classification is otherwise undiagnosable from the outside:
	 * "swipe-right at 512,571" says nothing about where the press was
	 * anchored, which is exactly the question every one of these bugs
	 * turned on.
	 */
	if (before != sh->screen) {
		fprintf(stderr, "astrix-shell: %s from %d,%d to %d,%d: %s -> %s\n",
		        astrix_gesture_name(g), sh->gestures.start_x, sh->gestures.start_y, x, y,
		        screen_name(before), screen_name(sh->screen));
	} else {
		fprintf(stderr, "astrix-shell: %s from %d,%d to %d,%d on %s (no screen change)\n",
		        astrix_gesture_name(g), sh->gestures.start_x, sh->gestures.start_y, x, y,
		        screen_name(before));
	}
}

/* --- entry point --------------------------------------------------------- */

bool astrix_shell_handle_input(struct astrix_shell *sh, const struct astrix_input_event *ev) {
	if (!sh || !ev) {
		return false;
	}
	sh->last_event = *ev;

	/* Track the finger position so the UI can show press feedback. */
	switch (ev->kind) {
	case ASTRIX_INPUT_TOUCH_DOWN:
	case ASTRIX_INPUT_TOUCH_MOTION:
	case ASTRIX_INPUT_POINTER_MOTION:
	case ASTRIX_INPUT_POINTER_BUTTON:
		sh->pointer_x = ev->x;
		sh->pointer_y = ev->y;
		break;
	default:
		break;
	}

	/*
	 * The keyboard gets the press itself, not the tap.
	 *
	 * A key must respond when the finger lands, the way a real key does -
	 * waiting for the release means a key that feels broken under a fast
	 * typist, and on a phone the release can come a long way away. The
	 * release only clears the highlight.
	 */
	bool pressed = (ev->kind == ASTRIX_INPUT_TOUCH_DOWN) ||
	               (ev->kind == ASTRIX_INPUT_POINTER_BUTTON &&
	                (ev->state & ASTRIX_KEY_PRESSED));
	if (sh->kbd_visible) {
		if (pressed) {
			if (astrix_kbd_tap(sh, ev->x, ev->y)) {
				return true;
			}
		} else if (ev->kind == ASTRIX_INPUT_TOUCH_UP ||
		           (ev->kind == ASTRIX_INPUT_POINTER_BUTTON &&
		            !(ev->state & ASTRIX_KEY_PRESSED))) {
			if (sh->kbd_press_x >= 0) {
				sh->kbd_press_x = -1;
				sh->kbd_press_y = -1;
				sh->needs_redraw = true;
			}
		}
	}

	/* Scrolling in the launcher, before gestures are interpreted, so a
	 * scroll never looks like a page-changing swipe. */
	if (ev->kind == ASTRIX_INPUT_SCROLL && sh->screen == ASTRIX_SCREEN_LAUNCHER) {
		sh->launcher_scroll += ev->scroll_y * 24;
		int max_scroll = sh->app_count > GRID_COLS
		                     ? (sh->app_count / GRID_COLS + 1) * (grid_cell_width(sh) + 12)
		                     : 0;
		if (sh->launcher_scroll < 0) {
			sh->launcher_scroll = 0;
		}
		if (sh->launcher_scroll > max_scroll) {
			sh->launcher_scroll = max_scroll;
		}
		sh->needs_redraw = true;
		return true;
	}

	/* The notification shade is a drag: swiping up from the top closes it
	 * and updates progress so it follows the finger. */
	if (sh->screen == ASTRIX_SCREEN_NOTIFICATION || sh->screen == ASTRIX_SCREEN_QUICK_SETTINGS) {
		if (ev->kind == ASTRIX_INPUT_TOUCH_UP || (ev->kind == ASTRIX_INPUT_POINTER_BUTTON &&
		                                           !(ev->state & ASTRIX_KEY_PRESSED))) {
			enum astrix_gesture g = astrix_gesture_handle(&sh->gestures, ev);
			/* An upward swipe from the shade closes it. */
			if (g == ASTRIX_GESTURE_SWIPE_UP) {
				go_back(sh);
				sh->needs_redraw = true;
			}
			/* Anything else in the shade is handled below as a tap. */
			else if (g == ASTRIX_GESTURE_TAP) {
				handle_gesture(sh, g, ev->x, ev->y);
			}
			return true;
		}
		if (ev->kind == ASTRIX_INPUT_TOUCH_MOTION) {
			return true; /* the shade follows the drag; redraw elsewhere */
		}
	}

	enum astrix_gesture g = astrix_gesture_handle(&sh->gestures, ev);
	if (g != ASTRIX_GESTURE_NONE) {
		handle_gesture(sh, g, ev->x, ev->y);
		return true;
	}

	/* Any pointer activity may require a repaint (press feedback). */
	sh->needs_redraw = true;
	return true;
}
