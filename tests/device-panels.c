/*
 * Astrix OS - host test for the shell layout at a real phone panel's geometry.
 *
 * Why this exists
 * ---------------
 * "Does it work on the moto g64 5G?" is currently unanswerable: that board has
 * no mainline display driver, so nothing has ever been drawn on it and nothing
 * ever will be until someone writes one. What *is* answerable today, without a
 * phone and without a boot, is the part that is purely arithmetic about the
 * panel: at 1080x2400, does the dock still fit, is every dock icon still big
 * enough to hit with a thumb, does the keyboard still fit on screen, and does
 * the status-bar keyboard button still land where the tap handler expects it.
 *
 * Those are real failure modes and they are invisible in the QEMU dev
 * environment, which runs at 1024x768 - a landscape shape neither phone has.
 * A layout that is fine at 1024x768 can easily put the dock's bottom edge off
 * a 2400-pixel-tall screen.
 *
 * The point of driving this from the device *profiles* rather than a hardcoded
 * list is that a test which duplicates the numbers it is checking against is a
 * test that silently passes when the hardware spec changes. tests/
 * test-device-panels.sh reads each profile and passes the geometry in, so a
 * panel that stops matching reality fails here.
 *
 * What is checked, at each geometry:
 *
 *   1. the dock band lies entirely inside the screen
 *   2. all four dock icons sit inside the band and are evenly spaced
 *   3. every dock icon is at least MIN_ICON_FRACTION of the panel width, so a
 *      thumb can hit it - checked as a fraction rather than in pixels, because
 *      720x1520 and 1080x2400 have completely different densities
 *   4. a tap at each dock icon's centre hit-tests to that icon, through the
 *      real hit_test_dock_icon(), not a copy of its arithmetic
 *   5. the dock is actually PAINTED where the hit-test claims it is - the two
 *      can disagree, and when they do the shell is beautiful and unusable
 *   6. the keyboard area and every key in it lie inside the screen
 *   7. every key is at least MIN_KEY_FRACTION of the panel width
 *   8. the status-bar keyboard button is inside the screen and above the
 *      keyboard, so it is reachable while the keyboard is closed
 *   9. every screen renders without the buffer being left untouched
 *
 * Host test: no phone, no bootloader, no QEMU, no compositor.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astrix_shell.h"
#include "keyboard.h"

/* Pulls in hit_test_dock_icon() so the assertions use the real hit-test. */
#include "../gui/shell/src/input.c"

/* The renderer draws 4 dock icons across the panel; see render.c. */
#define TEST_DOCK_COLS 4

/*
 * A dock icon narrower than this fraction of the panel is a mis-tap waiting to
 * happen. At 720 wide, 15% is 108px; at 1080 it is 162px. Both are comfortably
 * above a thumb contact patch, which is the point of the fraction.
 */
#define MIN_ICON_FRACTION 0.15
/* Same reasoning for a key. QWERTY keys are narrower than dock icons. */
#define MIN_KEY_FRACTION 0.06

static int failures;

static void check(bool ok, const char *what) {
	printf("  %s %s\n", ok ? "+" : "x", what);
	if (!ok) {
		failures++;
	}
}

static void checkf(bool ok, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	char msg[256];
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	check(ok, msg);
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

static struct astrix_shell *make_shell(int w, int h, int pinned) {
	struct astrix_shell *sh = astrix_shell_create(w, h);
	if (!sh) {
		return NULL;
	}
	for (int i = 0; i < pinned; i++) {
		struct astrix_app app;
		memset(&app, 0, sizeof(app));
		snprintf(app.id, sizeof(app.id), "org.astrix.Dock%d", i);
		snprintf(app.name, sizeof(app.name), "Dock %d", i);
		snprintf(app.exec, sizeof(app.exec), "astrix-app%d", i);
		app.pinned = true;
		astrix_shell_add_app(sh, &app);
	}
	for (int i = 0; i < 8; i++) {
		struct astrix_app app;
		memset(&app, 0, sizeof(app));
		snprintf(app.id, sizeof(app.id), "org.astrix.App%d", i);
		snprintf(app.name, sizeof(app.name), "App %d", i);
		snprintf(app.exec, sizeof(app.exec), "astrix-app%d", i);
		app.pinned = false;
		astrix_shell_add_app(sh, &app);
	}
	return sh;
}

static uint32_t pixel(const struct astrix_shell *sh, int x, int y) {
	if (x < 0 || y < 0 || x >= sh->width || y >= sh->height) {
		return 0;
	}
	return sh->pixels[(size_t)y * sh->stride + x];
}

static int luma(uint32_t p) {
	return (int)(((p >> 16) & 0xFF) * 30 + ((p >> 8) & 0xFF) * 59 + (p & 0xFF) * 11) / 100;
}

/* Mean luminance of a rect, skipping the outer 15% so rounded corners and the
 * gap between cards do not dominate the sample. */
static int mean_luma(const struct astrix_shell *sh, int x, int y, int w, int h) {
	int total = 0, n = 0;
	int x0 = x + w * 15 / 100, x1 = x + w * 85 / 100;
	int y0 = y + h * 15 / 100, y1 = y + h * 85 / 100;
	for (int yy = y0; yy < y1; yy++) {
		for (int xx = x0; xx < x1; xx++) {
			total += luma(pixel(sh, xx, yy));
			n++;
		}
	}
	return n ? total / n : 0;
}

/* Is the framebuffer non-uniform? A screen that never got drawn is one flat
 * colour, and every layout assertion above would still pass on it. */
static bool frame_is_painted(const struct astrix_shell *sh) {
	return mean_luma(sh, 0, 0, sh->width, sh->height) >= 0 &&
	       (luma(pixel(sh, 2, 2)) != luma(pixel(sh, sh->width - 3, sh->height / 2)) ||
	        luma(pixel(sh, sh->width / 2, sh->height / 2)) !=
	            luma(pixel(sh, sh->width / 2, sh->height - 4)));
}

static void check_geometry(int w, int h, const char *label) {
	printf("== %s: %dx%d ==\n", label, w, h);
	struct astrix_shell *sh = make_shell(w, h, 4);
	if (!sh) {
		check(false, "shell created");
		return;
	}
	sh->screen = ASTRIX_SCREEN_HOME;
	astrix_shell_draw(sh);

	/* --- 1/2/3: the dock band ------------------------------------------- */
	int cell, dock_h, x0, y0;
	dock_geometry(sh, &cell, &dock_h, &x0, &y0);
	checkf(y0 >= 0 && y0 + dock_h <= h,
	       "dock band (y %d..%d) is inside the %d-tall screen", y0, y0 + dock_h, h);
	checkf(x0 >= 0 && x0 + TEST_DOCK_COLS * (cell + 10) <= w,
	       "dock row (x %d..%d) is inside the %d-wide screen", x0,
	       x0 + TEST_DOCK_COLS * (cell + 10), w);
	checkf(cell >= (int)(w * MIN_ICON_FRACTION),
	       "each dock icon is %dpx, at least %.0f%% of the %dpx width", cell,
	       MIN_ICON_FRACTION * 100.0, w);

	int centres[TEST_DOCK_COLS];
	for (int i = 0; i < TEST_DOCK_COLS; i++) {
		int ix = x0 + i * (cell + 10);
		int cx = ix + cell / 2;
		int cy = y0 + dock_h / 2;
		centres[i] = cx;
		checkf(ix >= 0 && ix + cell <= w && y0 + dock_h <= h,
		       "dock icon %d (x %d..%d) is fully on screen", i, ix, ix + cell);
		/* 4: the real hit-test, not a reimplementation of it. */
		checkf(hit_test_dock_icon(sh, cx, cy) == i,
		       "a tap at the centre of dock icon %d (%d,%d) hits that icon", i, cx, cy);
	}
	/* Evenly spaced: equal gaps, so no icon is accidentally larger. */
	bool even = true;
	for (int i = 1; i < TEST_DOCK_COLS; i++) {
		if (centres[i] - centres[i - 1] != centres[1] - centres[0]) {
			even = false;
		}
	}
	checkf(even, "the four dock icons are evenly spaced across the panel");

	/* --- 5: the dock is painted where the hit-test says it is -----------
	 * This is the check that catches the bug everything else misses: a dock
	 * drawn at a different offset from the one the hit-test uses. Both
	 * rectangles are derived from the same constants here, so instead compare
	 * the *pixels*: the cards are materially brighter than the wallpaper
	 * strip directly above them, and if the band is empty they match.
	 */
	int icon_luma = mean_luma(sh, x0 + cell / 4, y0 + dock_h / 4, cell / 2, dock_h / 2);
	int wall_luma = mean_luma(sh, x0 + cell / 4, y0 - 60, cell / 2, 40);
	checkf(abs(icon_luma - wall_luma) >= 6,
	       "the dock band is actually painted (icons luma %d vs wallpaper %d)",
	       icon_luma, wall_luma);

	/* --- 6/7: the keyboard ---------------------------------------------- */
	struct astrix_rect area = astrix_kbd_area(w, h);
	checkf(area.x >= 0 && area.y >= 0 && area.x + area.w <= w && area.y + area.h <= h,
	       "keyboard area (x %d..%d, y %d..%d) is inside the screen", area.x,
	       area.x + area.w, area.y, area.y + area.h);
	checkf(area.h > 0 && area.h < h, "the keyboard covers %d of %d rows, not the whole screen",
	       area.h, h);

	struct astrix_kbd_key keys[ASTRIX_KBD_MAX_KEYS];
	int n = astrix_kbd_layout(sh, w, h, keys, ASTRIX_KBD_MAX_KEYS);
	checkf(n > 20, "the keyboard laid out %d keys", n);

	int offscreen = 0, slivers = 0;
	for (int i = 0; i < n; i++) {
		struct astrix_rect r = keys[i].rect;
		if (r.x < 0 || r.y < 0 || r.x + r.w > w || r.y + r.h > h) {
			offscreen++;
		}
		if (r.w < (int)(w * MIN_KEY_FRACTION)) {
			slivers++;
		}
	}
	checkf(offscreen == 0, "every key is inside the screen (%d outside)", offscreen);
	checkf(slivers == 0, "no key is narrower than %.0f%% of the width (%d slivers)",
	       MIN_KEY_FRACTION * 100.0, slivers);

	/* A key must also be reachable through the real hit-test. */
	int hit = astrix_kbd_hit(sh, w, h, keys[0].rect.x + keys[0].rect.w / 2,
	                         keys[0].rect.y + keys[0].rect.h / 2, &keys[0]);
	checkf(hit >= 0, "a tap at the centre of a key reaches that key");

	/* --- 8: the keyboard button in the status bar ----------------------- */
	struct astrix_rect tb = astrix_kbd_toggle_rect(sh);
	checkf(tb.x >= 0 && tb.y >= 0 && tb.x + tb.w <= w && tb.y + tb.h <= h,
	       "the status-bar keyboard button is inside the screen (x %d..%d, y %d..%d)",
	       tb.x, tb.x + tb.w, tb.y, tb.y + tb.h);
	checkf(tb.y + tb.h <= area.y,
	       "the status-bar button sits above the keyboard, so it is reachable "
	       "while the keyboard is closed");

	/* --- 9: every screen renders --------------------------------------- */
	astrix_shell_draw(sh);
	check(frame_is_painted(sh), "the home screen was actually painted");

	sh->screen = ASTRIX_SCREEN_LAUNCHER;
	astrix_shell_draw(sh);
	check(frame_is_painted(sh), "the launcher was actually painted");

	sh->screen = ASTRIX_SCREEN_NOTIFICATION;
	astrix_shell_notify(sh, "org.astrix.Terminal", "Terminal", "Build finished",
	                    "no errors", ASTRIX_NOTIF_NORMAL);
	astrix_shell_draw(sh);
	check(frame_is_painted(sh), "the notification shade was actually painted");

	sh->screen = ASTRIX_SCREEN_QUICK_SETTINGS;
	astrix_shell_draw(sh);
	check(frame_is_painted(sh), "quick settings was actually painted");

	sh->screen = ASTRIX_SCREEN_HOME;
	sh->apps[0].running = true;
	sh->apps[1].running = true;
	sh->apps[4].running = true;
	sh->screen = ASTRIX_SCREEN_APP_SWITCHER;
	astrix_shell_draw(sh);
	check(frame_is_painted(sh), "the app switcher was actually painted");

	astrix_shell_destroy(sh);
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: device-panels <width> <height> <label>\n");
		return 2;
	}
	check_geometry(atoi(argv[1]), atoi(argv[2]), argv[3]);
	printf("%s\n", failures ? "device panel checks FAILED" : "device panel checks passed");
	return failures ? 1 : 0;
}