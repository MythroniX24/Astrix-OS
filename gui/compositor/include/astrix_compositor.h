/*
 * Astrix OS - compositor internal API.
 *
 * Written against wlroots 0.18 (the version Debian trixie ships), verified
 * against the upstream examples for that release rather than from memory.
 * The 0.18 API differs from 0.15/0.16 in several ways that matter here:
 *   - wlr_renderer_autocreate() takes only a backend; the renderer type is
 *     selected from the environment/heuristics.
 *   - wlr_output_init_render() is required before an output can be used.
 *   - wlr_scene_output_commit() takes scene_output_state_options.
 *   - the seat no longer stores a pointer/touch device; capabilities are
 *     advertised with wlr_seat_set_capabilities().
 */

#ifndef ASTRIX_COMPOSITOR_H
#define ASTRIX_COMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

/* --- configuration ------------------------------------------------------- */

struct astrix_config {
	int width;          /* 0 = adopt the output's own mode   */
	int height;
	int scale;
	int refresh_hz;     /* 0 = output decides               */
	bool headless;      /* run without a real display       */
};

void astrix_config_init(struct astrix_config *cfg);

/* --- diagnostics --------------------------------------------------------- */

/*
 * What the compositor can actually see of the graphics hardware. Printed on
 * the failure path so a session that cannot start says *why* rather than just
 * "no backend". Every field is a fixed-size buffer: these end up in a log line
 * on a device with no display, and a truncation there is better than an
 * unbounded copy into it.
 */
struct astrix_drm_probe {
	char render_dev[64];  /* state of /dev/dri/renderD128        */
	char card_dev[32];    /* whether /dev/dri/card0 exists       */
	char card_open[64];   /* result of opening card0 O_RDWR      */
};

struct astrix_drm_probe astrix_drm_probe(void);

/*
 * The KMS card node the compositor drives. Overridable per device, because a
 * phone with two GPU vendors may well put the panel on card1. Astrix owns the
 * DRM master on this node, which is the one genuinely privileged thing the
 * graphical session does; astrix-compositor.service grants access to exactly
 * this path and nothing else.
 */
#define ASTRIX_DRM_CARD "/dev/dri/card0"

/* --- touch tracking ------------------------------------------------------ */

struct astrix_touch_point {
	struct wl_list link;
	int32_t id;
	double x, y;        /* normalised 0..1 from libinput */
	int px, py;         /* resolved pixel coordinates     */
	double dx, dy;      /* delta since last motion        */
	uint32_t time_msec;
	bool active;
};

/* --- output -------------------------------------------------------------- */

struct astrix_output {
	struct astrix_server *server;
	struct wlr_output *wlr_output;
	struct wl_list link;                 /* server->outputs */
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener description;   /* mode/scale changed */
	struct wlr_scene_output *scene_output;
	int width, height;
	int x, y;
	bool initialized;
};

/* --- input device wrappers ----------------------------------------------- */

struct astrix_keyboard {
	struct astrix_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_list link;                /* server->keyboards */
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
	/*
	 * True for a keyboard a *client* created through
	 * zwp_virtual_keyboard_v1 - that is, an on-screen keyboard - as
	 * opposed to one libinput found. The difference matters on teardown:
	 * a virtual keyboard's disappearance must not take the seat's
	 * keyboard capability away, because the physical keyboard that
	 * granted it may still be attached. It also carries no wlr_input_device
	 * of its own, so its destroy signal lives on wlr_keyboard.base.
	 */
	bool virtual_kb;
};

struct astrix_pointer {
	struct astrix_server *server;
	struct wlr_pointer *wlr_pointer;
	struct wl_list link;                /* server->pointers */
	struct wl_listener motion;
	struct wl_listener motion_absolute;
	struct wl_listener button;
	struct wl_listener axis;
	struct wl_listener destroy;

	/*
	 * System-gesture bookkeeping for the current press.
	 *
	 * The shell's home / app-switcher gestures can only work if the
	 * compositor is willing to take input back from a focused app, because
	 * input goes to whichever toplevel is focused. Without that, opening an
	 * app is a one-way door: the app keeps the focus, the shell never sees
	 * another pointer event, and there is no way home until the app exits
	 * by itself.
	 *
	 * down_x/down_y is where this press landed, in layout coordinates.
	 * gesture_armed means "this press started in the bottom system band",
	 * so it *may* turn into a system gesture; gesture_claimed means it did.
	 */
	double down_x, down_y;
	bool gesture_armed;
	bool gesture_claimed;
};

struct astrix_touch {
	struct astrix_server *server;
	struct wlr_touch *wlr_touch;
	struct wl_list link;                /* server->touch */
	struct wl_listener down;
	struct wl_listener up;
	struct wl_listener motion;
	struct wl_listener cancel;
	struct wl_listener destroy;
};

/* --- server -------------------------------------------------------------- */

struct astrix_server {
	struct wl_display *wl_display;
	struct wl_event_loop *wl_event_loop;

	bool display_live;  /* the wl_display may still be terminated      */
	struct wlr_backend *backend;
	/* True only once a real DRM/KMS backend has been built, i.e. there is
	 * genuinely a panel behind this session. Set on the one path that opens
	 * KMS and not by any fallback, because the difference between "the
	 * session started" and "something can be seen" is exactly the thing a
	 * headless fallback would hide. */
	bool real_display;
	/* The libseat/logind session the DRM backend is bound to. wlroots 0.18
	 * will not create a DRM backend without one, and it has to outlive the
	 * backend it gave out the device through, so it is kept here and torn
	 * down after the backend. */
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *wlr_compositor;
	struct wlr_subcompositor *subcompositor;
	struct wlr_data_device_manager *data_device_manager;
	struct wlr_primary_selection_v1_device_manager *primary_selection_manager;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_seat *seat;
	/*
	 * Kept, not discarded, so its new_virtual_keyboard signal can be
	 * watched for the lifetime of the session. See input.c: a virtual
	 * keyboard never arrives through the backend.
	 */
	struct wlr_virtual_keyboard_manager_v1 *vkbd_manager;

	struct wlr_output_layout *output_layout;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;

	struct wlr_scene *scene;
	struct wlr_scene_output *scene_output;
	/* The scene's root tree. Every toplevel is parented to this. */
	struct wlr_scene_tree *scene_root;

	struct astrix_config config;

	struct wl_list outputs;
	struct wl_list keyboards;
	struct wl_list pointers;
	struct wl_list touch;
	struct wl_list touchpoints;
	struct wl_list toplevels;          /* struct astrix_toplevel */

	struct wl_listener new_output;
	struct wl_listener new_input;
	struct wl_listener backend_destroy;
	struct wl_listener new_toplevel;
	struct wl_listener new_virtual_keyboard;

	/* Set when the shell's surface commits, so we can re-evaluate which
	 * client is the foreground app. */
	struct wlr_surface *active_surface;
	struct wlr_surface *previous_surface;
	/*
	 * The Astrix Shell's own surface. It is a Wayland client like any app,
	 * but it is special in one way: it is the fallback input target. When
	 * an app is unmapped and no other client is foreground, the compositor
	 * routes input back to the shell so the home screen is never dead.
	 */
	struct wlr_xdg_toplevel *shell_toplevel;

	/*
	 * The surface the seat's keyboard focus was last pushed to, and
	 * whether we have already complained about having no keyboard at all.
	 * Purely diagnostic: without it, "the on-screen keyboard types nothing"
	 * is indistinguishable from "the shell never had focus", because
	 * neither path logs anything.
	 */
	struct wlr_surface *keyboard_focus;
	bool warned_no_seat_keyboard;

	struct timespec last_present;
	struct wl_event_source *frame_timer;
	struct wl_event_source *repaint_timer;
	bool running;
	bool dirty;
};

/* Entry point. Returns process exit status. */
int astrix_compositor_run(const struct astrix_config *cfg);

/* Mark the scene dirty and wake the repaint timer. */
void astrix_server_invalidate(struct astrix_server *server);

/* Resolve a normalised (0..1) touch coordinate to pixels on an output. */
void astrix_touch_to_pixels(struct astrix_server *server, double nx, double ny, int *px, int *py);

void astrix_log(enum wlr_log_importance level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Provided by input.c */
void astrix_input_init(struct astrix_server *server);
void astrix_input_finish(struct astrix_server *server);
/* wl_signal listener: manager->events.new_virtual_keyboard */
void astrix_input_virtual_keyboard(struct wl_listener *listener, void *data);

/* Provided by shell.c */
void astrix_shell_init(struct astrix_server *server);
void astrix_shell_finish(struct astrix_server *server);
struct wlr_xdg_toplevel *astrix_shell_focused_toplevel(struct astrix_server *server);
/*
 * Re-send keyboard focus to whichever toplevel is currently foreground.
 *
 * wlroots tracks which surface *has* focus, but it does not decide *which*
 * surface should get it - that is the compositor's policy. Without this call
 * no client ever receives wl_keyboard.enter, so a terminal app would show a
 * cursor blinking and never see a keystroke. It is also needed after a
 * keyboard is hotplugged, because the key state and modifier state have to be
 * pushed to the new client.
 */
void astrix_shell_apply_keyboard_focus(struct astrix_server *server);
/* The surface that currently receives pointer/touch input, or NULL. */
struct wlr_surface *astrix_shell_input_target(struct astrix_server *server);

/*
 * Give up foreground app focus, so the shell becomes the input target again.
 * Used when a system gesture claims the pointer (see astrix_pointer above).
 */
void astrix_shell_release_focus(struct astrix_server *server);

#endif /* ASTRIX_COMPOSITOR_H */
