/*
 * Astrix OS - touch gesture recogniser.
 *
 * Pure logic: no Wayland, no allocation, no I/O. That is deliberate. Gesture
 * handling is where a touch OS actually lives, and it is the part most likely
 * to be wrong, so it must be unit testable on the build host rather than only
 * by squinting at a phone-shaped window in QEMU.
 *
 * The recogniser maps a stream of pointer events onto the gestures a phone
 * user expects:
 *
 *   swipe up           -> home / app drawer
 *   swipe up + hold    -> app switcher
 *   swipe down         -> notification shade / quick settings
 *   swipe from edge    -> back
 *   tap                -> activate
 *   double tap         -> (currently: go home, a common phone idiom)
 *   long press         -> contextual menu / app actions
 *   pinch              -> zoom where a view supports it
 *
 * All of these are recognised, but which ones are *bound to an action* is the
 * shell's decision, not the recogniser's.
 */

#include "astrix_shell.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void astrix_gesture_config_defaults(struct astrix_gesture_config *cfg) {
	if (!cfg) {
		return;
	}
	/*
	 * Defaults are chosen for a ~720x1600 portrait panel. On a real device
	 * the shell scales these by the display's physical size so behaviour is
	 * the same on a 5" and a 6.7" phone (see astrix_shell_create).
	 */
	cfg->swipe_threshold_px = 60;
	cfg->long_press_ms = 500;
	cfg->double_tap_ms = 300;
	cfg->edge_width_px = 24;
	cfg->touch_slop_px = 10;
	cfg->long_press_slop_px = 12;
	cfg->vertical_dominance_pct = 120;
	cfg->pinch_scale_trigger = 1.35f;
}

void astrix_gesture_init(struct astrix_gesture_state *st,
                         const struct astrix_gesture_config *cfg) {
	if (!st) {
		return;
	}
	memset(st, 0, sizeof(*st));
	if (cfg) {
		st->cfg = *cfg;
	} else {
		astrix_gesture_config_defaults(&st->cfg);
	}
}

const char *astrix_gesture_name(enum astrix_gesture g) {
	switch (g) {
	case ASTRIX_GESTURE_NONE: return "none";
	case ASTRIX_GESTURE_TAP: return "tap";
	case ASTRIX_GESTURE_DOUBLE_TAP: return "double-tap";
	case ASTRIX_GESTURE_LONG_PRESS: return "long-press";
	case ASTRIX_GESTURE_SWIPE_UP: return "swipe-up";
	case ASTRIX_GESTURE_SWIPE_DOWN: return "swipe-down";
	case ASTRIX_GESTURE_SWIPE_LEFT: return "swipe-left";
	case ASTRIX_GESTURE_SWIPE_RIGHT: return "swipe-right";
	case ASTRIX_GESTURE_SWIPE_UP_HOLD: return "swipe-up-hold";
	case ASTRIX_GESTURE_EDGE_SWIPE: return "edge-swipe";
	case ASTRIX_GESTURE_PINCH: return "pinch";
	case ASTRIX_GESTURE_TWO_FINGER_SWIPE: return "two-finger-swipe";
	}
	return "unknown";
}

static int64_t abs_dist(int a, int b) {
	int d = a - b;
	return d < 0 ? -d : d;
}

static int64_t abs64(int64_t v) {
	return v < 0 ? -v : v;
}

/*
 * True when the motion is vertical rather than horizontal.
 *
 * A strict "ady > adx" test is too brittle for a real finger, and it is
 * actively wrong for a *relative* pointer (virtio-mouse in QEMU, and most
 * touchpads): the reported x drifts while the finger only travels in y, so a
 * genuinely vertical swipe gets classified as a horizontal one and the app
 * switcher never opens. Requiring the vertical component to exceed the
 * horizontal one by a margin absorbs that drift without turning a real
 * diagonal into a vertical swipe.
 */
static bool vertical_dominant(const struct astrix_gesture_config *cfg,
                               int64_t adx, int64_t ady) {
	int pct = cfg->vertical_dominance_pct > 0 ? cfg->vertical_dominance_pct : 120;
	return ady * 100 >= adx * (int64_t)pct;
}

/*
 * Convert an event timestamp to milliseconds.
 *
 * astrix_input_event.timestamp is a double count of *seconds* (that is what
 * wlroots' time_msec/1000.0 gives us). Every threshold in the recogniser is
 * expressed in milliseconds, so the conversion must happen here. Truncating
 * the seconds value directly would make a 1.9-second hold look like 1ms.
 */
static int64_t ev_ms(const struct astrix_input_event *ev) {
	return (int64_t)(ev->timestamp * 1000.0 + 0.5);
}

static bool is_edge_start(const struct astrix_gesture_state *st, int x) {
	return x <= st->cfg.edge_width_px;
}

/*
 * Classify a completed touch sequence. Split out from the event handler so
 * the decision is readable in one place: given where the touch started, where
 * it ended and how long it took, what did the user mean?
 */
static enum astrix_gesture classify_release(struct astrix_gesture_state *st, int x, int y,
                                            int64_t dt, int64_t now) {
	int64_t dx = (int64_t)x - st->start_x;
	int64_t dy = (int64_t)y - st->start_y;
	int64_t adx = dx < 0 ? -dx : dx;
	int64_t ady = dy < 0 ? -dy : dy;
	int64_t travel = (int64_t)sqrt((double)((double)(dx * dx + dy * dy)));

	/* --- Swipes ------------------------------------------------------ */
	if (travel >= st->cfg.swipe_threshold_px) {
		bool vertical = vertical_dominant(&st->cfg, adx, ady);

		/* Upward plus a sustained hold is the app-switcher gesture. The hold
		 * is measured from the last movement, so a slow swipe that never
		 * pauses stays a plain swipe-up. */
		if (dy < 0 && vertical) {
			int64_t still_for = dt - (st->moved_ms - st->start_ms);
			if (still_for >= st->cfg.long_press_ms && !st->long_press_fired) {
				return ASTRIX_GESTURE_SWIPE_UP_HOLD;
			}
			return ASTRIX_GESTURE_SWIPE_UP;
		}
		if (dy > 0 && vertical) {
			return ASTRIX_GESTURE_SWIPE_DOWN;
		}
		/* A horizontal drag that started in the edge zone is "back". */
		if (is_edge_start(st, st->start_x) && adx > ady) {
			return ASTRIX_GESTURE_EDGE_SWIPE;
		}
		/* Anything left over is horizontal: prefer the axis the finger
		 * actually travelled along over the dominance test. */
		if (dx < 0) {
			return ASTRIX_GESTURE_SWIPE_LEFT;
		}
		if (dx > 0) {
			return ASTRIX_GESTURE_SWIPE_RIGHT;
		}
		return ASTRIX_GESTURE_NONE;
	}

	/* --- Long press: held, essentially still -------------------------- */
	if (!st->long_press_fired && dt >= st->cfg.long_press_ms &&
	    travel <= st->cfg.long_press_slop_px) {
		return ASTRIX_GESTURE_LONG_PRESS;
	}

	/* A long press already consumed this touch: releasing it must not also
	 * register as a tap. */
	if (st->long_press_fired) {
		return ASTRIX_GESTURE_NONE;
	}

	/* --- Tap / double tap --------------------------------------------- */
	if (travel <= st->cfg.touch_slop_px) {
		/* Double tap requires both proximity in time and in space; a slow
		 * tap elsewhere is just a tap. */
		bool double_tap = (st->last_tap_ms > 0) &&
		                  (now - st->last_tap_ms) <= st->cfg.double_tap_ms &&
		                  abs_dist(x, st->last_tap_x) < 60 &&
		                  abs_dist(y, st->last_tap_y) < 60;
		if (double_tap) {
			/* Consume the first tap so a triple tap is not a double tap
			 * followed by a tap. */
			st->last_tap_ms = 0;
			return ASTRIX_GESTURE_DOUBLE_TAP;
		}
		return ASTRIX_GESTURE_TAP;
	}
	return ASTRIX_GESTURE_NONE;
}

enum astrix_gesture astrix_gesture_handle(struct astrix_gesture_state *st,
                                          const struct astrix_input_event *ev) {
	if (!st || !ev) {
		return ASTRIX_GESTURE_NONE;
	}

	switch (ev->kind) {
	case ASTRIX_INPUT_TOUCH_DOWN:
	case ASTRIX_INPUT_POINTER_BUTTON: {
		/* A pointer button delivers press and release through one event
		 * kind, so the pressed bit selects which half it is. */
		bool press = (ev->kind == ASTRIX_INPUT_TOUCH_DOWN) ||
		             (ev->state & ASTRIX_KEY_PRESSED);
		if (press) {
			st->active_points++;
			if (st->active_points == 1) {
				st->down = true;
				st->moving = false;
				st->long_press_fired = false;
				st->long_press_cancelled = false;
				st->swiped_up = false;
				st->latched = ASTRIX_GESTURE_NONE;
				st->start_x = st->last_x = ev->x;
				st->start_y = st->last_y = ev->y;
				st->start_ms = ev_ms(ev);
				st->moved_ms = st->start_ms;
				st->hold_anchor_x = ev->x;
				st->hold_anchor_y = ev->y;
				st->pinch_start_distance = 0;
			} else if (st->active_points == 2) {
				/* Second finger: begin measuring a pinch span. */
				st->pinch_start_distance = 0;
			}
			return ASTRIX_GESTURE_NONE;
		}
		/* Fall through: a button release is handled as a touch up. */
	}
	/* fallthrough */
	case ASTRIX_INPUT_TOUCH_UP: {
		if (st->active_points > 0) {
			st->active_points--;
		}
		if (st->active_points > 0 || !st->down) {
			/* Other fingers still down, or a release with no press:
			 * not a completed gesture. */
			return ASTRIX_GESTURE_NONE;
		}
		st->down = false;

		int64_t now = ev_ms(ev);
		int64_t dt = now - st->start_ms;
		enum astrix_gesture g = classify_release(st, ev->x, ev->y, dt, now);

		if (st->latched != ASTRIX_GESTURE_NONE) {
			/*
			 * The gesture was recognised while the finger was still
			 * down and has *already been returned* - that is the whole
			 * point of latching it: a long press shows its menu the
			 * instant the hold completes, not when the finger lifts.
			 * Reporting it a second time here was a real bug, and a
			 * nasty one, because the second report is what the shell
			 * acts on: on a swipe the latched long press *replaced*
			 * the swipe, so a genuine upward swipe in the system band
			 * was silently swallowed and the app switcher never opened.
			 * Clear the latch and report nothing.
			 */
			st->latched = ASTRIX_GESTURE_NONE;
			return ASTRIX_GESTURE_NONE;
		}

		if (g == ASTRIX_GESTURE_LONG_PRESS) {
			st->long_press_fired = true;
		} else if (g == ASTRIX_GESTURE_TAP) {
			st->last_tap_ms = now;
			st->last_tap_x = ev->x;
			st->last_tap_y = ev->y;
		} else if (g == ASTRIX_GESTURE_DOUBLE_TAP) {
			st->last_tap_ms = 0;
		}
		return g;
	}

	case ASTRIX_INPUT_TOUCH_MOTION:
	case ASTRIX_INPUT_POINTER_MOTION: {
		if (!st->down) {
			return ASTRIX_GESTURE_NONE;
		}
		st->last_x = ev->x;
		st->last_y = ev->y;

		int64_t dx = (int64_t)ev->x - st->start_x;
		int64_t dy = (int64_t)ev->y - st->start_y;
		int64_t travel = (int64_t)sqrt((double)((double)(dx * dx + dy * dy)));
		if (!st->moving && travel > st->cfg.touch_slop_px) {
			st->moving = true;
		}
		/* Any real movement restarts the "still" clock. */
		if (abs_dist(ev->x, st->hold_anchor_x) > st->cfg.long_press_slop_px ||
		    abs_dist(ev->y, st->hold_anchor_y) > st->cfg.long_press_slop_px) {
			st->moved_ms = ev_ms(ev);
			st->hold_anchor_x = ev->x;
			st->hold_anchor_y = ev->y;
		}


		/*
		 * Long press must be recognised *while the finger is down*, not at
		 * release: a phone shows a context menu the instant the hold
		 * completes. It fires only while the finger is essentially still,
		 * so a slow drag never triggers it.
		 *
		 * "Swipe up and hold" is different: the finger has already moved up
		 * past the swipe threshold and is then held. That is tracked with
		 * the held_after_swipe flag rather than by distance from the start,
		 * because by definition it is no longer near the start point.
		 */
		int64_t held = ev_ms(ev) - st->start_ms;
		if (!st->moving && travel > st->cfg.swipe_threshold_px) {
			st->moving = true;
		}
		/*
		 * A long press that has already been reported is revoked once
		 * the finger travels, and never fires again for this contact.
		 *
		 * The recogniser fires the hold on a *timer*, not on movement,
		 * so a slow swipe can cross the hold threshold while the finger
		 * is still inside the slop - which is exactly what happens
		 * under emulation, where one injected waypoint can take tens of
		 * milliseconds. Without the revocation the hold wins and the
		 * swipe is lost. Without `long_press_cancelled` it would also
		 * fire again on the next motion, which is worse: a booted-VM
		 * log showed the same hold reported twice for one touch.
		 */
		if (st->long_press_fired && travel > st->cfg.long_press_slop_px) {
			st->long_press_fired = false;
			st->latched = ASTRIX_GESTURE_NONE;
			st->long_press_cancelled = true;
		}
		if (st->moving && dy < 0 &&
		    vertical_dominant(&st->cfg, abs64(dx), abs64(dy)) &&
		    travel >= st->cfg.swipe_threshold_px) {
			st->swiped_up = true;
		}
		/*
		 * "Hold" means the finger stopped, so the clock runs from the
		 * last real movement rather than from the start of the touch.
		 * Timed from the start, any swipe slow enough to outlive
		 * long_press_ms satisfies the hold - and on a loaded machine
		 * that is every swipe. That is not hypothetical: on a booted
		 * VM a swipe meant to dismiss a recents card came out as
		 * swipe-up-and-hold, so the switcher stayed open and nothing
		 * closed.
		 */
		int64_t still_for = ev_ms(ev) - st->moved_ms;
		if (st->swiped_up && still_for >= st->cfg.long_press_ms &&
		    !st->long_press_fired) {
			st->long_press_fired = true;
			st->swiped_up = false;
			/* Latch it: the upcoming release must not re-classify. */
			st->latched = ASTRIX_GESTURE_SWIPE_UP_HOLD;
			return ASTRIX_GESTURE_SWIPE_UP_HOLD;
		}
		if (!st->long_press_fired && !st->long_press_cancelled &&
		    travel <= st->cfg.long_press_slop_px && held >= st->cfg.long_press_ms) {
			st->long_press_fired = true;
			st->latched = ASTRIX_GESTURE_LONG_PRESS;
			return ASTRIX_GESTURE_LONG_PRESS;
		}
		return ASTRIX_GESTURE_NONE;
	}

	case ASTRIX_INPUT_SCROLL:
	case ASTRIX_INPUT_KEY:
		/* Not gestures: the shell consumes scrolling itself, and keys are
		 * handled by the focused app. */
		return ASTRIX_GESTURE_NONE;
	}
	return ASTRIX_GESTURE_NONE;
}
