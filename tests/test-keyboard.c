/*
 * Astrix OS - host test for the system on-screen keyboard.
 *
 * Why this exists
 * ---------------
 * An on-screen keyboard is the one piece of UI where "it drew something" and
 * "it works" are completely different claims. The hard part is not the
 * drawing - it is that a tap has to become the right character, that shift has
 * to be one-shot like a real keyboard, and that a tap on the keyboard must
 * never also fall through to the home screen behind it.
 *
 * The delivery half (codepoint -> xkb keycode -> compositor) cannot be tested
 * here, and this test is explicit about that: what it pins is the decision
 * half, in keyboard.c, plus the geometry the renderer draws from. If the two
 * ever disagree about where a key is, this fails.
 *
 * Host test: no compositor, no Wayland, no QEMU.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "keyboard.h"

static int failures;

static void check(bool ok, const char *what) {
	printf("%s %s\n", ok ? "  +" : "  x", what);
	if (!ok) {
		failures++;
	}
}

/*
 * Tap a key by its label, wherever the layout put it.
 *
 * Case-insensitive on purpose: with shift on, the key labelled 'q' is
 * labelled 'Q', and a helper that insisted on 'q' would quietly fail to find
 * it. The label is a position, not a value.
 */
static bool tap_label(struct astrix_shell *sh, const char *label) {
	struct astrix_kbd_key keys[ASTRIX_KBD_MAX_KEYS];
	int n = astrix_kbd_layout(sh, sh->width, sh->height, keys,
	                           ASTRIX_KBD_MAX_KEYS);
	for (int i = 0; i < n; i++) {
		if (strcasecmp(keys[i].label, label) == 0) {
			return astrix_kbd_tap(sh, keys[i].rect.x + keys[i].rect.w / 2,
			                      keys[i].rect.y + keys[i].rect.h / 2);
		}
	}
	return false;
}

static bool pop_is(struct astrix_shell *sh, enum astrix_kbd_action action,
                   uint32_t cp) {
	enum astrix_kbd_action a;
	uint32_t c;
	if (!astrix_kbd_pop(sh, &a, &c)) {
		return false;
	}
	return a == action && (action != ASTRIX_KBD_CHAR || c == cp);
}

int main(void) {
	/* A portrait phone panel. The keyboard starts closed, like it does on a
	 * real session; every section below opens it explicitly rather than
	 * relying on a default that would hide a mistake. */
	struct astrix_shell *sh = astrix_shell_create(720, 1600);
	sh->kbd_visible = true;

	printf("  geometry\n");
	struct astrix_rect area = astrix_kbd_area(720, 1600);
	check(area.y + area.h == 1600,
	      "the keyboard reaches the bottom of the panel");
	check(area.h >= 140 && area.h <= 1600 / 2,
	      "it takes a sensible share of the screen");
	/* The nav bar/dock sits at the bottom; a keyboard that overlaps it
	 * produces phantom taps on the icons behind. */
	check(area.y > 1600 / 2, "it does not reach up into the dock");

	printf("  layout\n");
	int n = astrix_kbd_layout(sh, 720, 1600, NULL, 0);
	check(n == 0, "a NULL output buffer yields no keys instead of crashing");
	struct astrix_kbd_key keys[ASTRIX_KBD_MAX_KEYS];
	n = astrix_kbd_layout(sh, 720, 1600, keys, ASTRIX_KBD_MAX_KEYS);
	check(n > 20, "a full keyboard is laid out");
	check(n <= ASTRIX_KBD_MAX_KEYS, "it fits in the advertised array");
	bool all_inside = true;
	for (int i = 0; i < n; i++) {
		if (keys[i].rect.x < 0 || keys[i].rect.y < area.y ||
		    keys[i].rect.x + keys[i].rect.w > 720 ||
		    keys[i].rect.y + keys[i].rect.h > 1600) {
			all_inside = false;
		}
	}
	check(all_inside, "every key is inside the keyboard area");

	/* The draw pass and the hit-test read the same layout, so a hit-test on
	 * a key's own centre must find that key back. */
	bool hit_ok = true;
	for (int i = 0; i < n; i++) {
		struct astrix_kbd_key got;
		int idx = astrix_kbd_hit(sh, 720, 1600,
		                         keys[i].rect.x + keys[i].rect.w / 2,
		                         keys[i].rect.y + keys[i].rect.h / 2, &got);
		if (idx != i) {
			hit_ok = false;
		}
	}
	check(hit_ok, "a tap on a key's centre hit-tests back to that key");

	printf("  typing\n");
	check(tap_label(sh, "q"), "tapping q is consumed by the keyboard");
	check(pop_is(sh, ASTRIX_KBD_CHAR, 'q'), "and it queues the character q");
	check(tap_label(sh, "w"), "tapping w is consumed");
	check(pop_is(sh, ASTRIX_KBD_CHAR, 'w'), "and it queues w");
	check(sh->kbd_queue_len == 0, "the queue drains in order");
	check(!astrix_kbd_pop(sh, NULL, NULL), "an empty queue reports empty");

	printf("  shift\n");
	tap_label(sh, "shift");
	tap_label(sh, "q");
	check(pop_is(sh, ASTRIX_KBD_CHAR, 'Q'), "shift+q gives an uppercase Q");
	check(!sh->kbd_shift, "shift is one-shot, like a real keyboard");
	tap_label(sh, "q");
	check(pop_is(sh, ASTRIX_KBD_CHAR, 'q'), "and the next q is lowercase again");

	tap_label(sh, "shift");
	tap_label(sh, "shift");
	check(sh->kbd_caps, "pressing shift twice locks caps");
	tap_label(sh, "q");
	check(pop_is(sh, ASTRIX_KBD_CHAR, 'Q'), "caps q is uppercase");
	tap_label(sh, "shift");
	check(!sh->kbd_caps && !sh->kbd_shift, "and pressing shift releases it");

	printf("  modifiers\n");
	tap_label(sh, "space");
	check(pop_is(sh, ASTRIX_KBD_SPACE, 0), "space queues a space");
	tap_label(sh, "return");
	check(pop_is(sh, ASTRIX_KBD_ENTER, 0), "return queues an enter");
	tap_label(sh, "bksp");
	check(pop_is(sh, ASTRIX_KBD_BACKSPACE, 0), "backspace queues a backspace");

	printf("  symbol layer\n");
	tap_label(sh, "?123");
	check(sh->kbd_symbols, "the symbol layer turns on");
	/* On the symbol layer the digit row is !@#$%^&*()_+, so the first key
	 * is '!' and the character must not still be the digit. */
	check(tap_label(sh, "!"), "the symbol layer's first key is '!'");
	check(pop_is(sh, ASTRIX_KBD_CHAR, '!'),
	      "and it queues '!', not '1'");
	tap_label(sh, "?123");
	check(!sh->kbd_symbols, "and it turns back off");
	check(tap_label(sh, "q"), "the letter layer is back");
	check(pop_is(sh, ASTRIX_KBD_CHAR, 'q'),
	      "and it queues 'q' again");

	printf("  hiding\n");
	tap_label(sh, "hide");
	check(!sh->kbd_visible, "the hide key closes the keyboard");
	check(!astrix_kbd_tap(sh, 100, 1500),
	      "a closed keyboard consumes nothing");
	check(sh->kbd_queue_len == 0, "hiding queues no key");

	printf("  taps do not leak to the UI behind\n");
	sh->kbd_visible = true;
	sh->kbd_press_x = -1;
	/* Above the keyboard is the app's own area, so that tap belongs to the
	 * app - a phone does not steal taps from the top half of the screen. */
	check(!astrix_kbd_tap(sh, 360, 300),
	      "a tap above the keyboard belongs to the app behind it");
	/* Inside the keyboard but in the gap above the first row is ambiguous,
	 * and ambiguity is where phantom taps come from: it is swallowed. */
	struct astrix_rect ka = astrix_kbd_area(720, 1600);
	check(astrix_kbd_tap(sh, 360, ka.y + 1),
	      "a tap in the keyboard's gap is swallowed");
	check(sh->kbd_queue_len == 0,
	      "and queues nothing, so it cannot reach the home screen behind it");

	printf("  the status bar button\n");
	sh->kbd_visible = false;
	struct astrix_rect btn = astrix_kbd_toggle_rect(sh);
	check(btn.w >= 20 && btn.h >= 20, "the button is a legal touch target");
	check(astrix_kbd_toggle(sh, btn.x + 2, btn.y + 2), "tapping it opens");
	check(sh->kbd_visible, "the keyboard opened");
	check(astrix_kbd_toggle(sh, btn.x + 2, btn.y + 2), "tapping it again closes");
	check(!sh->kbd_visible, "the keyboard closed");
	check(!astrix_kbd_toggle(sh, 0, 0), "tapping elsewhere does not");

	printf("  panel sizes\n");
	int sizes[][2] = { { 720, 1600 }, { 1080, 2400 }, { 1024, 768 }, { 480, 800 } };
	bool all_ok = true;
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		struct astrix_shell *s2 = astrix_shell_create(sizes[i][0], sizes[i][1]);
		s2->kbd_visible = true;
		struct astrix_kbd_key k2[ASTRIX_KBD_MAX_KEYS];
		int n2 = astrix_kbd_layout(s2, sizes[i][0], sizes[i][1], k2,
		                            ASTRIX_KBD_MAX_KEYS);
		if (n2 < 20) {
			all_ok = false;
		}
		for (int j = 0; j < n2; j++) {
			if (k2[j].rect.h <= 0 || k2[j].rect.w <= 0) {
				all_ok = false;
			}
		}
		astrix_shell_destroy(s2);
	}
	check(all_ok, "every key has a positive size on every panel");

	astrix_shell_destroy(sh);
	if (failures) {
		printf("FAILED: %d keyboard check(s)\n", failures);
		return 1;
	}
	printf("all on-screen keyboard checks passed\n");
	return 0;
}