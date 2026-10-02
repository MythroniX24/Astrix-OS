/*
 * Astrix OS - input handling.
 *
 * Touch is the primary input on a phone; a mouse/trackpad is a convenience
 * for the QEMU milestone and for external keyboards. Both funnel into the
 * same normalised coordinate space so the shell has one code path.
 *
 * The compositor does NOT interpret gestures. It resolves input to pixels and
 * forwards it to the focused client. Gesture recognition (swipe up/down,
 * long press, app-switcher hold) lives in the Astrix Shell, where it belongs:
 * a gesture that opens the notification shade is a shell decision, not a
 * compositor one.
 *
 * The one exception, and it is a routing decision rather than recognition:
 * the compositor owns a system-gesture band along the bottom edge. A press
 * that starts there and then travels upward far enough belongs to the shell,
 * even when an app holds the focus. Input follows focus, so without this an
 * app that has focus keeps the pointer and the user cannot get home - opening
 * an app would be a one-way door. The compositor decides *who receives the
 * gesture*; the shell still decides what it means. See maybe_claim_system_gesture.
 *
 * Forwarding, in wlroots 0.18 terms
 * ----------------------------------
 * wlroots deliberately does not forward input for you. It gives the compositor
 * a wlr_seat, and it expects the compositor to call the matching
 * wlr_seat_*_notify_* function with the surface that should receive the event.
 * That is why every handler below ends in a notify call: without them the
 * compositor starts, the clients connect, and no input is ever delivered to
 * anyone.
 *
 * The event structs used here (wlr_pointer_*_event, wlr_touch_*_event,
 * wlr_keyboard_event) are all public in 0.18. The wlr_cursor_* event structs
 * are not, so the cursor is used only to track position and to apply
 * output/region constraints - never as the source of the event data.
 */

#include "astrix_compositor.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>

/* --- pointer ------------------------------------------------------------- */

/*
 * Convert output-layout coordinates into surface-local coordinates.
 *
 * wl_pointer and wl_touch want *surface-local* coordinates, and the cursor
 * speaks layout coordinates. For Astrix they coincide, because the shell's
 * toplevel is full-screen on its output and that output sits at a known
 * position in the layout - so the translation is the output's own origin
 * subtracted. Doing it through the layout rather than assuming (0,0) keeps
 * it correct on a two-output device, and the honest limit is that a
 * *partly* off-screen client would still need its real surface position,
 * which wlroots 0.18 only exposes through the scene buffer. A phone has one
 * full-screen client, so that limit is noted rather than designed around.
 *
 * Note the position is NOT output->lx/ly: wlroots 0.18 moved the layout
 * position off wlr_output and onto wlr_output_layout_output, so it is read
 * through wlr_output_layout_get().
 */
static void layout_to_surface(struct astrix_server *server, double lx, double ly, double *sx,
                              double *sy) {
	struct wlr_output *output = wlr_output_layout_output_at(server->output_layout, lx, ly);
	if (output) {
		struct wlr_output_layout_output *layout_output =
		    wlr_output_layout_get(server->output_layout, output);
		if (layout_output) {
			*sx = lx - layout_output->x;
			*sy = ly - layout_output->y;
			return;
		}
	}
	/* No output under the point: fall back to identity rather than
	 * inventing a position. */
	*sx = lx;
	*sy = ly;
}

/*
 * The shell draws its own tap indicator, so the hardware cursor is never
 * composited. It still exists, because it is the only supported way to apply
 * output-layout and input-region constraints to a pointer position, and
 * clients expect a wl_pointer surface enter.
 *
 * The enter is the part that is easy to forget and fatal to omit. wlroots
 * does not deliver pointer events to a surface it has not been told is
 * focused: wlr_seat_pointer_notify_motion() and _notify_button() both act on
 * the focused surface, so without an enter they are silent no-ops. The whole
 * session looks healthy - the device is on the seat, the capabilities are
 * advertised, the client is mapped - and the phone is completely deaf to
 * touch.
 */
/*
 * The system band: the bottom ASTRIX_SYSTEM_GESTURE_BAND fraction of the
 * panel. A phone's home gesture starts at the very bottom edge, and so does
 * Astrix's.
 *
 * The band is a fraction rather than a pixel count because the shell lays out
 * for a 720x1600 panel while the QEMU panel is 1024x768; a fixed number of
 * pixels would be a different physical gesture on each. 6% of 768 is 46px,
 * which sits below the dock (the dock ends at y=711), so dock taps are never
 * at risk of being read as the start of a system gesture.
 */
#define ASTRIX_SYSTEM_GESTURE_BAND 0.94

/*
 * How far the pointer must travel upward before the gesture is claimed.
 * Expressed as a fraction of panel height for the same reason. 3% of 768 is
 * 23px, comfortably more than a tap's jitter and less than a deliberate
 * swipe.
 */
#define ASTRIX_GESTURE_SLOP_FRACTION 0.03

/* The output containing a layout point, or NULL if the point is off-panel. */
static struct astrix_output *layout_output_at(struct astrix_server *server, double lx,
                                              double ly) {
	struct astrix_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!output->initialized) {
			continue;
		}
		if (lx >= output->x && lx < output->x + output->width && ly >= output->y &&
		    ly < output->y + output->height) {
			return output;
		}
	}
	return NULL;
}

static void forward_pointer_motion(struct astrix_server *server, uint32_t time_msec,
                                   double lx, double ly);

/*
 * Release every button still held by the surface that currently has pointer
 * focus, so a client that was given a press is always given its release.
 *
 * This is a protocol invariant, not a nicety. The input target follows app
 * focus, so the target changes *in the middle of a gesture* every time a
 * dock tap launches an app: the shell gets the press, the new toplevel is
 * focused, and the release then goes to the toplevel instead. The shell is
 * left with a contact it can never end, and its gesture recogniser stays
 * anchored to that old press forever. The visible symptom is baffling
 * because nothing is logged as wrong at the time - the next gesture is
 * simply classified against a stale anchor, so a straight upward home swipe
 * reads as "swipe-right from 400,598 to 512,576", where 400,598 is where
 * the finger was several gestures ago.
 *
 * Releasing here (while the old surface still has focus) rather than after
 * the switch is what makes it work: wlroots delivers a release to the
 * focused surface, so the order is release, then leave, then enter.
 */
static void release_held_buttons(struct astrix_server *server, uint32_t time_msec) {
	struct wlr_seat_pointer_state *state = &server->seat->pointer_state;
	if (state->button_count == 0 || !state->focused_surface) {
		return;
	}
	size_t count = state->button_count;
	uint32_t buttons[WLR_POINTER_BUTTONS_CAP];
	for (size_t i = 0; i < count && i < WLR_POINTER_BUTTONS_CAP; i++) {
		buttons[i] = state->buttons[i];
	}
	astrix_log(WLR_INFO, "releasing %zu held button(s) before retargeting the pointer", count);
	for (size_t i = 0; i < count && i < WLR_POINTER_BUTTONS_CAP; i++) {
		wlr_seat_pointer_notify_button(server->seat, time_msec, buttons[i],
		                               WL_POINTER_BUTTON_STATE_RELEASED);
	}
	wlr_seat_pointer_notify_frame(server->seat);
}

/* Note where this press landed, and whether it could become a system gesture. */
static void pointer_arm_system_gesture(struct astrix_pointer *ap) {
	struct astrix_server *server = ap->server;
	ap->down_x = server->cursor->x;
	ap->down_y = server->cursor->y;
	ap->gesture_claimed = false;
	ap->gesture_armed = false;

	struct astrix_output *output = layout_output_at(server, ap->down_x, ap->down_y);
	if (!output) {
		return;
	}
	double band_top = output->y + output->height * ASTRIX_SYSTEM_GESTURE_BAND;
	ap->gesture_armed = ap->down_y >= band_top;
}

/*
 * Hand the gesture to the shell once it is clearly an upward swipe.
 *
 * Called from the motion handlers before the motion is forwarded, so the app
 * receives the press and the first few pixels of travel and then loses the
 * gesture - which is how Android's touch slop behaves, and why the slop
 * exists at all: a tap in the band must still be a tap for the app.
 */
static void maybe_claim_system_gesture(struct astrix_pointer *ap, uint32_t time_msec) {
	struct astrix_server *server = ap->server;
	if (!ap->gesture_armed || ap->gesture_claimed) {
		return;
	}
	double travelled = ap->down_y - server->cursor->y;
	if (travelled <= 0.0) {
		/* Downward or sideways: not a system gesture. */
		return;
	}
	struct astrix_output *output = layout_output_at(server, ap->down_x, ap->down_y);
	if (!output || travelled < output->height * ASTRIX_GESTURE_SLOP_FRACTION) {
		return;
	}
	ap->gesture_claimed = true;

	/* Give the screen back to the shell, which makes it the input target. */
	astrix_shell_release_focus(server);
	struct wlr_surface *shell = astrix_shell_input_target(server);
	if (!shell) {
		astrix_log(WLR_ERROR,
		           "system gesture claimed but the shell has no surface; dropping it");
		return;
	}

	astrix_log(WLR_INFO, "system gesture claimed after %.0fpx; input returns to the shell",
	           travelled);

	/*
	 * The app that had focus must be told it lost the pointer, and any
	 * button it is still holding must be released to it first (see
	 * release_held_buttons).
	 *
	 * Then the shell has to be told where the press *started*, not where
	 * the cursor happens to be now: wl_pointer button events carry no
	 * coordinates at all, so a client reads the position of a press from
	 * the last motion it received.
	 */
	/*
	 * If the shell already holds the press - a gesture that starts on a
	 * screen the shell owns, where it is the input target all along -
	 * there is nothing to hand over. Releasing and re-pressing would
	 * deliver a spurious release/press pair, which the shell correctly
	 * reads as a tap, and the user gets a phantom tap on top of their
	 * swipe. Re-anchor the position and leave the contact alone.
	 */
	if (wlr_seat_pointer_surface_has_focus(server->seat, shell)) {
		double ox, oy;
		layout_to_surface(server, ap->down_x, ap->down_y, &ox, &oy);
		astrix_log(WLR_INFO, "the shell already holds the press; re-anchoring at %.0f,%.0f",
		           ap->down_x, ap->down_y);
		wlr_seat_pointer_notify_motion(server->seat, time_msec, ox, oy);
		wlr_seat_pointer_notify_frame(server->seat);
		return;
	}

	release_held_buttons(server, time_msec);
	wlr_seat_pointer_notify_clear_focus(server->seat);
	/*
	 * Enter where the cursor actually is, then *move* to the press
	 * origin. The order matters and is not obvious: wlroots suppresses a
	 * motion that repeats the position the enter just established, so
	 * entering at the origin and then moving to it drops the position
	 * entirely and the shell anchors the gesture to wherever it last was.
	 * That was the whole failure - the VM log read
	 * "swipe-right from 400,598 to 512,574" for a straight upward swipe,
	 * 400,598 being a dock icon touched several gestures earlier.
	 */
	double sx, sy;
	layout_to_surface(server, server->cursor->x, server->cursor->y, &sx, &sy);
	wlr_seat_pointer_notify_enter(server->seat, shell, sx, sy);
	layout_to_surface(server, ap->down_x, ap->down_y, &sx, &sy);
	wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	wlr_seat_pointer_notify_frame(server->seat);
	/*
	 * Synthesise the press the shell never received. The gesture
	 * recogniser starts on button-down, so handing the shell motions and a
	 * release with no press in between classifies as nothing at all: the
	 * home swipe would silently do nothing while every log line said the
	 * gesture had been delivered.
	 */
	astrix_log(WLR_INFO, "synthesising the shell's press at %.0f,%.0f", ap->down_x, ap->down_y);
	wlr_seat_pointer_notify_button(server->seat, time_msec, BTN_LEFT,
	                               WL_POINTER_BUTTON_STATE_PRESSED);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void forward_pointer_motion(struct astrix_server *server, uint32_t time_msec, double lx,
                                   double ly) {
	struct wlr_surface *target = astrix_shell_input_target(server);
	if (!target) {
		/*
		 * Nothing to aim at. Focus is cleared rather than left pointing
		 * at a surface that has gone away: a stale focus would deliver
		 * the next tap to a dead client.
		 */
		wlr_seat_pointer_notify_clear_focus(server->seat);
		return;
	}
	double sx, sy;
	layout_to_surface(server, lx, ly, &sx, &sy);

	if (!wlr_seat_pointer_surface_has_focus(server->seat, target)) {
		/*
		 * The target moved under a held button (an app just took
		 * focus). Close out the contact on the old surface first;
		 * see release_held_buttons.
		 */
		release_held_buttons(server, time_msec);
		astrix_log(WLR_INFO, "pointer entering surface at %.0f,%.0f", sx, sy);
		wlr_seat_pointer_notify_enter(server->seat, target, sx, sy);
	}
	wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
	/*
	 * Motion events must be followed by a frame event so a client that
	 * batches by frame sees a coherent position. Sending it here rather
	 * than on a timer keeps the shell's redraw aligned with the input.
	 */
	wlr_seat_pointer_notify_frame(server->seat);
}

static void pointer_motion_handler(struct wl_listener *listener, void *data) {
	struct astrix_pointer *ap = wl_container_of(listener, ap, motion);
	struct wlr_pointer_motion_event *event = data;
	/*
	 * wlr_cursor_move applies the device's output/region constraints and
	 * updates cursor->x/y to the resulting layout coordinates. Reading the
	 * position back from the cursor rather than accumulating deltas here
	 * is what keeps the cursor inside the usable area at a screen edge.
	 */
	wlr_cursor_move(ap->server->cursor, &ap->wlr_pointer->base, event->delta_x, event->delta_y);
	maybe_claim_system_gesture(ap, event->time_msec);
	forward_pointer_motion(ap->server, event->time_msec, ap->server->cursor->x,
	                       ap->server->cursor->y);
	/*
	 * DEBUG, not INFO. This is the highest-volume line in the compositor -
	 * roughly one per frame of a drag - and on a device the only place to
	 * read it is a serial console, where 60 lines a second buries the
	 * messages that matter. It is what proves where the cursor actually
	 * landed when a tap comes out in the wrong place, which is why it
	 * stays available: raise ASTRIX_LOG_LEVEL to 3 when debugging input.
	 *
	 * The usable bounds are deliberately not logged: wlroots 0.18 keeps
	 * them private inside wlr_cursor_state, and handle_new_output already
	 * logs the output's position and size, which is the same number.
	 */
	astrix_log(WLR_DEBUG, "pointer motion to %.0f,%.0f", ap->server->cursor->x,
	           ap->server->cursor->y);
	astrix_server_invalidate(ap->server);
}

static void pointer_motion_absolute_handler(struct wl_listener *listener, void *data) {
	struct astrix_pointer *ap = wl_container_of(listener, ap, motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	/* event->x / event->y are normalised 0..1 across the pointing device. */
	wlr_cursor_warp_absolute(ap->server->cursor, &ap->wlr_pointer->base, event->x, event->y);
	maybe_claim_system_gesture(ap, event->time_msec);
	forward_pointer_motion(ap->server, event->time_msec, ap->server->cursor->x,
	                       ap->server->cursor->y);
	astrix_log(WLR_DEBUG, "absolute motion %.3f,%.3f -> %.0f,%.0f", event->x, event->y,
	           ap->server->cursor->x, ap->server->cursor->y);
	astrix_server_invalidate(ap->server);
}

static void pointer_button_handler(struct wl_listener *listener, void *data) {
	struct astrix_pointer *ap = wl_container_of(listener, ap, button);
	struct wlr_pointer_button_event *event = data;
	/*
	 * Astrix is touch-first: the shell treats a button press the same way
	 * it treats a tap, so there is one downstream path, not a mouse path
	 * and a touch path.
	 */
	/*
	 * Logged at INFO for the same reason touch down is: this is the line
	 * that distinguishes "the hardware event never arrived" from "the
	 * event arrived and the client did nothing with it". Those two
	 * failures look identical from the shell's side.
	 */
	/*
	 * Compared against the wl_pointer enum, not wlr_button_state. The two
	 * happen to share the same 0/1 encoding today, but only one of them is
	 * the type of this expression, and gcc flags the mismatch (-Wenum-compare).
	 */
	astrix_log(WLR_INFO, "pointer button %u %s", event->button,
	           event->state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released");

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		/* Remember where this press landed, in case it becomes a system
		 * gesture. It goes to the app first either way: the press alone
		 * is not enough travel to claim one. */
		pointer_arm_system_gesture(ap);
	} else {
		/* The gesture is over; the next press starts a new candidate. */
		ap->gesture_armed = false;
		ap->gesture_claimed = false;
	}wlr_seat_pointer_notify_button(ap->server->seat, event->time_msec, event->button,
	                           event->state);
	wlr_seat_pointer_notify_frame(ap->server->seat);
	astrix_server_invalidate(ap->server);
}

static void pointer_axis_handler(struct wl_listener *listener, void *data) {
	struct astrix_pointer *ap = wl_container_of(listener, ap, axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(ap->server->seat, event->time_msec, event->orientation,
	                             event->delta, event->delta_discrete, event->source,
	                             event->relative_direction);
	wlr_seat_pointer_notify_frame(ap->server->seat);
	astrix_server_invalidate(ap->server);
}

static void pointer_destroy_handler(struct wl_listener *listener, void *data) {
	struct astrix_pointer *ap = wl_container_of(listener, ap, destroy);
	astrix_log(WLR_INFO, "pointer device removed");
	wl_list_remove(&ap->link);
	wl_list_remove(&ap->motion.link);
	wl_list_remove(&ap->motion_absolute.link);
	wl_list_remove(&ap->button.link);
	wl_list_remove(&ap->axis.link);
	wl_list_remove(&ap->destroy.link);
	free(ap);
}

/* --- touch --------------------------------------------------------------- */

static struct astrix_touch_point *find_point(struct astrix_server *server, int32_t id) {
	struct astrix_touch_point *tp;
	wl_list_for_each(tp, &server->touchpoints, link) {
		if (tp->id == id) {
			return tp;
		}
	}
	return NULL;
}

static void touch_down_handler(struct wl_listener *listener, void *data) {
	struct astrix_touch *at = wl_container_of(listener, at, down);
	struct wlr_touch_down_event *event = data;

	if (find_point(at->server, event->touch_id)) {
		/* Already tracking this id: treat a duplicate down as a new
		 * sample rather than leaking a second point. */
		return;
	}
	struct astrix_touch_point *tp = calloc(1, sizeof(*tp));
	if (!tp) {
		return;
	}
	tp->id = event->touch_id;
	tp->x = event->x;
	tp->y = event->y;
	tp->time_msec = event->time_msec;
	tp->active = true;
	astrix_touch_to_pixels(at->server, event->x, event->y, &tp->px, &tp->py);
	wl_list_insert(&at->server->touchpoints, &tp->link);

	/*
	 * Forward to the client. A touch point is only accepted if the target
	 * surface accepts touch; if there is no target (no app and no shell
	 * yet), the point is tracked but not delivered, so a finger that lands
	 * during startup is not replayed to whatever maps next.
	 */
	struct wlr_surface *target = astrix_shell_input_target(at->server);
	if (target) {
		/* wl_touch wants surface-local coordinates too. */
		double sx, sy;
		layout_to_surface(at->server, event->x, event->y, &sx, &sy);
		wlr_seat_touch_notify_down(at->server->seat, target, event->time_msec,
		                           event->touch_id, sx, sy);
		wlr_seat_touch_notify_frame(at->server->seat);
	}

	/*
	 * INFO, not DEBUG: a finger landing on the screen is the one event
	 * whose absence is otherwise completely silent. Without it, a
	 * multi-touch panel that is attached but not delivering looks exactly
	 * like a panel that is working and an app that ignores touches.
	 */
	astrix_log(WLR_INFO, "touch down id=%d at %d,%d", tp->id, tp->px, tp->py);
	astrix_server_invalidate(at->server);
}

static void touch_motion_handler(struct wl_listener *listener, void *data) {
	struct astrix_touch *at = wl_container_of(listener, at, motion);
	struct wlr_touch_motion_event *event = data;

	struct astrix_touch_point *tp = find_point(at->server, event->touch_id);
	if (!tp) {
		return;
	}
	tp->dx = event->x - tp->x;
	tp->dy = event->y - tp->y;
	tp->x = event->x;
	tp->y = event->y;
	tp->time_msec = event->time_msec;
	astrix_touch_to_pixels(at->server, event->x, event->y, &tp->px, &tp->py);
	/*
	 * Motion must be forwarded even when the point is not owned by the
	 * focused client, so that a client's own gesture state can be
	 * cancelled or completed correctly.
	 */
	/* wl_touch motion is surface-local as well. */
	double sx, sy;
	layout_to_surface(at->server, event->x, event->y, &sx, &sy);
	wlr_seat_touch_notify_motion(at->server->seat, event->time_msec, event->touch_id, sx, sy);
	wlr_seat_touch_notify_frame(at->server->seat);
	astrix_server_invalidate(at->server);
}

static void touch_up_handler(struct wl_listener *listener, void *data) {
	struct astrix_touch *at = wl_container_of(listener, at, up);
	struct wlr_touch_up_event *event = data;

	struct astrix_touch_point *tp = find_point(at->server, event->touch_id);
	if (tp) {
		wl_list_remove(&tp->link);
		free(tp);
	}
	/*
	 * notify_up is sent even for a point the client never received a down
	 * for: the return value tells us whether the client actually had it,
	 * and leaving a down outstanding would wedge the client.
	 */
	if (wlr_seat_touch_notify_up(at->server->seat, event->time_msec, event->touch_id)) {
		wlr_seat_touch_notify_frame(at->server->seat);
	}
	astrix_server_invalidate(at->server);
}

static void touch_cancel_handler(struct wl_listener *listener, void *data) {
	struct astrix_touch *at = wl_container_of(listener, at, cancel);

	/* A cancel invalidates every in-flight touch (palm rejection, gesture
	 * ownership change, surface teardown). */
	struct astrix_touch_point *tp, *tmp;
	wl_list_for_each_safe(tp, tmp, &at->server->touchpoints, link) {
		wl_list_remove(&tp->link);
		free(tp);
	}
	/*
	 * Tell the client its whole touch sequence is void. Without this a
	 * gesture recogniser that is mid-swipe would keep waiting for a
	 * release that is never coming. NULL means "cancel for every client
	 * that had a point down", which is the honest interpretation of a
	 * device-level cancel.
	 */
	wlr_seat_touch_notify_cancel(at->server->seat, NULL);
	wlr_seat_touch_notify_frame(at->server->seat);
	astrix_server_invalidate(at->server);
}

static void touch_destroy_handler(struct wl_listener *listener, void *data) {
	struct astrix_touch *at = wl_container_of(listener, at, destroy);
	astrix_log(WLR_INFO, "touch device removed");
	wl_list_remove(&at->link);
	wl_list_remove(&at->down.link);
	wl_list_remove(&at->up.link);
	wl_list_remove(&at->motion.link);
	wl_list_remove(&at->cancel.link);
	wl_list_remove(&at->destroy.link);
	free(at);
}

/* --- keyboard ------------------------------------------------------------ */

static void keyboard_key_handler(struct wl_listener *listener, void *data) {
	struct astrix_keyboard *ak = wl_container_of(listener, ak, key);
	/*
	 * The event struct is wlr_keyboard_key_event in wlroots 0.18 - the
	 * older wlr_keyboard_event name is gone.
	 */
	struct wlr_keyboard_key_event *event = data;
	/*
	 * A phone needs a hardware keyboard (a Bluetooth keyboard, or the
	 * QEMU milestone), and Astrix Terminal has to be usable with one.
	 * wlroots has already resolved the keycode; this only has to hand it
	 * to the seat, which applies any active grab and delivers it to the
	 * focused surface.
	 */
	wlr_seat_keyboard_notify_key(ak->server->seat, event->time_msec, event->keycode,
	                             event->state);
	astrix_server_invalidate(ak->server);
}

static void keyboard_modifiers_handler(struct wl_listener *listener, void *data) {
	struct astrix_keyboard *ak = wl_container_of(listener, ak, modifiers);
	struct wlr_keyboard_modifiers *mods = data;
	/*
	 * Modifiers are separate from key events in the protocol. A client
	 * that never receives this cannot implement Shift/Tab correctly, so it
	 * is forwarded on every change rather than only on key events.
	 */
	wlr_seat_keyboard_notify_modifiers(ak->server->seat, mods);
	astrix_server_invalidate(ak->server);
}

static void keyboard_destroy_handler(struct wl_listener *listener, void *data) {
	struct astrix_keyboard *ak = wl_container_of(listener, ak, destroy);
	wl_list_remove(&ak->link);
	wl_list_remove(&ak->modifiers.link);
	wl_list_remove(&ak->key.link);
	wl_list_remove(&ak->destroy.link);
	/*
	 * Detach the device from the seat before dropping our reference.
	 * Leaving it attached would keep a dangling wlr_keyboard on
	 * seat->keyboards, which is walked on every key event.
	 */
	if (wlr_seat_get_keyboard(ak->server->seat) == ak->wlr_keyboard) {
		wlr_seat_keyboard_notify_clear_focus(ak->server->seat);
		wlr_seat_set_keyboard(ak->server->seat, NULL);
	}
	/*
	 * If the last keyboard disappears the seat must stop advertising the
	 * capability, or clients will wait forever for key events.
	 */
	if (wl_list_empty(&ak->server->keyboards)) {
		wlr_seat_set_capabilities(ak->server->seat, 0);
	}
	free(ak);
}

/* --- device registry ----------------------------------------------------- */

static void update_seat_capabilities(struct astrix_server *server) {
	uint32_t caps = 0;
	if (!wl_list_empty(&server->pointers)) {
		caps |= WL_SEAT_CAPABILITY_POINTER;
	}
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	if (!wl_list_empty(&server->touch)) {
		caps |= WL_SEAT_CAPABILITY_TOUCH;
	}
	wlr_seat_set_capabilities(server->seat, caps);
	/*
	 * A newly attached keyboard has to be told who has focus, and the
	 * focused client has to be told what keys are currently down.
	 */
	astrix_shell_apply_keyboard_focus(server);
	astrix_log(WLR_INFO, "seat capabilities: %s%s%s", (caps & WL_SEAT_CAPABILITY_POINTER) ? "pointer " : "",
	           (caps & WL_SEAT_CAPABILITY_KEYBOARD) ? "keyboard " : "",
	           (caps & WL_SEAT_CAPABILITY_TOUCH) ? "touch" : "");
}

static void new_input_handler(struct wl_listener *listener, void *data) {
	struct astrix_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD: {
		struct astrix_keyboard *ak = calloc(1, sizeof(*ak));
		if (!ak) {
			return;
		}
		ak->server = server;
		ak->wlr_keyboard = wlr_keyboard_from_input_device(device);
		wl_list_insert(&server->keyboards, &ak->link);
		ak->modifiers.notify = keyboard_modifiers_handler;
		wl_signal_add(&ak->wlr_keyboard->events.modifiers, &ak->modifiers);
		ak->key.notify = keyboard_key_handler;
		wl_signal_add(&ak->wlr_keyboard->events.key, &ak->key);
		ak->destroy.notify = keyboard_destroy_handler;
		wl_signal_add(&device->events.destroy, &ak->destroy);
		/*
		 * The seat only forwards key events for the keyboard it has
		 * been given. Without this the device is tracked but the client
		 * never sees a single key.
		 */
		wlr_seat_set_keyboard(server->seat, ak->wlr_keyboard);
		astrix_log(WLR_INFO, "keyboard added: %s", device->name ? device->name : "?");
		break;
	}
	case WLR_INPUT_DEVICE_POINTER: {
		struct astrix_pointer *ap = calloc(1, sizeof(*ap));
		if (!ap) {
			return;
		}
		ap->server = server;
		ap->wlr_pointer = wlr_pointer_from_input_device(device);
		wl_list_insert(&server->pointers, &ap->link);
		ap->motion.notify = pointer_motion_handler;
		wl_signal_add(&ap->wlr_pointer->events.motion, &ap->motion);
		ap->motion_absolute.notify = pointer_motion_absolute_handler;
		wl_signal_add(&ap->wlr_pointer->events.motion_absolute, &ap->motion_absolute);
		ap->button.notify = pointer_button_handler;
		wl_signal_add(&ap->wlr_pointer->events.button, &ap->button);
		ap->axis.notify = pointer_axis_handler;
		wl_signal_add(&ap->wlr_pointer->events.axis, &ap->axis);
		ap->destroy.notify = pointer_destroy_handler;
		wl_signal_add(&device->events.destroy, &ap->destroy);
		astrix_log(WLR_INFO, "pointer added: %s", device->name ? device->name : "?");
		wlr_cursor_attach_input_device(server->cursor, device);
		break;
	}
	case WLR_INPUT_DEVICE_TOUCH: {
		struct astrix_touch *at = calloc(1, sizeof(*at));
		if (!at) {
			return;
		}
		at->server = server;
		at->wlr_touch = wlr_touch_from_input_device(device);
		wl_list_insert(&server->touch, &at->link);
		at->down.notify = touch_down_handler;
		wl_signal_add(&at->wlr_touch->events.down, &at->down);
		at->up.notify = touch_up_handler;
		wl_signal_add(&at->wlr_touch->events.up, &at->up);
		at->motion.notify = touch_motion_handler;
		wl_signal_add(&at->wlr_touch->events.motion, &at->motion);
		at->cancel.notify = touch_cancel_handler;
		wl_signal_add(&at->wlr_touch->events.cancel, &at->cancel);
		at->destroy.notify = touch_destroy_handler;
		wl_signal_add(&device->events.destroy, &at->destroy);
		astrix_log(WLR_INFO, "touch device added: %s", device->name ? device->name : "?");
		break;
	}
	default:
		astrix_log(WLR_INFO, "ignoring unsupported input device type %d", device->type);
		return;
	}

	update_seat_capabilities(server);
	astrix_server_invalidate(server);
}

/* --- lifecycle ----------------------------------------------------------- */

void astrix_input_init(struct astrix_server *server) {
	server->new_input.notify = new_input_handler;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	/*
	 * The cursor needs the output layout so that its position is
	 * constrained to a real screen. wlroots 0.18's wlr_backend has no
	 * device list of its own - every device arrives through new_input,
	 * including the ones that already existed when the backend started -
	 * so devices are attached to the cursor as they are added.
	 */
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
}

void astrix_input_finish(struct astrix_server *server) {
	struct astrix_touch_point *tp, *tmp;
	wl_list_for_each_safe(tp, tmp, &server->touchpoints, link) {
		wl_list_remove(&tp->link);
		free(tp);
	}
}
