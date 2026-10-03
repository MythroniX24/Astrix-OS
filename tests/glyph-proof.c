/*
 * Glyph proof sheet.
 *
 * Draws every icon in the glyph set into a small ARGB buffer and prints it as
 * ASCII. The shell cannot be screenshotted out of a booted VM (the compositor
 * runs on pixman, and QEMU's screendump captures the text console), so the
 * only honest way to *look* at what the icon code draws is to render it here
 * and read the pixels. A glyph that is silently empty, off-centre or
 * unreadable is otherwise invisible to every test in the suite.
 */
#include "astrix_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 44

static int failures;

static void show(const char *name, enum astrix_glyph g) {
	static uint32_t buf[N * N];
	struct astrix_canvas c = { buf, N, N, N };
	memset(buf, 0, sizeof(buf));
	astrix_clear(&c, astrix_rgba(0x10, 0x12, 0x18, 0xFF));
	astrix_draw_glyph(&c, (struct astrix_rect){ 2, 2, N - 4, N - 4 }, g,
	                  astrix_rgba(0xF5, 0xF7, 0xFF, 0xFF));
	printf("--- %s ---\n", name);
	int ink = 0;
	int min_x = N, max_x = -1, min_y = N, max_y = -1;
	for (int y = 0; y < N; y++) {
		char line[N + 1];
		for (int x = 0; x < N; x++) {
			uint32_t p = buf[y * N + x];
			int a = (p >> 24) & 0xFF;
			int lum = ((((p >> 16) & 0xFF) * 299 + ((p >> 8) & 0xFF) * 587 +
			            (p & 0xFF) * 114) / 1000) * a / 255;
			if (lum > 40) {
				ink++;
				if (x < min_x) {
					min_x = x;
				}
				if (x > max_x) {
					max_x = x;
				}
				if (y < min_y) {
					min_y = y;
				}
				if (y > max_y) {
					max_y = y;
				}
			}
			line[x] = lum > 170 ? '#' : lum > 90 ? '+' : lum > 40 ? '.' : ' ';
		}
		line[N] = 0;
		printf("%s\n", line);
	}
	/* An empty or nearly-empty glyph is a silent failure: everything else
	 * still draws, the app is just unrecognisable. */
	printf("ink=%d %s\n", ink, ink < 120 ? "  <-- SUSPICIOUS: nearly empty" : "");
	if (ink < 120) {
		printf("  FAIL %s: drew almost nothing (%d px)\n", name, ink);
		failures++;
		return;
	}
	/*
	 * Centring. A glyph whose ink sits off to one side is a broken glyph
	 * that still "passes" an ink check, and a broken glyph in the middle of
	 * a home screen is exactly the kind of detail that makes an OS look
	 * unfinished. The tolerance is a tenth of the box, which is generous for
	 * a shape that is meant to be visually centred rather than metrically.
	 */
	int cx = (min_x + max_x) / 2, cy = (min_y + max_y) / 2;
	if (abs(cx - N / 2) > N / 10 || abs(cy - N / 2) > N / 10) {
		printf("  FAIL %s: ink centred at (%d,%d), expected near (%d,%d)\n", name, cx, cy,
		       N / 2, N / 2);
		failures++;
	}
}

int main(void) {
	printf("=== Astrix glyph proof sheet ===\n");
	failures = 0;
	show("HOME", ASTRIX_GLYPH_HOME);
	show("FOLDER", ASTRIX_GLYPH_FOLDER);
	show("TERMINAL", ASTRIX_GLYPH_TERMINAL);
	show("SETTINGS", ASTRIX_GLYPH_SETTINGS);
	show("STORE", ASTRIX_GLYPH_STORE);
	show("ANDROID", ASTRIX_GLYPH_ANDROID);
	show("GRID", ASTRIX_GLYPH_GRID);
	show("POWER", ASTRIX_GLYPH_POWER);
	show("WIFI", ASTRIX_GLYPH_WIFI);
	show("KEYBOARD", ASTRIX_GLYPH_KEYBOARD);
	show("MOON", ASTRIX_GLYPH_MOON);
	show("GLOBE", ASTRIX_GLYPH_GLOBE);
	show("MEMORY", ASTRIX_GLYPH_MEMORY);
	show("CHECK", ASTRIX_GLYPH_CHECK);
	if (failures) {
		printf("\nFAILED: %d glyph problem(s)\n", failures);
		return 1;
	}
	printf("\nok every glyph draws and is centred\n");
	return 0;
}