/*
 * Astrix OS - unit tests for the Astrix Shell's pure logic.
 *
 * Gesture recognition is the highest-risk part of a touch OS: a recogniser
 * that is 10px too eager turns every scroll into an app switch. These tests
 * pin the thresholds down so the behaviour is verified on the build host
 * rather than discovered by hand on a phone-shaped window.
 */

#include "astrix_shell.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0, checks = 0;

#define CHECK(cond, ...)                                                                    \
	do {                                                                                   \
		checks++;                                                                          \
		if (!(cond)) {                                                                     \
			failures++;                                                                    \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                          \
			fprintf(stderr, __VA_ARGS__);                                                 \
			fprintf(stderr, "\n");                                                         \
		}                                                                                  \
	} while (0)

static struct astrix_input_event ev(enum astrix_input_kind k, int x, int y, double t) {
	struct astrix_input_event e = { 0 };
	e.kind = k;
	e.x = x;
	e.y = y;
	e.timestamp = t;
	return e;
}

/* Feed a gesture as a press-move-release sequence and return the last result. */
static enum astrix_gesture swipe(struct astrix_gesture_state *st, int x0, int y0, int x1,
                                 int y1, double t0, int steps) {
	struct astrix_input_event e;
	e = ev(ASTRIX_INPUT_TOUCH_DOWN, x0, y0, t0);
	astrix_gesture_handle(st, &e);
	for (int i = 1; i <= steps; i++) {
		double f = (double)i / steps;
		e = ev(ASTRIX_INPUT_TOUCH_MOTION, x0 + (int)((x1 - x0) * f),
		       y0 + (int)((y1 - y0) * f), t0 + 0.01 * i);
		astrix_gesture_handle(st, &e);
	}
	e = ev(ASTRIX_INPUT_TOUCH_UP, x1, y1, t0 + 0.01 * steps + 0.01);
	return astrix_gesture_handle(st, &e);
}

/* --- tests --------------------------------------------------------------- */

static void test_defaults(void) {
	struct astrix_gesture_config c;
	astrix_gesture_config_defaults(&c);
	CHECK(c.swipe_threshold_px > 0, "swipe threshold must be positive");
	CHECK(c.long_press_ms >= 300, "long press should be at least 300ms");
	CHECK(c.edge_width_px > 0, "edge zone must be positive");
	CHECK(c.touch_slop_px > 0, "slop must be positive");
}

static void test_tap(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	enum astrix_gesture g = swipe(&st, 360, 800, 361, 801, 1.0, 2);
	CHECK(g == ASTRIX_GESTURE_TAP, "a stationary quick press is a tap, got %s",
	      astrix_gesture_name(g));
}

static void test_swipe_up(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	enum astrix_gesture g = swipe(&st, 360, 1200, 360, 400, 1.0, 6);
	CHECK(g == ASTRIX_GESTURE_SWIPE_UP, "upward swipe expected, got %s", astrix_gesture_name(g));
}

static void test_swipe_down(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	enum astrix_gesture g = swipe(&st, 360, 400, 360, 1200, 1.0, 6);
	CHECK(g == ASTRIX_GESTURE_SWIPE_DOWN, "downward swipe expected, got %s",
	      astrix_gesture_name(g));
}

static void test_swipe_horizontal(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	CHECK(swipe(&st, 600, 800, 100, 800, 1.0, 6) == ASTRIX_GESTURE_SWIPE_LEFT,
	      "leftward swipe expected");
	astrix_gesture_init(&st, NULL);
	CHECK(swipe(&st, 100, 800, 600, 800, 1.0, 6) == ASTRIX_GESTURE_SWIPE_RIGHT,
	      "rightward swipe expected");
}

static void test_short_swipe_is_not_a_swipe(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	/* 30px is under the 60px threshold and over the 10px slop: this is a
	 * scroll, not a swipe. Without this distinction every list scroll
	 * would fire a navigation gesture. */
	enum astrix_gesture g = swipe(&st, 360, 800, 360, 770, 1.0, 3);
	CHECK(g == ASTRIX_GESTURE_NONE, "a short drag must not be a swipe, got %s",
	      astrix_gesture_name(g));
}

static void test_long_press(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	struct astrix_input_event e;

	e = ev(ASTRIX_INPUT_TOUCH_DOWN, 200, 400, 1.0);
	astrix_gesture_handle(&st, &e);
	/* Hold still past the long-press duration, then release. */
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 201, 401, 1.6);
	enum astrix_gesture g = astrix_gesture_handle(&st, &e);
	CHECK(g == ASTRIX_GESTURE_LONG_PRESS, "held-still press is a long press, got %s",
	      astrix_gesture_name(g));
}

static void test_long_press_fires_once(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	struct astrix_input_event e;
	int fires = 0;
	e = ev(ASTRIX_INPUT_TOUCH_DOWN, 200, 400, 1.0);
	astrix_gesture_handle(&st, &e);
	/* Keep moving slightly for 1.5s: must not fire repeatedly. */
	for (int i = 0; i < 15; i++) {
		e = ev(ASTRIX_INPUT_TOUCH_MOTION, 200 + (i % 2), 400, 1.0 + 0.1 * i);
		if (astrix_gesture_handle(&st, &e) == ASTRIX_GESTURE_LONG_PRESS) {
			fires++;
		}
	}
	CHECK(fires <= 1, "long press must fire at most once, fired %d times", fires);
}

static void test_swipe_up_hold_is_switcher(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	struct astrix_input_event e;

	/* Press, move up, then hold at the top before releasing: the app
	 * switcher gesture on every phone OS. It is reported while the finger
	 * is still down - the switcher must appear at the end of the hold,
	 * not when the finger finally lifts - and exactly once. */
	e = ev(ASTRIX_INPUT_TOUCH_DOWN, 360, 1300, 1.0);
	CHECK(astrix_gesture_handle(&st, &e) == ASTRIX_GESTURE_NONE, "press reports nothing");
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 360, 600, 1.15);
	CHECK(astrix_gesture_handle(&st, &e) == ASTRIX_GESTURE_NONE,
	      "the swipe itself reports nothing");
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 360, 590, 1.8);
	enum astrix_gesture g = astrix_gesture_handle(&st, &e);
	CHECK(g == ASTRIX_GESTURE_SWIPE_UP_HOLD,
	      "swipe-up-and-hold should open the app switcher, got %s", astrix_gesture_name(g));
	e = ev(ASTRIX_INPUT_TOUCH_UP, 360, 590, 1.9);
	CHECK(astrix_gesture_handle(&st, &e) == ASTRIX_GESTURE_NONE,
	      "the release must not report the latched gesture a second time");
}

static void test_edge_swipe_is_back(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	/* Starts inside the 24px left edge zone and moves right. */
	enum astrix_gesture g = swipe(&st, 8, 800, 90, 800, 1.0, 5);
	CHECK(g == ASTRIX_GESTURE_EDGE_SWIPE, "swipe from the left edge is back, got %s",
	      astrix_gesture_name(g));
}

static void test_mid_screen_horizontal_is_not_back(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	/* Same movement, but starting well away from the edge. */
	enum astrix_gesture g = swipe(&st, 360, 800, 500, 800, 1.0, 5);
	CHECK(g == ASTRIX_GESTURE_SWIPE_RIGHT, "mid-screen right swipe is not a back gesture, got %s",
	      astrix_gesture_name(g));
}

static void test_vertical_swipe_survives_pointer_drift(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);

	/* A relative pointer (QEMU virtio-mouse, most touchpads) drifts in x
	 * while the finger only travels in y. A real, strictly vertical swipe
	 * must still be a swipe-up, or the app switcher never opens. */
	enum astrix_gesture g = swipe(&st, 500, 740, 560, 420, 1.0, 6);
	CHECK(g == ASTRIX_GESTURE_SWIPE_UP,
	      "vertical swipe with 60px of x drift must still be swipe-up, got %s",
	      astrix_gesture_name(g));

	/* Drift that is a real diagonal stays horizontal. */
	astrix_gesture_init(&st, NULL);
	g = swipe(&st, 360, 800, 560, 760, 1.0, 6);
	CHECK(g == ASTRIX_GESTURE_SWIPE_RIGHT,
	      "mostly horizontal swipe must stay swipe-right, got %s",
	      astrix_gesture_name(g));

	/* The app-switcher variant (swipe up and hold) must tolerate the same
	 * drift, and it is latched during motion, not at release. The finger
	 * has to *stop* after the swipe - that is what "and hold" means - so
	 * the second motion stays put. */
	astrix_gesture_init(&st, NULL);
	struct astrix_input_event e;
	e = ev(ASTRIX_INPUT_TOUCH_DOWN, 500, 740, 1.0);
	astrix_gesture_handle(&st, &e);
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 540, 420, 1.15);
	astrix_gesture_handle(&st, &e);
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 542, 419, 1.8);
	g = astrix_gesture_handle(&st, &e);
	CHECK(g == ASTRIX_GESTURE_SWIPE_UP_HOLD,
	      "swipe-up-and-hold with x drift must still open the switcher, got %s",
	      astrix_gesture_name(g));
	e = ev(ASTRIX_INPUT_TOUCH_UP, 542, 419, 1.9);
	CHECK(astrix_gesture_handle(&st, &e) == ASTRIX_GESTURE_NONE,
	      "the release must not report the latched gesture a second time");
}

/*
 * "Swipe up and hold" must mean the finger stops. Timing the hold from the
 * start of the touch instead means any swipe slow enough to outlive
 * long_press_ms satisfies it - and on a loaded machine that is every swipe.
 * On a booted VM this turned a swipe meant to dismiss a recents card into a
 * swipe-up-and-hold, so the switcher just stayed open and nothing closed.
 */
static void test_slow_swipe_is_not_a_hold(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	struct astrix_input_event e;

	e = ev(ASTRIX_INPUT_TOUCH_DOWN, 512, 744, 1.0);
	astrix_gesture_handle(&st, &e);
	/* Waypoints 700ms apart: the whole swipe takes well over long_press_ms
	 * and the finger never stops. */
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 512, 640, 1.7);
	CHECK(astrix_gesture_handle(&st, &e) == ASTRIX_GESTURE_NONE,
	      "a swipe in progress is not a hold");
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 512, 540, 2.4);
	CHECK(astrix_gesture_handle(&st, &e) == ASTRIX_GESTURE_NONE,
	      "a slow swipe must not latch the switcher halfway through");
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 512, 440, 3.1);
	CHECK(astrix_gesture_handle(&st, &e) == ASTRIX_GESTURE_NONE,
	      "nor at the end of the travel");
	e = ev(ASTRIX_INPUT_TOUCH_UP, 512, 440, 3.2);
	enum astrix_gesture g = astrix_gesture_handle(&st, &e);
	CHECK(g == ASTRIX_GESTURE_SWIPE_UP,
	      "a slow swipe with no hold must still be a plain swipe-up, got %s",
	      astrix_gesture_name(g));
}

/*
 * The gesture recogniser fires long press on a timer, and under emulation one
 * injected waypoint can take tens of milliseconds - so a slow swipe crosses
 * the hold threshold while the finger is still inside the slop. The hold must
 * then be revoked when the finger travels, or the swipe is lost and the app
 * switcher never opens. This is the exact shape of the failure seen on a
 * booted VM, where a real upward swipe logged "long-press" and nothing else.
 */
static void test_long_press_is_revoked_by_later_travel(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	struct astrix_input_event e;

	e = ev(ASTRIX_INPUT_TOUCH_DOWN, 512, 744, 1.0);
	astrix_gesture_handle(&st, &e);
	/* Slow first waypoint: past the hold threshold, still inside the slop. */
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 512, 736, 1.6);
	CHECK(astrix_gesture_handle(&st, &e) == ASTRIX_GESTURE_LONG_PRESS,
	      "a held, nearly-still press is a long press");
	/* Now it travels: the hold is revoked and the swipe is recognised. */
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 514, 600, 1.9);
	enum astrix_gesture g = astrix_gesture_handle(&st, &e);
	CHECK(g != ASTRIX_GESTURE_LONG_PRESS, "the premature hold must be revoked");
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 516, 423, 2.2);
	g = astrix_gesture_handle(&st, &e);
	CHECK(g != ASTRIX_GESTURE_LONG_PRESS,
	      "a revoked hold must not fire again later in the same contact");
	e = ev(ASTRIX_INPUT_TOUCH_MOTION, 516, 422, 2.9);
	g = astrix_gesture_handle(&st, &e);
	CHECK(g == ASTRIX_GESTURE_SWIPE_UP_HOLD,
	      "stopping after the swipe must still reach the switcher, got %s",
	      astrix_gesture_name(g));
	e = ev(ASTRIX_INPUT_TOUCH_UP, 516, 422, 3.0);
	CHECK(astrix_gesture_handle(&st, &e) == ASTRIX_GESTURE_NONE,
	      "and the release must not report it twice");
}

static void test_double_tap(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	double t = 1.0;
	enum astrix_gesture first = swipe(&st, 360, 800, 360, 800, t, 1);
	CHECK(first == ASTRIX_GESTURE_TAP, "first tap should be a tap, got %s",
	      astrix_gesture_name(first));
	t = 1.1;
	enum astrix_gesture second = swipe(&st, 360, 800, 360, 800, t, 1);
	CHECK(second == ASTRIX_GESTURE_DOUBLE_TAP, "second quick tap is a double tap, got %s",
	      astrix_gesture_name(second));
}

static void test_slow_second_tap_is_not_double(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	swipe(&st, 360, 800, 360, 800, 1.0, 1);
	/* Well beyond double_tap_ms: must be a fresh tap, not a double tap. */
	enum astrix_gesture g = swipe(&st, 360, 800, 360, 800, 5.0, 1);
	CHECK(g == ASTRIX_GESTURE_TAP, "a slow second tap is a tap, got %s", astrix_gesture_name(g));
}

static void test_distant_taps_not_double(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	swipe(&st, 100, 200, 100, 200, 1.0, 1);
	/* Quick in time, but far away in space. */
	enum astrix_gesture g = swipe(&st, 600, 1400, 600, 1400, 1.05, 1);
	CHECK(g == ASTRIX_GESTURE_TAP, "distant quick tap is not a double tap, got %s",
	      astrix_gesture_name(g));
}

static void test_multi_finger_no_gesture(void) {
	struct astrix_gesture_state st;
	astrix_gesture_init(&st, NULL);
	struct astrix_input_event e;
	/* Two fingers down; the first finger's release must not complete a
	 * gesture while a second is still down. */
	e = ev(ASTRIX_INPUT_TOUCH_DOWN, 300, 900, 1.0);
	astrix_gesture_handle(&st, &e);
	e = ev(ASTRIX_INPUT_TOUCH_DOWN, 400, 900, 1.05);
	astrix_gesture_handle(&st, &e);
	e = ev(ASTRIX_INPUT_TOUCH_UP, 300, 900, 1.1);
	enum astrix_gesture g = astrix_gesture_handle(&st, &e);
	CHECK(g == ASTRIX_GESTURE_NONE, "lifting one of two fingers is not a gesture, got %s",
	      astrix_gesture_name(g));
}

static void test_gesture_names_unique(void) {
	/* Every gesture must have a distinct name for logs and settings. */
	const char *seen[16];
	int n = 0;
	for (int g = 0; g <= ASTRIX_GESTURE_TWO_FINGER_SWIPE; g++) {
		const char *nm = astrix_gesture_name((enum astrix_gesture)g);
		for (int i = 0; i < n; i++) {
			CHECK(strcmp(seen[i], nm) != 0, "duplicate gesture name '%s'", nm);
		}
		if (n < 16) {
			seen[n++] = nm;
		}
	}
}

/* --- app registry -------------------------------------------------------- */

static void test_app_registry(void) {
	struct astrix_shell *sh = astrix_shell_create(720, 1600);
	CHECK(sh != NULL, "shell should be created");
	if (!sh) {
		return;
	}
	struct astrix_app a = { 0 };
	snprintf(a.id, sizeof(a.id), "org.astrix.Terminal");
	snprintf(a.name, sizeof(a.name), "Terminal");
	snprintf(a.exec, sizeof(a.exec), "astrix-terminal");
	a.kind = ASTRIX_APP_SYSTEM;
	int idx = astrix_shell_add_app(sh, &a);
	CHECK(idx == 0, "first app should get index 0, got %d", idx);
	CHECK(sh->app_count == 1, "app_count should be 1, got %d", sh->app_count);

	struct astrix_app b = { 0 };
	snprintf(b.id, sizeof(b.id), "com.example.android");
	snprintf(b.name, sizeof(b.name), "Example");
	b.kind = ASTRIX_APP_ANDROID;
	snprintf(b.android_activity, sizeof(b.android_activity), ".MainActivity");
	astrix_shell_add_app(sh, &b);
	CHECK(sh->app_count == 2, "should have 2 apps, got %d", sh->app_count);
	/* Native and Android apps must be indistinguishable to the user: both
	 * appear in the same list. */
	CHECK(sh->apps[1].kind == ASTRIX_APP_ANDROID, "kind should be preserved");

	astrix_shell_destroy(sh);
}

static void test_app_capacity(void) {
	struct astrix_shell *sh = astrix_shell_create(720, 1600);
	if (!sh) {
		return;
	}
	char id[64];
	int added = 0;
	for (int i = 0; i < ASTRIX_MAX_APPS + 10; i++) {
		struct astrix_app a = { 0 };
		snprintf(id, sizeof(id), "org.test.app%d", i);
		snprintf(a.id, sizeof(a.id), "%s", id);
		snprintf(a.name, sizeof(a.name), "App %d", i);
		if (astrix_shell_add_app(sh, &a) >= 0) {
			added++;
		}
	}
	CHECK(added == ASTRIX_MAX_APPS, "registry must cap at %d apps, added %d", ASTRIX_MAX_APPS,
	      added);
	CHECK(sh->app_count == ASTRIX_MAX_APPS, "app_count must respect the cap, got %d",
	      sh->app_count);
	astrix_shell_destroy(sh);
}

static void test_notifications(void) {
	struct astrix_shell *sh = astrix_shell_create(720, 1600);
	if (!sh) {
		return;
	}
	int n = astrix_shell_notify(sh, "org.astrix.Mail", "Mail", "New message",
	                            "You have 3 unread messages", ASTRIX_NOTIF_NORMAL);
	CHECK(n >= 0, "notification should be accepted");
	CHECK(sh->notification_count == 1, "should hold 1 notification, got %d",
	      sh->notification_count);
	CHECK(astrix_shell_unread_count(sh) == 1, "unread count should be 1");

	/* A critical notification outranks a normal one. */
	astrix_shell_notify(sh, "org.astrix.System", "System", "Low battery",
	                    "5% remaining", ASTRIX_NOTIF_CRITICAL);
	CHECK(sh->notification_count == 2, "should hold 2 notifications, got %d",
	      sh->notification_count);

	astrix_shell_dismiss_notification(sh, 0);
	CHECK(sh->notification_count == 1, "dismiss should remove one, got %d",
	      sh->notification_count);
	CHECK(sh->notifications[0].priority == ASTRIX_NOTIF_CRITICAL,
	      "dismiss should preserve the remaining notification");

	astrix_shell_clear_notifications(sh);
	CHECK(sh->notification_count == 0, "clear should empty the list, got %d",
	      sh->notification_count);
	astrix_shell_destroy(sh);
}

static void test_notification_capacity(void) {
	struct astrix_shell *sh = astrix_shell_create(720, 1600);
	if (!sh) {
		return;
	}
	for (int i = 0; i < ASTRIX_MAX_NOTIFICATIONS + 20; i++) {
		char t[64];
		snprintf(t, sizeof(t), "Notification %d", i);
		astrix_shell_notify(sh, "org.test", "Test", t, "body", ASTRIX_NOTIF_LOW);
	}
	CHECK(sh->notification_count <= ASTRIX_MAX_NOTIFICATIONS,
	      "notification list must be capped, got %d", sh->notification_count);
	astrix_shell_destroy(sh);
}

static void test_navigation(void) {
	struct astrix_shell *sh = astrix_shell_create(720, 1600);
	if (!sh) {
		return;
	}
	struct astrix_app a = { 0 };
	snprintf(a.id, sizeof(a.id), "org.astrix.Terminal");
	snprintf(a.name, sizeof(a.name), "Terminal");
	astrix_shell_add_app(sh, &a);

	astrix_shell_go_home(sh);
	CHECK(sh->screen == ASTRIX_SCREEN_HOME, "go_home should show the home screen");

	astrix_shell_open_app(sh, 0);
	CHECK(sh->screen == ASTRIX_SCREEN_APP, "opening an app should switch to the app screen");
	CHECK(sh->foreground_app == 0, "foreground_app should be 0, got %d", sh->foreground_app);
	/*
	 * The shell does not fork; it asks whoever holds the compositor
	 * connection to start the app. This is the whole of that contract: the
	 * request is recorded, and it is recorded for the app that was tapped
	 * and not for some other one.
	 */
	CHECK(sh->launch_pending == 0, "opening an app should request a launch, got %d",
	      sh->launch_pending);

	astrix_shell_close_app(sh, 0);
	CHECK(sh->launch_pending == -1,
	      "closing a pending app should withdraw the launch request, got %d",
	      sh->launch_pending);
	astrix_shell_open_app(sh, 0);

	astrix_shell_go_home(sh);
	CHECK(sh->screen == ASTRIX_SCREEN_HOME, "going home should leave the app screen");

	astrix_shell_toggle_lock(sh);
	CHECK(sh->locked, "toggle_lock should lock");
	CHECK(sh->screen == ASTRIX_SCREEN_LOCK, "locking should show the lock screen");
	astrix_shell_toggle_lock(sh);
	CHECK(!sh->locked, "toggle_lock should unlock");
	astrix_shell_destroy(sh);
}

/*
 * App lifecycle bookkeeping.
 *
 * The seam being pinned here is the whole point of the lifecycle work:
 * the shell model never forks and never calls kill(), it only records what
 * the display owner did. So every claim the UI makes about an app - running,
 * which pid, "please terminate this" - has to be a field the model owns, or
 * it cannot be tested without a compositor.
 */
static void test_app_lifecycle(void) {
	struct astrix_shell *sh = astrix_shell_create(720, 1600);
	if (!sh) {
		return;
	}
	struct astrix_app a = { 0 };
	snprintf(a.id, sizeof(a.id), "org.astrix.Files");
	snprintf(a.name, sizeof(a.name), "Files");
	astrix_shell_add_app(sh, &a);
	struct astrix_app b = { 0 };
	snprintf(b.id, sizeof(b.id), "org.astrix.Terminal");
	snprintf(b.name, sizeof(b.name), "Terminal");
	astrix_shell_add_app(sh, &b);
	CHECK(sh->app_count == 2, "two distinct apps should be registered, got %d", sh->app_count);

	CHECK(sh->apps[0].pid == 0, "a fresh app must have no pid, got %d", sh->apps[0].pid);
	CHECK(!sh->apps[0].running, "a fresh app must not claim to be running");

	/*
	 * Tapping an icon asks for a launch but does not make the app
	 * running. Claiming "running" here is what made a failed fork look
	 * like a successful launch.
	 */
	astrix_shell_open_app(sh, 0);
	CHECK(sh->launch_pending == 0, "tap should request a launch, got %d", sh->launch_pending);
	CHECK(!sh->apps[0].running,
	      "an app must not be running before a pid was recorded (fork may fail)");

	/* The display owner forked and reports the pid. */
	astrix_shell_set_app_pid(sh, 0, 4242);
	CHECK(sh->apps[0].running, "recording a pid must mark the app running");
	CHECK(sh->apps[0].pid == 4242, "pid should be recorded, got %d", sh->apps[0].pid);

	/* An untracked pid must not disturb a tracked app. */
	CHECK(!astrix_shell_note_app_exit(sh, 9999),
	      "an unknown pid must not be reported as an app exit");
	CHECK(sh->apps[0].running, "an unknown pid must not clear a running app");
	CHECK(sh->apps[0].pid == 4242, "an unknown pid must not clear a recorded pid");

	/* Closing requests a termination of the real process. */
	astrix_shell_close_app(sh, 0);
	CHECK(sh->kill_pending == 0, "closing a running app should request a kill, got %d",
	      sh->kill_pending);
	CHECK(!sh->apps[0].running, "a closed app must not stay running");
	CHECK(sh->apps[0].pid == 0, "a closed app must not keep its pid");
	CHECK(sh->kill_pending != 0 || sh->apps[0].pid == 0,
	      "close must either request a kill or have no process to kill");
	CHECK(sh->screen == ASTRIX_SCREEN_HOME, "closing the foreground app returns home");

	/*
	 * The pid to signal must be captured by the close itself.
	 *
	 * This is not a detail. `close_app` clears apps[i].pid on the very
	 * next line, because the app is no longer running from the UI's point
	 * of view. The process that actually sends the signal is a different
	 * translation unit, and if it looks the pid up from apps[i] it gets
	 * zero - drops the request without a word, and the app keeps running
	 * with its surface mapped while the launcher shows it closed. On a
	 * booted VM that is exactly what happened: the app switcher reported
	 * the card as hit, the screen went home, and no signal was ever sent.
	 */
	CHECK(sh->kill_pending_pid == 4242,
	      "the close must capture the pid to signal, got %d (the app entry is cleared to %d)",
	      sh->kill_pending_pid, sh->apps[0].pid);

	/*
	 * The owner consumed the request. It must not be possible to signal
	 * the same app again, because by then the pid could have been reused
	 * by an unrelated process.
	 */
	sh->kill_pending = -1;
	sh->kill_pending_pid = 0;

	/* A failed launch: the owner reports no pid. */
	astrix_shell_open_app(sh, 1);
	CHECK(sh->launch_pending == 1, "second app should request a launch");
	astrix_shell_set_app_pid(sh, 1, 0);
	CHECK(!sh->apps[1].running, "a launch that produced no pid must not be 'running'");
	CHECK(sh->foreground_app != 1,
	      "an app with no process must not stay in the foreground, got %d", sh->foreground_app);

	/* A crash is reported through the same path as a clean exit. */
	astrix_shell_open_app(sh, 0);
	astrix_shell_set_app_pid(sh, 0, 7777);
	astrix_shell_open_app(sh, 0);
	CHECK(sh->foreground_app == 0, "app 0 should be foreground, got %d", sh->foreground_app);
	CHECK(astrix_shell_note_app_exit(sh, 7777),
	      "the exit of a tracked pid must be reported as handled");
	CHECK(!sh->apps[0].running, "a crashed app must not stay running");
	CHECK(sh->apps[0].pid == 0, "a crashed app must not keep its pid");
	CHECK(sh->foreground_app == -1,
	      "a crashed foreground app must be dropped, got %d", sh->foreground_app);
	CHECK(sh->screen == ASTRIX_SCREEN_HOME, "a crashed foreground app returns home");
	CHECK(!astrix_shell_note_app_exit(sh, 7777), "a second reap of the same pid must be a no-op");

	/*
	 * Re-registering an app (a reinstall, a rescanned desktop entry) must
	 * not orphan a running process. Losing the pid here left the app
	 * marked running with nothing left to signal - unkillable, and
	 * permanently in the app switcher.
	 */
	astrix_shell_open_app(sh, 0);
	astrix_shell_set_app_pid(sh, 0, 5555);
	struct astrix_app refreshed = { 0 };
	snprintf(refreshed.id, sizeof(refreshed.id), "org.astrix.Files");
	snprintf(refreshed.name, sizeof(refreshed.name), "Files (new)");
	astrix_shell_add_app(sh, &refreshed);
	CHECK(sh->app_count == 2, "a rediscovery must not add a second entry, got %d", sh->app_count);
	CHECK(sh->apps[0].pid == 5555,
	      "a rediscovered app must keep its pid, got %d", sh->apps[0].pid);
	CHECK(sh->apps[0].running, "a rediscovered running app must stay running");
	astrix_shell_close_app(sh, 0);
	CHECK(sh->kill_pending == 0, "a rediscovered running app must still be killable, got %d",
	      sh->kill_pending);

	astrix_shell_destroy(sh);
}

static void test_resize(void) {
	struct astrix_shell *sh = astrix_shell_create(720, 1600);
	if (!sh) {
		return;
	}
	CHECK(sh->width == 720 && sh->height == 1600, "initial size wrong");
	astrix_shell_set_size(sh, 1080, 2400);
	CHECK(sh->width == 1080 && sh->height == 2400, "size not updated after resize");
	CHECK(sh->canvas.width == 1080, "canvas width should follow the shell size, got %d",
	      sh->canvas.width);
	astrix_shell_destroy(sh);
}

int main(void) {
	test_defaults();
	test_tap();
	test_swipe_up();
	test_swipe_down();
	test_swipe_horizontal();
	test_short_swipe_is_not_a_swipe();
	test_long_press();
	test_long_press_fires_once();
	test_swipe_up_hold_is_switcher();
	test_edge_swipe_is_back();
	test_mid_screen_horizontal_is_not_back();
	test_vertical_swipe_survives_pointer_drift();
	test_long_press_is_revoked_by_later_travel();
	test_slow_swipe_is_not_a_hold();
	test_double_tap();
	test_slow_second_tap_is_not_double();
	test_distant_taps_not_double();
	test_multi_finger_no_gesture();
	test_gesture_names_unique();
	test_app_registry();
	test_app_capacity();
	test_notifications();
	test_notification_capacity();
	test_navigation();
	test_app_lifecycle();
	test_resize();

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
