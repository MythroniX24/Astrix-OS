/*
 * Astrix OS - host test for the shell's dock hit testing.
 *
 * Why this exists: the dock icons are drawn in a band pinned above the
 * navigation bar, while the app grid is laid out from the top of the
 * screen. They are two different coordinate systems on the same surface,
 * and they had already drifted apart once - the icons were drawn at the
 * bottom and hit-tested with grid coordinates near the top, so real taps
 * missed. Nothing about that is visible in a screenshot; it only shows up
 * when a real finger lands on the screen.
 *
 * The renderer (gui/shell/src/render.c) draws the dock with:
 *
 *     cell   = width / 5
 *     dock_h = cell + 24
 *     x0     = (width - ASTRIX_DOCK_COLS * (cell + 10)) / 2
 *     icon i = { x0 + i * (cell + 10), nav_top - dock_h, cell, dock_h }
 *
 * and this test pins the *hit test* to exactly that geometry. It includes
 * input.c directly because the functions under test are static - the
 * alternative is exporting them purely for the test.
 *
 * It is a host test: no Wayland, no compositor, no QEMU. It runs in
 * milliseconds, so it can guard every layout change instead of costing a
 * ten-minute VM boot to notice.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astrix_shell.h"

/* The layout constants the renderer uses; see the comment above. */
#define TEST_DOCK_COLS 4
#define TEST_GRID_COLS 4

/* Pulls in hit_test_dock_icon(), hit_test_app_icon() and grid_cell_width(). */
#include "../gui/shell/src/input.c"

static int failures;

static void check(bool ok, const char *what) {
	printf("%s %s\n", ok ? "  +" : "  x", what);
	if (!ok) {
		failures++;
	}
}

/* Mirrors gui/shell/src/render.c's dock drawing. */
static void dock_geometry(const struct astrix_shell *sh, int *cell, int *dock_h, int *x0,
                          int *y0) {
	*cell = sh->width / 5;
	*dock_h = *cell + 24;
	int nav_top = sh->height - sh->theme->navbar_h;
	*x0 = (sh->width - TEST_DOCK_COLS * (*cell + 10)) / 2;
	*y0 = nav_top - *dock_h;
}

static void add_app(struct astrix_shell *sh, const char *id, const char *name,
                    const char *exec, bool pinned) {
	struct astrix_app app;
	memset(&app, 0, sizeof(app));
	snprintf(app.id, sizeof(app.id), "%s", id);
	snprintf(app.name, sizeof(app.name), "%s", name);
	snprintf(app.exec, sizeof(app.exec), "%s", exec);
	app.pinned = pinned;
	astrix_shell_add_app(sh, &app);
}

static struct astrix_shell *make_shell(int w, int h, int pinned_count) {
	struct astrix_shell *sh = astrix_shell_create(w, h);
	for (int i = 0; i < pinned_count; i++) {
		char id[32], name[32], exec[32];
		snprintf(id, sizeof(id), "astrix-app%d", i);
		snprintf(name, sizeof(name), "App %d", i);
		snprintf(exec, sizeof(exec), "/usr/bin/astrix-app%d", i);
		add_app(sh, id, name, exec, true);
	}
	for (int i = 0; i < 3; i++) {
		char id[32], name[32], exec[32];
		snprintf(id, sizeof(id), "other%d", i);
		snprintf(name, sizeof(name), "Other %d", i);
		snprintf(exec, sizeof(exec), "/usr/bin/other%d", i);
		add_app(sh, id, name, exec, false);
	}
	return sh;
}

int main(void) {
	printf("== shell dock hit test ==\n");

	/*
	 * The panel the QEMU dev environment actually provides, and the
	 * phone panel this build targets. Both must work, because the shell
	 * now lays out for whatever wl_output reports.
	 */
	const int sizes[][2] = { { 720, 1600 }, { 1024, 768 }, { 1080, 2400 } };
	for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		int w = sizes[s][0], h = sizes[s][1];
		struct astrix_shell *sh = make_shell(w, h, 4);

		char msg[160];
		int cell, dock_h, x0, y0;
		dock_geometry(sh, &cell, &dock_h, &x0, &y0);

		snprintf(msg, sizeof(msg),
		         "%dx%d: dock band is y %d..%d across %d icons (cell %d)", w, h, y0,
		         y0 + dock_h, TEST_DOCK_COLS, cell);
		check(y0 >= 0 && y0 + dock_h <= h, msg);

		/* Every drawn dock icon must accept a tap at its centre. */
		bool all_hit = true;
		for (int i = 0; i < TEST_DOCK_COLS; i++) {
			int cx = x0 + i * (cell + 10) + cell / 2;
			int cy = y0 + dock_h / 2;
			int hit = hit_test_dock_icon(sh, cx, cy);
			if (hit != i) {
				all_hit = false;
				snprintf(msg, sizeof(msg),
				         "%dx%d: tap at the centre of dock icon %d (%d,%d) hit index "
				         "%d, expected %d",
				         w, h, i, cx, cy, hit, i);
				check(false, msg);
			}
		}
		if (all_hit) {
			snprintf(msg, sizeof(msg),
			         "%dx%d: every drawn dock icon accepts a tap at its centre", w, h);
			check(true, msg);
		}

		/* The dock must win over the grid on the home screen. */
		int grid_first = hit_test_app_icon(sh, ASTRIX_SCREEN_HOME, x0 + cell / 2,
		                                   y0 + dock_h / 2);
		snprintf(msg, sizeof(msg),
		         "%dx%d: a dock tap resolves to the dock icon, not a grid cell (got %d)",
		         w, h, grid_first);
		check(grid_first >= 0 && grid_first < 4, msg);

		/* Just outside the dock must not open anything. */
		int above = hit_test_dock_icon(sh, x0 + cell / 2, y0 - 5);
		int below = hit_test_dock_icon(sh, x0 + cell / 2, y0 + dock_h + 5);
		snprintf(msg, sizeof(msg), "%dx%d: taps just outside the dock band miss", w, h);
		check(above == -1 && below == -1, msg);

		/* The dock is home-screen only. */
		sh->home_page = 1;
		int other_page = hit_test_dock_icon(sh, x0 + cell / 2, y0 + dock_h / 2);
		sh->home_page = 0;
		snprintf(msg, sizeof(msg), "%dx%d: the dock is not tappable on other pages", w, h);
		check(other_page == -1, msg);

		/* The navigation bar below the dock must not open an app. */
		int navbar = hit_test_app_icon(sh, ASTRIX_SCREEN_HOME, x0 + cell / 2, h - 8);
		snprintf(msg, sizeof(msg), "%dx%d: the navigation bar does not open an app", w, h);
		check(navbar == -1, msg);

		/* A tap on a dock icon must actually open that app.
		 *
		 * The recogniser is fed raw touch events - it is what decides
		 * that a down/up pair 40 ms apart at the same point is a tap -
		 * so the test does the same rather than asserting against a
		 * synthesised gesture. */
		struct astrix_input_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.x = x0 + cell / 2;
		ev.y = y0 + dock_h / 2;
		ev.kind = ASTRIX_INPUT_TOUCH_DOWN;
		ev.timestamp = 1.0;
		astrix_shell_handle_input(sh, &ev);
		ev.kind = ASTRIX_INPUT_TOUCH_UP;
		ev.timestamp = 1.04;
		astrix_shell_handle_input(sh, &ev);
		snprintf(msg, sizeof(msg), "%dx%d: tapping a dock icon opens app 0 (screen %d, "
		                           "foreground %d)",
		         w, h, (int)sh->screen, sh->foreground_app);
		check(sh->screen == ASTRIX_SCREEN_APP && sh->foreground_app == 0, msg);

		astrix_shell_destroy(sh);
	}

	/*
	 * A dock with more pinned apps than columns must not leak a fifth
	 * icon into the hit test: the loop has to stop at ASTRIX_DOCK_COLS.
	 */
	{
		struct astrix_shell *sh = make_shell(720, 1600, 7);
		int cell, dock_h, x0, y0;
		dock_geometry(sh, &cell, &dock_h, &x0, &y0);
		int extra_x = x0 + 5 * (cell + 10) + cell / 2;
		int extra = hit_test_dock_icon(sh, extra_x, y0 + dock_h / 2);
		check(extra == -1, "a fifth column does not exist when the dock holds 4 columns");
		astrix_shell_destroy(sh);
	}

	/*
	 * App switcher cards. Until this existed, closing a running app had
	 * no user path at all: astrix_shell_close_app() was implemented but
	 * nothing in the input layer could reach it, so a launched app stayed
	 * alive for the whole session. The card layout here is the one in
	 * render.c's draw_app_switcher(), and a swipe up on a card must
	 * request a kill of exactly that app.
	 */
	printf("== app switcher card hit test ==\n");
	{
		const int sizes[][2] = { { 720, 1600 }, { 1024, 768 }, { 1080, 2400 } };
		for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
			int w = sizes[s][0], h = sizes[s][1];
			struct astrix_shell *sh = make_shell(w, h, 4);
			/* Three running apps = the three cards the switcher draws. */
			for (int i = 0; i < 3; i++) {
				sh->apps[i].running = true;
				sh->apps[i].pid = 1000 + i;
			}
			sh->screen = ASTRIX_SCREEN_APP_SWITCHER;

			int card_w = w / 3 + 40;
			int card_h = (int)(h * 0.52f);
			int step = card_w / 2;
			int cx = (w - 2 * card_w) / 2;
			int cy = h / 2 - card_h / 2 + 20;

			char msg[160];
			/* Every card must lie entirely on the panel, or it cannot
			 * be reached. This is the bug that made the second and
			 * third recents cards dead. */
			bool all_onscreen = true;
			for (int i = 0; i < 3; i++) {
				int card_x = cx + i * step;
				if (card_x < 0 || card_x + card_w > w) {
					all_onscreen = false;
					snprintf(msg, sizeof(msg), "%dx%d: card %d spans x %d..%d, off the panel", w, h,
					         i, card_x, card_x + card_w);
					check(false, msg);
				}
			}
			snprintf(msg, sizeof(msg), "%dx%d: all 3 recents cards fit on the panel", w, h);
			check(all_onscreen, msg);

			bool all_hit = true;
			for (int i = 0; i < 3; i++) {
				int card_x = cx + i * step;
				/* Aim at the strip of this card that no later card
				 * covers: its left half, or the whole card for the
				 * frontmost one. */
				int px = card_x + step / 2;
				int py = cy + card_h / 2;
				int hit = hit_test_switcher_card(sh, px, py);
				if (hit != i) {
					all_hit = false;
					snprintf(msg, sizeof(msg), "%dx%d: visible strip of card %d should hit app %d, got %d",
					         w, h, i, i, hit);
					check(false, msg);
				}
			}
			snprintf(msg, sizeof(msg),
			         "%dx%d: every drawn recents card is hit-testable (%d cards)", w, h, 3);
			check(all_hit, msg);

			/* The switcher must not claim taps on other screens. */
			sh->screen = ASTRIX_SCREEN_HOME;
			snprintf(msg, sizeof(msg), "%dx%d: no card hit outside the switcher", w, h);
			check(hit_test_switcher_card(sh, cx + step / 2, cy + card_h / 2) == -1, msg);

			/*
			 * The whole point: a swipe up on a card closes that app
			 * and requests a kill of its real pid.
			 */
			sh->screen = ASTRIX_SCREEN_APP_SWITCHER;
			sh->foreground_app = 1;
			struct astrix_input_event ev;
			memset(&ev, 0, sizeof(ev));
			ev.kind = ASTRIX_INPUT_TOUCH_DOWN;
			ev.x = cx + step / 2;
			ev.y = cy + card_h / 2;
			ev.timestamp = 1.0;
			astrix_shell_handle_input(sh, &ev);
			/* Move well past the swipe threshold, then release. */
			ev.kind = ASTRIX_INPUT_TOUCH_MOTION;
			ev.y = cy - sh->gestures.cfg.swipe_threshold_px - 20;
			ev.timestamp = 1.05;
			astrix_shell_handle_input(sh, &ev);
			ev.kind = ASTRIX_INPUT_TOUCH_UP;
			ev.timestamp = 1.09;
			astrix_shell_handle_input(sh, &ev);

			snprintf(msg, sizeof(msg), "%dx%d: swiping up a card closes app 0 (running %d)", w, h,
			         (int)sh->apps[0].running);
			check(!sh->apps[0].running, msg);
			snprintf(msg, sizeof(msg),
			         "%dx%d: swiping up a card requests a kill of app 0 (got %d)", w, h,
			         sh->kill_pending);
			check(sh->kill_pending == 0, msg);
			snprintf(msg, sizeof(msg), "%dx%d: the other cards stay running", w, h);
			check(sh->apps[1].running && sh->apps[2].running, msg);

			/*
			 * Closing the last running app must not leave an empty
			 * recents screen on display.
			 */
			sh->apps[1].running = false;
			sh->apps[2].running = false;
			sh->apps[0].running = true;
			sh->apps[0].pid = 1000;
			sh->kill_pending = -1;
			sh->screen = ASTRIX_SCREEN_APP_SWITCHER;
			memset(&ev, 0, sizeof(ev));
			ev.kind = ASTRIX_INPUT_TOUCH_DOWN;
			ev.x = cx + step / 2;
			ev.y = cy + card_h / 2;
			ev.timestamp = 2.0;
			astrix_shell_handle_input(sh, &ev);
			ev.kind = ASTRIX_INPUT_TOUCH_MOTION;
			ev.y = cy - sh->gestures.cfg.swipe_threshold_px - 20;
			ev.timestamp = 2.05;
			astrix_shell_handle_input(sh, &ev);
			ev.kind = ASTRIX_INPUT_TOUCH_UP;
			ev.timestamp = 2.09;
			astrix_shell_handle_input(sh, &ev);
			snprintf(msg, sizeof(msg), "%dx%d: closing the last app leaves the switcher (screen %d)",
			         w, h, (int)sh->screen);
			check(sh->screen == ASTRIX_SCREEN_HOME, msg);

			astrix_shell_destroy(sh);
		}
	}

	if (failures > 0) {
		printf("FAILED: %d check(s)\n", failures);
		return 1;
	}
	printf("all shell input checks passed\n");
	return 0;
}
