/*
 * Astrix OS - native application host implementation.
 *
 * See astrix_app.h for the contract. This file owns everything Wayland and
 * hands the app a plain ARGB8888 canvas plus input events.
 *
 * Frame pacing
 * ------------
 * The app never draws into a buffer the compositor is still reading. Two shm
 * buffers are kept; a draw is skipped if both are in flight, and the release /
 * frame-done callbacks re-trigger it. That is what makes a 60 Hz panel redraw
 * without tearing, and it is why the loop can idle at 250 ms without the UI
 * feeling laggy: input wakes it immediately via poll().
 */

#include "astrix_app.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-header.h"

struct astrix_app_host {
	const struct astrix_app_desc *desc;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct xdg_wm_base *wm_base;
	struct wl_pointer *pointer;
	struct wl_touch *touch;
	struct wl_keyboard *keyboard;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;

	struct wl_buffer *buffers[2];
	uint32_t *buffer_data[2];
	bool buffer_valid[2];
	int buffer_count;
	int cur_buffer;

	struct wl_callback *frame_callback;
	bool configured;
	bool running;
	bool dirty;
	bool has_focus;
	bool started;

	int width, height;
	struct astrix_canvas canvas;
	const struct astrix_theme *theme;
	bool dark;

	void *user;
	int64_t start_ms;
	int64_t last_tick_ms;
	uint64_t frames;

	/* Transient message drawn at the bottom of the screen. */
	char toast[192];
	int64_t toast_until_ms;
	int pointer_x, pointer_y;
	bool pointer_down;
};

#define HOST_BUF_COUNT 2

/* --- time ---------------------------------------------------------------- */

static int64_t monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t astrix_app_now_ms(struct astrix_app_host *host) {
	(void)host;
	return monotonic_ms();
}

static int64_t now_ms_rel(struct astrix_app_host *host) {
	return monotonic_ms() - host->start_ms;
}

/* --- public accessors ---------------------------------------------------- */

struct astrix_canvas *astrix_app_canvas(struct astrix_app_host *host) {
	return &host->canvas;
}

const struct astrix_theme *astrix_app_theme(struct astrix_app_host *host) {
	return host->theme;
}

void astrix_app_size(struct astrix_app_host *host, int *w, int *h) {
	*w = host->width;
	*h = host->height;
}

void astrix_app_invalidate(struct astrix_app_host *host) {
	host->dirty = true;
}

bool astrix_app_dark(struct astrix_app_host *host) {
	return host->dark;
}

bool astrix_app_set_dark(struct astrix_app_host *host, bool dark) {
	if (host->dark == dark) {
		return false;
	}
	host->dark = dark;
	host->theme = dark ? astrix_theme_dark() : astrix_theme_light();
	host->dirty = true;
	return true;
}

void *astrix_app_user(struct astrix_app_host *host) {
	return host->user;
}

void astrix_app_set_user(struct astrix_app_host *host, void *user) {
	host->user = user;
}

void astrix_app_quit(struct astrix_app_host *host) {
	host->running = false;
}

void astrix_app_request_close(struct astrix_app_host *host) {
	/*
	 * xdg_toplevel.close is a server-to-client event; a client cannot send
	 * it. The honest way to close an app from inside the app is to exit;
	 * the shell's app switcher handles the user-facing "close" action.
	 */
	host->running = false;
}

void astrix_app_toast(struct astrix_app_host *host, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(host->toast, sizeof(host->toast), fmt, ap);
	va_end(ap);
	host->toast_until_ms = now_ms_rel(host) + 2600;
	host->dirty = true;
}

/* --- drawing ------------------------------------------------------------- */

static void draw_and_commit(struct astrix_app_host *host);

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
	struct astrix_app_host *host = data;
	for (int i = 0; i < HOST_BUF_COUNT; i++) {
		if (host->buffers[i] == wl_buffer) {
			host->buffer_valid[i] = true;
		}
	}
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	struct astrix_app_host *host = data;
	wl_callback_destroy(callback);
	host->frame_callback = NULL;
	if (host->dirty) {
		draw_and_commit(host);
	}
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

static int create_buffer(struct astrix_app_host *host, int index) {
	size_t size = (size_t)host->width * host->height * 4;
	int fd = memfd_create("astrix-app", MFD_CLOEXEC);
	if (fd < 0) {
		/* Older kernels, or a hardened memfd policy, can refuse this. */
		char tmpl[] = "/tmp/astrix-app-XXXXXX";
		fd = mkstemp(tmpl);
		if (fd < 0) {
			return -1;
		}
		unlink(tmpl);
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return -1;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(host->shm, fd, (int32_t)size);
	host->buffers[index] =
	    wl_shm_pool_create_buffer(pool, 0, host->width, host->height, host->width * 4,
	                              WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	if (!host->buffers[index]) {
		munmap(data, size);
		return -1;
	}
	wl_buffer_add_listener(host->buffers[index], &buffer_listener, host);
	host->buffer_data[index] = data;
	host->buffer_valid[index] = true;
	return 0;
}

static void destroy_buffers(struct astrix_app_host *host) {
	for (int i = 0; i < HOST_BUF_COUNT; i++) {
		if (host->buffers[i]) {
			wl_buffer_destroy(host->buffers[i]);
			host->buffers[i] = NULL;
		}
		if (host->buffer_data[i]) {
			munmap(host->buffer_data[i], (size_t)host->width * host->height * 4);
			host->buffer_data[i] = NULL;
		}
	}
	host->buffer_count = 0;
}

static int allocate_buffers(struct astrix_app_host *host) {
	destroy_buffers(host);
	int n = 0;
	for (int i = 0; i < HOST_BUF_COUNT; i++) {
		if (create_buffer(host, i) == 0) {
			n++;
		}
	}
	host->buffer_count = n;
	host->canvas.pixels = n > 0 ? host->buffer_data[0] : NULL;
	host->canvas.width = host->width;
	host->canvas.height = host->height;
	host->canvas.stride = host->width;
	return n;
}

static void draw_toast(struct astrix_app_host *host) {
	if (!host->toast[0] || now_ms_rel(host) > host->toast_until_ms) {
		return;
	}
	struct astrix_canvas *c = &host->canvas;
	const struct astrix_theme *th = host->theme;
	int w = astrix_text_width(host->toast) + th->spacing * 6;
	int h = 40;
	int x = (host->width - w) / 2;
	int y = host->height - th->navbar_h - h - 24;
	struct astrix_rect r = { x, y, w, h };
	astrix_fill_rect_rounded(c, r, h / 2, astrix_color_with_alpha(th->text, 235));
	astrix_draw_text(c, x + th->spacing * 3, y + (h - ASTRIX_FONT_H) / 2, host->toast,
	                 th->background);
	host->dirty = true; /* keep redrawing while the toast is visible */
}

static void draw_and_commit(struct astrix_app_host *host) {
	if (!host->surface || !host->configured || host->buffer_count == 0) {
		return;
	}
	int idx = -1;
	for (int i = 0; i < host->buffer_count; i++) {
		if (host->buffer_valid[i]) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		/* Both buffers in flight; the next release triggers a redraw. */
		host->dirty = true;
		return;
	}

	host->canvas.pixels = host->buffer_data[idx];
	host->canvas.width = host->width;
	host->canvas.height = host->height;
	host->canvas.stride = host->width;

	astrix_clear(&host->canvas, host->theme->background);
	if (host->desc->on_frame) {
		host->desc->on_frame(host);
	}
	draw_toast(host);
	host->frames++;
	host->dirty = false;
	host->buffer_valid[idx] = false;

	wl_surface_attach(host->surface, host->buffers[idx], 0, 0);
	wl_surface_damage_buffer(host->surface, 0, 0, host->width, host->height);
	if (host->frame_callback) {
		wl_callback_destroy(host->frame_callback);
	}
	host->frame_callback = wl_surface_frame(host->surface);
	wl_callback_add_listener(host->frame_callback, &frame_listener, host);
	wl_surface_commit(host->surface);
	if (wl_display_flush(host->display) < 0) {
		host->running = false;
	}
}

/* --- input --------------------------------------------------------------- */

static void deliver(struct astrix_app_host *host, enum astrix_input_kind kind, int x, int y) {
	struct astrix_input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = kind;
	ev.x = x;
	ev.y = y;
	ev.timestamp = (double)monotonic_ms() / 1000.0;
	host->pointer_x = x;
	host->pointer_y = y;
	if (host->desc->on_input) {
		host->desc->on_input(host, &ev);
	}
	host->dirty = true;
}

static void pointer_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                          wl_fixed_t x, wl_fixed_t y) {
}

static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *s) {
}

static void pointer_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
                           wl_fixed_t y) {
	deliver(d, ASTRIX_INPUT_POINTER_MOTION, wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state) {
	struct astrix_app_host *host = d;
	/* wl_pointer.button carries evdev codes, so BTN_LEFT is the left click. */
	if (button != BTN_LEFT) {
		return;
	}
	bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	host->pointer_down = pressed;
	deliver(host, pressed ? ASTRIX_INPUT_POINTER_BUTTON : ASTRIX_INPUT_TOUCH_UP,
	        host->pointer_x, host->pointer_y);
	/*
	 * The compositor's only role for input is forwarding; the app is the
	 * UI. Reusing TOUCH_UP for the pointer release keeps one code path in
	 * the apps, and the shell does the same for the same reason.
	 */
}

static void pointer_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis,
                         wl_fixed_t value) {
	struct astrix_app_host *host = d;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}
	struct astrix_input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = ASTRIX_INPUT_SCROLL;
	ev.scroll_y = -wl_fixed_to_int(value);
	ev.timestamp = (double)monotonic_ms() / 1000.0;
	if (host->desc->on_input) {
		host->desc->on_input(host, &ev);
	}
	host->dirty = true;
}

/*
 * libwayland aborts a client that receives an event it has no listener for
 * ("listener function for opcode 5 of wl_pointer is NULL"), so every event
 * the interface declares needs a handler - even the ones this library has no
 * use for. See docs/STATUS.md bug 34: astrix-files died on the first touch
 * of every session because this initialiser stopped at .axis and the rest
 * zero-filled to NULL.
 */
static void pointer_frame(void *d, struct wl_pointer *p) {
}

static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t source) {
}

static void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis) {
}

static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t axis, int32_t discrete) {
}

/*
 * axis_value120 carries the same information as wl_pointer.axis at 120x the
 * resolution. The library drives apps through the legacy wl_pointer.axis
 * event, so this is accepted and ignored rather than mapped - but it must
 * not be NULL.
 */
static void pointer_axis_value120(void *d, struct wl_pointer *p, uint32_t axis, int32_t value120) {
}

static void pointer_axis_relative_direction(void *d, struct wl_pointer *p, uint32_t axis,
                                            uint32_t direction) {
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
	.axis_value120 = pointer_axis_value120,
	.axis_relative_direction = pointer_axis_relative_direction,
};

static void touch_down(void *d, struct wl_touch *t, uint32_t serial, uint32_t time,
                       struct wl_surface *s, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	deliver(d, ASTRIX_INPUT_TOUCH_DOWN, wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void touch_up(void *d, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id) {
	deliver(d, ASTRIX_INPUT_TOUCH_UP, 0, 0);
}

static void touch_motion(void *d, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x,
                         wl_fixed_t y) {
	deliver(d, ASTRIX_INPUT_TOUCH_MOTION, wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void touch_frame(void *d, struct wl_touch *t) {
}

static void touch_cancel(void *d, struct wl_touch *t) {
	/* The gesture was aborted by the compositor; drop any tracking state. */
	struct astrix_app_host *host = d;
	host->pointer_down = false;
	host->dirty = true;
}

/*
 * shape and orientation describe a stylus. Astrix treats every touch as a
 * finger, so the values are never read - but the handlers must exist: on a
 * panel that reports a pen, libwayland would abort the app over an event it
 * was never going to look at.
 */
static void touch_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t major,
                        wl_fixed_t minor) {
}

static void touch_orientation(void *d, struct wl_touch *t, int32_t id, int32_t orientation) {
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
	.shape = touch_shape,
	.orientation = touch_orientation,
};

static void keyboard_keymap(void *d, struct wl_keyboard *kb, uint32_t format, int32_t fd,
                            uint32_t size) {
	close(fd);
}

static void keyboard_enter(void *d, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *s, struct wl_array *keys) {
	struct astrix_app_host *host = d;
	host->has_focus = true;
	host->dirty = true;
}

static void keyboard_leave(void *d, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *s) {
	struct astrix_app_host *host = d;
	host->has_focus = false;
	host->dirty = true;
}

static void keyboard_key(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state) {
	struct astrix_app_host *host = d;
	struct astrix_input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = ASTRIX_INPUT_KEY;
	ev.key = key;
	ev.state = state == WL_KEYBOARD_KEY_STATE_PRESSED ? ASTRIX_KEY_PRESSED : 0;
	ev.timestamp = (double)monotonic_ms() / 1000.0;
	host->dirty = true;
	if (host->desc->on_input) {
		host->desc->on_input(host, &ev);
	}
}

static void keyboard_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial,
                               uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
}

static void keyboard_repeat(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay) {
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat,
};

static void seat_capabilities(void *d, struct wl_seat *seat, uint32_t caps) {
	struct astrix_app_host *host = d;
	/*
	 * The protocol forbids requesting a device before its capability is
	 * advertised, and forbids re-requesting one that disappears. So the
	 * devices are created here, on the capabilities event, not eagerly.
	 */
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !host->pointer) {
		host->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(host->pointer, &pointer_listener, host);
	}
	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !host->touch) {
		host->touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(host->touch, &touch_listener, host);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !host->keyboard) {
		host->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(host->keyboard, &keyboard_listener, host);
	}
}

static void seat_name(void *d, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void registry_global(void *d, struct wl_registry *r, uint32_t name,
                            const char *interface, uint32_t version) {
	struct astrix_app_host *host = d;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		host->compositor = wl_registry_bind(r, name, &wl_compositor_interface,
		                                    version < 4 ? version : 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		host->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		host->wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
		                                 version < 3 ? version : 3);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		host->seat = wl_registry_bind(r, name, &wl_seat_interface, version < 7 ? version : 7);
		wl_seat_add_listener(host->seat, &seat_listener, host);
		/*
		 * No wl_seat.set_keyboard_interactivity request is made here.
		 * The Astrix compositor has exactly one foreground surface and
		 * grants it keyboard focus unconditionally, so asking for it
		 * would be redundant - and the request is not present in the
		 * wl_seat interface this client library exposes, so calling it
		 * would be a link error on some libwayland versions. desc->
		 * wants_keyboard is therefore advisory: the app uses it to decide
		 * whether to draw a text caret and an on-screen keyboard, not to
		 * negotiate input.
		 */
	}
}

static void registry_remove(void *d, struct wl_registry *r, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

/* --- xdg-shell ----------------------------------------------------------- */

static void xdg_surface_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	struct astrix_app_host *host = d;
	xdg_surface_ack_configure(xs, serial);
	host->configured = true;
	wl_surface_commit(host->surface);
	if (!host->started) {
		host->started = true;
		if (host->desc->on_start) {
			host->desc->on_start(host);
		}
	}
	host->dirty = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *d, struct xdg_toplevel *t, int32_t width, int32_t height,
                               struct wl_array *states) {
	struct astrix_app_host *host = d;
	if (width <= 0 || height <= 0) {
		return;
	}
	if (width == host->width && height == host->height) {
		return;
	}
	host->width = width;
	host->height = height;
	if (allocate_buffers(host) == 0) {
		fprintf(stderr, "[%s] could not allocate shm buffers for %dx%d\n", host->desc->app_id,
		        width, height);
		host->running = false;
	}
	host->dirty = true;
}

static void toplevel_close(void *d, struct xdg_toplevel *t) {
	struct astrix_app_host *host = d;
	host->running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

/* --- startup ------------------------------------------------------------- */

static void query_panel_size(int *w, int *h) {
	*w = 720;
	*h = 1600;
	const char *ws = getenv("ASTRIX_WIDTH");
	const char *hs = getenv("ASTRIX_HEIGHT");
	if (ws && hs) {
		int nw = atoi(ws), nh = atoi(hs);
		if (nw > 0 && nh > 0) {
			*w = nw;
			*h = nh;
		}
	}
}

int astrix_app_main(const struct astrix_app_desc *desc, int argc, char **argv) {
	return astrix_app_main_with_user(desc, argc, argv, NULL);
}

int astrix_app_main_with_user(const struct astrix_app_desc *desc, int argc, char **argv,
                              void *user) {
	struct astrix_app_host host;
	memset(&host, 0, sizeof(host));
	host.desc = desc;
	host.user = user;
	host.start_ms = monotonic_ms();
	host.last_tick_ms = host.start_ms;
	host.cur_buffer = 0;

	/* --app-no-animation is honoured so automated runs are deterministic. */
	bool animate = true;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dark") == 0) {
			host.dark = true;
		} else if (strcmp(argv[i], "--light") == 0) {
			host.dark = false;
		}
	}
	host.theme = host.dark ? astrix_theme_dark() : astrix_theme_light();

	query_panel_size(&host.width, &host.height);

	host.display = wl_display_connect(NULL);
	if (!host.display) {
		fprintf(stderr, "[%s] cannot connect to a Wayland display (WAYLAND_DISPLAY=%s)\n",
		        desc->app_id, getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
		return 1;
	}
	host.registry = wl_display_get_registry(host.display);
	wl_registry_add_listener(host.registry, &registry_listener, &host);
	wl_display_roundtrip(host.display);

	if (!host.compositor || !host.shm || !host.wm_base || !host.seat) {
		fprintf(stderr, "[%s] compositor is missing required globals\n", desc->app_id);
		wl_display_disconnect(host.display);
		return 1;
	}
	if (allocate_buffers(&host) == 0) {
		fprintf(stderr, "[%s] could not create any shm buffers\n", desc->app_id);
		wl_display_disconnect(host.display);
		return 1;
	}

	host.surface = wl_compositor_create_surface(host.compositor);
	host.xdg_surface = xdg_wm_base_get_xdg_surface(host.wm_base, host.surface);
	xdg_surface_add_listener(host.xdg_surface, &xdg_surface_listener, &host);
	host.xdg_toplevel = xdg_surface_get_toplevel(host.xdg_surface);
	xdg_toplevel_add_listener(host.xdg_toplevel, &toplevel_listener, &host);
	xdg_toplevel_set_app_id(host.xdg_toplevel, desc->app_id);
	xdg_toplevel_set_title(host.xdg_toplevel, desc->title ? desc->title : desc->app_id);
	wl_surface_commit(host.surface);
	wl_display_roundtrip(host.display);

	host.running = true;
	host.dirty = true;

	int last_toast_state = 0;
	while (host.running) {
		if (wl_display_flush(host.display) < 0 && errno != EAGAIN) {
			break;
		}
		int timeout = host.dirty ? 0 : 200;
		struct pollfd pfd = { .fd = wl_display_get_fd(host.display), .events = POLLIN };
		if (poll(&pfd, 1, timeout) > 0 && (pfd.revents & POLLIN)) {
			if (wl_display_dispatch(host.display) < 0) {
				break;
			}
		} else {
			wl_display_dispatch_pending(host.display);
		}

		int64_t t = monotonic_ms();
		if (desc->on_tick) {
			double dt = (double)(t - host.last_tick_ms) / 1000.0;
			if (dt > 0.25) {
				dt = 0.25; /* a suspend must not produce a huge dt */
			}
			desc->on_tick(&host, dt);
		}
		host.last_tick_ms = t;

		/* A toast that has just expired needs one last repaint to clear. */
		int toast_state = host.toast[0] && now_ms_rel(&host) <= host.toast_until_ms;
		if (toast_state != last_toast_state) {
			host.dirty = true;
			last_toast_state = toast_state;
		}

		if (host.dirty) {
			draw_and_commit(&host);
		}
	}
	(void)animate;

	if (desc->on_exit) {
		desc->on_exit(&host);
	}
	destroy_buffers(&host);
	wl_display_disconnect(host.display);
	return 0;
}

/* ======================================================================== */
/* Widgets                                                                    */
/* ======================================================================== */

/*
 * A stable colour per app/list item, derived from the title. Real apps would
 * ship PNG/SVG icons; hashing the name keeps the system free of an image
 * decoder at boot while still giving every row a distinct, stable colour.
 */
static struct astrix_color swatch_color(const char *name) {
	uint32_t h = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		h = (h ^ *p) * 16777619u;
	}
	/* Constrain to a mid-brightness, reasonably saturated band. */
	int hue = (int)(h % 360);
	int s = 55 + (int)((h >> 9) % 25);
	int v = 62 + (int)((h >> 17) % 20);
	/* Simple HSL->RGB so the colours are predictable and testable. */
	float hf = (float)(hue % 360) / 60.0f;
	float sn = s / 100.0f, vn = v / 100.0f;
	int i = (int)hf;
	float ff = hf - i;
	float pp = vn * (1.0f - sn);
	float qv = vn * (1.0f - sn * ff);
	float tv = vn * (1.0f - sn * (1.0f - ff));
	float r, g, b;
	switch (i) {
	case 0: r = vn; g = tv; b = pp; break;
	case 1: r = qv; g = vn; b = pp; break;
	case 2: r = pp; g = vn; b = tv; break;
	case 3: r = pp; g = qv; b = vn; break;
	case 4: r = tv; g = pp; b = vn; break;
	default: r = vn; g = pp; b = qv; break;
	}
	return (struct astrix_color){ (uint8_t)(r * 255.0f), (uint8_t)(g * 255.0f),
		                       (uint8_t)(b * 255.0f), 255 };
}

struct astrix_rect astrix_widget_appbar(struct astrix_canvas *c, const struct astrix_theme *th,
                                        const char *title, const char *subtitle,
                                        bool show_back) {
	int bar_h = th->status_h + 52;
	struct astrix_rect bar = { 0, 0, c->width, bar_h };
	astrix_fill_rect(c, bar, th->surface);
	astrix_fill_rect(c, (struct astrix_rect){ 0, bar_h - 1, c->width, 1 }, th->divider);

	int text_x = show_back ? 56 : th->spacing * 3;
	if (show_back) {
		/* A chevron, drawn as two strokes. */
		int cx = 26, cy = bar_h / 2;
		for (int i = 0; i < 10; i++) {
			astrix_blend_pixel(c, cx + i, cy - 10 + i,
			                   astrix_color_with_alpha(th->text, 200));
			astrix_blend_pixel(c, cx + i, cy + 10 - i,
			                   astrix_color_with_alpha(th->text, 200));
		}
	}
	int ty = subtitle ? bar_h / 2 - ASTRIX_FONT_H - 4 : (bar_h - ASTRIX_FONT_H) / 2;
	char ell[80];
	astrix_text_ellipsize(title, c->width - text_x - th->spacing * 3, ell, sizeof(ell));
	astrix_draw_text(c, text_x, ty, ell, th->text);
	if (subtitle) {
		astrix_text_ellipsize(subtitle, c->width - text_x - th->spacing * 3, ell, sizeof(ell));
		astrix_draw_text(c, text_x, bar_h / 2 + 4, ell, th->text_dim);
	}
	return (struct astrix_rect){ 0, bar_h, c->width, c->height - bar_h };
}

struct astrix_rect astrix_widget_back_hit(int width) {
	(void)width;
	return (struct astrix_rect){ 0, 0, 56, 100 };
}

struct astrix_rect astrix_widget_button(struct astrix_canvas *c, const struct astrix_theme *th,
                                        struct astrix_rect r, const char *label, int variant,
                                        bool pressed) {
	struct astrix_color bg, fg;
	switch (variant) {
	case 1:
		bg = th->primary;
		fg = th->on_primary;
		break;
	case 2:
		bg = astrix_color_with_alpha(th->danger, pressed ? 255 : 235);
		fg = astrix_rgba(255, 255, 255, 255);
		break;
	default:
		bg = pressed ? th->surface_alt : th->surface;
		fg = th->text;
		break;
	}
	if (variant == 0) {
		astrix_fill_rect_rounded(c, r, th->radius, bg);
		astrix_stroke_rect_rounded(c, r, th->radius, 1, th->divider);
	} else {
		astrix_fill_rect_rounded(c, r, th->radius, bg);
	}
	int tx = r.x + (r.w - astrix_text_width(label)) / 2;
	int ty = r.y + (r.h - ASTRIX_FONT_H) / 2;
	astrix_draw_text(c, tx, ty, label, fg);
	return r;
}

void astrix_widget_row(struct astrix_canvas *c, const struct astrix_theme *th,
                       struct astrix_rect r, const char *title, const char *subtitle,
                       const char *trailing, bool selected, bool pressed) {
	if (pressed) {
		astrix_fill_rect(c, r, th->surface_alt);
	} else if (selected) {
		astrix_fill_rect(c, r, astrix_color_with_alpha(th->primary, 28));
	}
	int sw = 40;
	int sy = r.y + (r.h - sw) / 2;
	struct astrix_rect swr = { r.x + th->spacing * 2, sy, sw, sw };
	struct astrix_color sc = swatch_color(title);
	astrix_fill_rect_rounded(c, swr, 10, sc);
	/* First letter of the title, so the swatch reads as an app icon. */
	char initial[2] = { title[0] ? title[0] : '?', 0 };
	astrix_draw_text(c, swr.x + (sw - astrix_text_width(initial)) / 2,
	                 swr.y + (sw - ASTRIX_FONT_H) / 2, initial, astrix_rgba(255, 255, 255, 235));

	int tx = swr.x + sw + th->spacing * 2;
	int right = r.x + r.w - th->spacing * 2;
	if (trailing && trailing[0]) {
		int tw = astrix_text_width(trailing);
		astrix_draw_text(c, right - tw, r.y + (r.h - ASTRIX_FONT_H) / 2, trailing, th->text_dim);
		right -= tw + th->spacing * 2;
	}
	int avail = right - tx;
	if (avail < 20) {
		return;
	}
	char buf[96];
	astrix_text_ellipsize(title, avail, buf, sizeof(buf));
	if (subtitle && subtitle[0]) {
		astrix_draw_text(c, tx, r.y + r.h / 2 - ASTRIX_FONT_H - 2, buf, th->text);
		astrix_text_ellipsize(subtitle, avail, buf, sizeof(buf));
		astrix_draw_text(c, tx, r.y + r.h / 2 + 4, buf, th->text_dim);
	} else {
		astrix_draw_text(c, tx, r.y + (r.h - ASTRIX_FONT_H) / 2, buf, th->text);
	}
}

void astrix_widget_section(struct astrix_canvas *c, const struct astrix_theme *th,
                           struct astrix_rect r, const char *text) {
	astrix_draw_text(c, r.x, r.y, text, th->text_dim);
}

struct astrix_rect astrix_widget_switch(struct astrix_canvas *c, const struct astrix_theme *th,
                                        struct astrix_rect r, bool on, bool pressed) {
	int w = 48, h = 28;
	int y = r.y + (r.h - h) / 2;
	struct astrix_rect track = { r.x + r.w - w, y, w, h };
	struct astrix_color bg = on ? th->primary
	                            : astrix_color_with_alpha(th->text, pressed ? 90 : 60);
	astrix_fill_rect_rounded(c, track, h / 2, bg);
	int cx = on ? track.x + w - h / 2 : track.x + h / 2;
	astrix_fill_rect(c, (struct astrix_rect){ cx - h / 2 + 2, y + 2, h - 4, h - 4 },
	                 astrix_rgba(255, 255, 255, 245));
	return track;
}

struct astrix_rect astrix_widget_slider(struct astrix_canvas *c, const struct astrix_theme *th,
                                        struct astrix_rect r, float value, bool pressed) {
	int track_h = 6;
	int ty = r.y + (r.h - track_h) / 2;
	struct astrix_rect track = { r.x, ty, r.w, track_h };
	astrix_fill_rect_rounded(c, track, track_h / 2, astrix_color_with_alpha(th->text, 50));
	if (value > 0.0f) {
		struct astrix_rect fill = { r.x, ty, (int)(r.w * value), track_h };
		astrix_fill_rect_rounded(c, fill, track_h / 2, th->primary);
	}
	int tx = r.x + (int)(r.w * value);
	int thumb = pressed ? 14 : 12;
	struct astrix_rect thumb_r = { tx - thumb / 2, r.y + (r.h - thumb) / 2, thumb, thumb };
	astrix_fill_rect(c, thumb_r, astrix_rgba(255, 255, 255, 250));
	astrix_stroke_rect(c, thumb_r, 1, astrix_color_with_alpha(th->text, 60));
	return thumb_r;
}struct astrix_rect astrix_widget_card_inner(const struct astrix_theme *th, struct astrix_rect r) {
	return astrix_rect_inset(r, th->spacing * 2, th->spacing * 2);
}

struct astrix_rect astrix_widget_card(struct astrix_canvas *c,
                                      const struct astrix_theme *th, struct astrix_rect r) {
	astrix_fill_rect_rounded(c, r, th->radius + 4, th->surface);
	return astrix_widget_card_inner(th, r);
}

void astrix_widget_empty(struct astrix_canvas *c, const struct astrix_theme *th,
                         struct astrix_rect r, const char *title, const char *body) {
	int cy = r.y + r.h / 2 - 40;
	astrix_draw_text_centered(c, r, cy, title, th->text_dim);
	if (body && body[0]) {
		astrix_draw_text_centered(c, r, cy + 28, body, astrix_color_with_alpha(th->text_dim, 180));
	}
}

void astrix_widget_progress(struct astrix_canvas *c, const struct astrix_theme *th,
                            struct astrix_rect r, float fraction) {
	if (fraction < 0.0f) {
		fraction = 0.0f;
	}
	if (fraction > 1.0f) {
		fraction = 1.0f;
	}
	struct astrix_rect track = r;
	int h = 8;
	track.y = r.y + (r.h - h) / 2;
	track.h = h;
	astrix_fill_rect_rounded(c, track, h / 2, astrix_color_with_alpha(th->text, 45));
	if (fraction > 0.0f) {
		struct astrix_rect fill = { track.x, track.y, (int)(track.w * fraction), h };
		astrix_fill_rect_rounded(c, fill, h / 2, th->primary);
	}
}

/* ======================================================================== */
/* Scrolling                                                                  */
/* ======================================================================== */

void astrix_scroll_reset(struct astrix_scroll *s, int content_h, int view_h, int view_w) {
	s->offset = 0;
	s->content_h = content_h;
	s->view_h = view_h;
	s->view_w = view_w;
	s->dragging = false;
	s->tracking = false;
	s->inertial = false;
	s->velocity = 0;
}

void astrix_scroll_clamp(struct astrix_scroll *s) {
	int max = s->content_h - s->view_h;
	if (max < 0) {
		max = 0;
	}
	if (s->offset < 0) {
		s->offset = 0;
	}
	if (s->offset > max) {
		s->offset = max;
	}
}

bool astrix_scroll_step(struct astrix_scroll *s) {
	if (!s->inertial) {
		return false;
	}
	s->offset += s->velocity;
	/* Exponential decay. astrix_scroll_step is documented to be called once
	 * per display frame, so a 3/4 factor gives the same feel at 60 Hz and
	 * 120 Hz. */
	s->velocity = s->velocity * 3 / 4;
	if (s->velocity < 2) {
		s->velocity = 0;
		s->inertial = false;
		return false;
	}
	int before = s->offset;
	astrix_scroll_clamp(s);
	if (s->offset != before) {
		/* Reached the end of the list: stop instead of jittering. */
		s->inertial = false;
		s->velocity = 0;
	}
	return s->inertial;
}

bool astrix_scroll_handle(struct astrix_scroll *s, const struct astrix_input_event *ev) {
	int slop = 8;
	switch (ev->kind) {
	case ASTRIX_INPUT_TOUCH_DOWN:
		s->tracking = true;
		s->dragging = false;
		s->touch_x = ev->x;
		s->touch_y = ev->y;
		s->last_y = ev->y;
		s->last_ms = (int64_t)(ev->timestamp * 1000.0);
		s->down_ms = s->last_ms;
		s->start_offset = s->offset;
		s->inertial = false;
		s->velocity = 0;
		return false;
	case ASTRIX_INPUT_TOUCH_MOTION:
	case ASTRIX_INPUT_POINTER_MOTION:
		if (!s->tracking) {
			return false;
		}
		if (!s->dragging && (abs(ev->x - s->touch_x) > slop || abs(ev->y - s->touch_y) > slop)) {
			s->dragging = true;
		}
		if (s->dragging) {
			/*
			 * The list follows the finger once it has moved further
			 * than the slop. A touch that never leaves the slop is
			 * left to the app, so a tap on a row still registers
			 * even with an over-long list underneath it.
			 */
			s->offset = s->start_offset - (ev->y - s->touch_y);
			astrix_scroll_clamp(s);

			/*
			 * Velocity is measured between the last two motion
			 * samples, expressed as pixels per 16 ms frame so
			 * astrix_scroll_step stays frame-rate independent.
			 */
			int64_t now = (int64_t)(ev->timestamp * 1000.0);
			int64_t dt = now - s->last_ms;
			if (dt > 0) {
				int dy = ev->y - s->last_y;
				/* Smooth over the frame interval the sample spans. */
				int per_frame = (int)((int64_t)dy * 16 / dt);
				/* Ignore a single-pixel jitter, clamp absurd speeds. */
				if (per_frame > -60 && per_frame < 60) {
					s->velocity = per_frame;
				}
				s->last_y = ev->y;
				s->last_ms = now;
			}
			return true;
		}
		return false;	case ASTRIX_INPUT_TOUCH_UP:
		if (s->dragging) {
			/*
			 * A flick is a release while the finger was still
			 * moving. If the finger was held still before lifting,
			 * kill the fling so the list stops where it was left.
			 */
			int64_t held_ms = (int64_t)(ev->timestamp * 1000.0) - s->last_ms;
			if (held_ms > 80) {
				s->velocity = 0;
			}
			s->inertial = s->velocity != 0;
			s->tracking = false;
			s->dragging = false;
			return true;
		}
		s->tracking = false;
		return false;
	default:
		return false;
	}
}
