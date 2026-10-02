/*
 * Fixture: the compositor focus code as it was BEFORE bug 33 was fixed.
 *
 * This file exists only so tests/test-compositor-focus.sh can be shown to
 * actually fail on the broken shape. It is never compiled and never shipped.
 *
 *     ASTRIX_FOCUS_SRC=build/focus-regression-fixture.c \
 *       bash tests/test-compositor-focus.sh   # must fail
 */
#include <stdbool.h>

struct astrix_toplevel {
	struct astrix_server *server;
	bool focused;
};

struct astrix_server {
	struct wl_list toplevels;
};

/* The bug: the server is read from a toplevel that may be NULL. */
static void set_focused_toplevel(struct astrix_toplevel *toplevel) {
	struct astrix_server *server = toplevel->server;
	struct astrix_toplevel *t, *tmp;

	wl_list_for_each_safe(t, tmp, &server->toplevels, link) {
		if (t != toplevel && t->focused) {
			t->focused = false;
			wlr_xdg_toplevel_set_activated(t->xdg_toplevel, false);
		}
	}

	if (toplevel) {
		toplevel->focused = true;
	} else {
		server->active_surface = astrix_shell_input_target(server);
	}
}

static void toplevel_unmap_handler(struct astrix_toplevel *t) {
	if (t->focused) {
		set_focused_toplevel(NULL);
	}
}

static void toplevel_request_minimize_handler(struct astrix_toplevel *t) {
	set_focused_toplevel(NULL);
}

static void toplevel_map_handler(struct astrix_toplevel *t) {
	set_focused_toplevel(t);
}