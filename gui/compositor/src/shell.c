/*
 * Astrix OS - xdg-shell surface management and focus policy.
 *
 * Written against wlroots 0.18, where xdg_toplevel no longer uses a listener
 * struct: everything is a wl_signal on toplevel->events, and map/unmap live
 * on the *surface* (wlr_surface.events.map / .unmap), not the role object.
 * Title and app_id are plain char* fields on the toplevel.
 *
 * Mobile focus policy
 * -------------------
 * A phone has no overlapping windows. Exactly one client is foreground at a
 * time and it is always fullscreen. When no app is focused the shell owns the
 * screen and draws the home screen. This is the whole of "window management"
 * in Astrix, and it is intentionally tiny.
 */

#include "astrix_compositor.h"

#include <stdlib.h>
#include <string.h>

struct astrix_toplevel {
	struct astrix_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wl_list link;                 /* server->toplevels */

	/* Surface-level events (map/unmap/commit live on wlr_surface in 0.18). */
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener surface_destroy;

	/* Toplevel-level requests. */
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_minimize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;

	/* Destroyed when the xdg_toplevel role object goes away. */
	struct wl_listener toplevel_destroy;

	bool mapped;
	bool focused;
	/*
	 * Set once this toplevel has been torn down.
	 *
	 * A client going away fires *both* the surface destroy and the role
	 * destroy, and wlroots unmaps the surface in between. Each of those
	 * events still walks the listener list, and the listeners live inside
	 * this struct - so freeing it from one handler leaves dangling links
	 * in the others' signal lists, and the next emit dereferences freed
	 * memory. The crash was `wl_client_destroy -> wlr_surface_unmap ->
	 * wl_signal_emit_mutable -> <freed toplevel>`, a SIGSEGV that took
	 * the whole session down whenever an app was closed.
	 */
	bool torn_down;
	/* True once we have sent this toplevel's first configure. */
	bool configured;
	/*
	 * The scene-graph node that displays this toplevel.
	 *
	 * This is not optional bookkeeping. In wlroots a wlr_surface only
	 * becomes `mapped`, and only emits map/unmap, once it has been added to
	 * a scene tree. Without this node a client connects, configures,
	 * commits - and never appears on screen. wlr_scene_xdg_surface_create
	 * also picks up the surface's subsurfaces and its popups for free,
	 * which is why it is used instead of the lower-level surface API.
	 */
	struct wlr_scene_tree *scene_tree;
};

/* --- focus policy -------------------------------------------------------- */

/* The app_id the Astrix Shell advertises. Matched exactly. */
#define ASTRIX_SHELL_APP_ID "org.astrix.Shell"

/*
 * The surface that should receive input right now.
 *
 * The shell draws the home screen, launcher, lock screen, notification shade
 * and power menu itself, so it has to be able to take input back from an app
 * the moment that app closes. Without this fallback the device would go
 * unresponsive after the first app exit, which is the single most common way
 * a mobile shell breaks.
 */
struct wlr_surface *astrix_shell_input_target(struct astrix_server *server) {
	struct wlr_xdg_toplevel *focused = astrix_shell_focused_toplevel(server);
	if (focused) {
		return focused->base->surface;
	}
	if (server->shell_toplevel && server->shell_toplevel->base->surface->mapped) {
		return server->shell_toplevel->base->surface;
	}
	return NULL;
}

/*
 * Push the foreground surface (and its key state) to the seat's active
 * keyboard.
 *
 * The Astrix policy is deliberately unconditional: an app that maps becomes the
 * keyboard's recipient without having to ask for interactivity. A phone has
 * one foreground surface, so there is never a case where the "right" answer is
 * ambiguous, and skipping the negotiation removes a whole class of bug where
 * an app draws a text field but never receives input.
 */
void astrix_shell_apply_keyboard_focus(struct astrix_server *server) {
	if (!server->seat) {
		return;
	}
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (!kb) {
		/*
		 * No keyboard on the seat yet. wlroots sends neither
		 * wl_keyboard.enter nor a keymap without one, so every key press
		 * in the meantime is dropped - silently. The same call runs again
		 * when a keyboard appears, so this is a delay and not a failure,
		 * but it is the single most useful thing to have said out loud
		 * when on-screen typing "does nothing": say it once, not on every
		 * focus change.
		 */
		if (!server->warned_no_seat_keyboard) {
			server->warned_no_seat_keyboard = true;
			astrix_log(WLR_ERROR,
			           "no keyboard on the seat: keys will be dropped until one "
			           "appears");
		}
		return;
	}
	server->warned_no_seat_keyboard = false;
	struct wlr_surface *target = astrix_shell_input_target(server);
	if (target != server->keyboard_focus) {
		server->keyboard_focus = target;
		astrix_log(WLR_INFO, "keyboard focus -> %s",
		           target ? (target == (server->shell_toplevel
		                                    ? server->shell_toplevel->base->surface
		                                    : NULL)
		                          ? "the Astrix Shell"
		                          : "a foreground app")
		                  : "nothing (no client has keyboard focus)");
	}
	if (!target) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		return;
	}
	/*
	 * A client needs the full keymap at the moment of the enter event,
	 * including which keys are currently held down, or its own modifier
	 * tracking starts from the wrong state.
	 */
	wlr_seat_keyboard_notify_enter(server->seat, target, kb->keycodes, kb->num_keycodes,
	                               &kb->modifiers);
}

struct wlr_xdg_toplevel *astrix_shell_focused_toplevel(struct astrix_server *server) {
	struct astrix_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->focused) {
			return t->xdg_toplevel;
		}
	}
	return NULL;
}

/*
 * Focus `toplevel`, or focus nothing when it is NULL ("go home").
 *
 * The server is a separate parameter on purpose. `toplevel` is NULL on every
 * path where focus is being *cleared* - an app closing, an app asking to be
 * minimised, the shell losing the foreground - and those are exactly the
 * paths that run while a client is being destroyed. Deriving the server from
 * `toplevel->server` therefore dereferenced NULL in the one case where
 * there is no toplevel to derive it from:
 *
 *   astrix-compositor[745]: fatal signal, backtrace follows:
 *   ... astrix-compositor(+0x6254)            set_focused_toplevel, shell.c:147
 *   ... wl_signal_emit_mutable+0x90
 *   ... wlr_surface_unmap+0x40                our unmap handler
 *   ... wl_client_destroy+0x90
 *   sig=11
 *
 * The compositor took the whole session down every time an app exited. Bug 33.
 */
static void set_focused_toplevel(struct astrix_server *server, struct astrix_toplevel *toplevel) {
	struct astrix_toplevel *t, *tmp;

	if (!server) {
		return;
	}

	/* Deactivate every other toplevel; a phone shows one app at a time. */
	wl_list_for_each_safe(t, tmp, &server->toplevels, link) {
		if (t != toplevel && t->focused) {
			t->focused = false;
			wlr_xdg_toplevel_set_activated(t->xdg_toplevel, false);
		}
	}

	if (toplevel) {
		toplevel->focused = true;
		wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
		server->active_surface = toplevel->xdg_toplevel->base->surface;
		astrix_log(WLR_INFO, "focus app: %s",
		           toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title
		                                        : "(untitled)");
	} else {
		server->active_surface = astrix_shell_input_target(server);
		astrix_log(WLR_INFO, "no app focused; Astrix Shell owns the screen");
	}
	/*
	 * Focus and keyboard focus are decided together. If they diverged, an
	 * app could be on screen (focused) while the keyboard was still
	 * pointed at whatever was there before.
	 */
	astrix_shell_apply_keyboard_focus(server);
	astrix_server_invalidate(server);
}

/*
 * Drop foreground app focus so the shell takes input back.
 *
 * This is what makes "go home" and "app switcher" reachable at all. Input
 * follows focus, so an app that has focus holds the pointer hostage: the
 * shell's gestures never fire and the user is stuck inside the app. The
 * compositor calls this when a gesture starting in the bottom system band
 * turns out to be a swipe, which is the compositor's only input policy
 * decision - what the gesture *means* is still the shell's business.
 */
void astrix_shell_release_focus(struct astrix_server *server) {
	set_focused_toplevel(server, NULL);
}

/* --- surface events ------------------------------------------------------ */

static void toplevel_map_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, map);
	t->mapped = true;
	/*
	 * Recognise the Astrix Shell by its app_id. It draws the system UI, so
	 * it has to be recorded as the fallback input target.
	 */
	if (t->xdg_toplevel->app_id &&
	    strcmp(t->xdg_toplevel->app_id, ASTRIX_SHELL_APP_ID) == 0) {
		t->server->shell_toplevel = t->xdg_toplevel;
		astrix_log(WLR_INFO, "Astrix Shell surface mapped");
	}
	/*
	 * Phone apps are always fullscreen. Granting both fullscreen and
	 * maximized on map means an app can render immediately without
	 * negotiating a desktop-style geometry first.
	 */
	wlr_xdg_toplevel_set_fullscreen(t->xdg_toplevel, true);
	wlr_xdg_toplevel_set_maximized(t->xdg_toplevel, true);
	wlr_xdg_toplevel_set_activated(t->xdg_toplevel, true);
	/*
	 * The shell must never steal focus from a real app. It is a normal
	 * client that maps, but it is the *fallback*, not a peer: when it
	 * maps and nothing else is focused, it simply becomes the input
	 * target. It is deliberately not marked focused, so that an app
	 * mapping later still wins.
	 */
	if (t->xdg_toplevel == t->server->shell_toplevel && !astrix_shell_focused_toplevel(t->server)) {
		t->server->active_surface = astrix_shell_input_target(t->server);
		astrix_shell_apply_keyboard_focus(t->server);
		astrix_server_invalidate(t->server);
		return;
	}
	set_focused_toplevel(t->server, t);
}

static void toplevel_unmap_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, unmap);
	/* Defensive: a client that disconnects is unmapped and then destroyed,
	 * and the destroy path may already have torn this record down. */
	if (t->torn_down) {
		return;
	}
	t->mapped = false;
	/*
	 * Remove it from the scene so it stops being composited. The node owns
	 * no client resources, so dropping it here is safe and immediate.
	 */
	if (t->scene_tree) {
		/* struct wlr_scene_tree embeds the node, so &tree->node is the node. */
		wlr_scene_node_destroy(&t->scene_tree->node);
		t->scene_tree = NULL;
	}
	if (t->server->shell_toplevel == t->xdg_toplevel) {
		t->server->shell_toplevel = NULL;
	}
	if (t->focused) {
		set_focused_toplevel(t->server, NULL);
	} else {
		/*
		 * Re-evaluate anyway: unmapping a background surface can change
		 * whether the shell is now the input target.
		 */
		t->server->active_surface = astrix_shell_input_target(t->server);
		astrix_shell_apply_keyboard_focus(t->server);
	}
}

static void toplevel_commit_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, commit);
	if (t->torn_down) {
		return;
	}
	/*
	 * Send the first configure.
	 *
	 * xdg-shell requires the client to commit its surface once before it has
	 * a buffer, and requires the compositor to answer that commit with a
	 * configure. A conforming client (our shell included) waits for that
	 * configure and does not draw until it arrives - so omitting it does not
	 * degrade, it deadlocks: the client connects, registers a toplevel, and
	 * then sits there forever with an unmapped surface.
	 *
	 * It is sent on the first commit rather than at construction time
	 * because the configure carries the geometry the client is expected to
	 * adopt, and before the first commit there is no client state to report.
	 */
	if (!t->configured) {
		t->configured = true;
		wlr_xdg_surface_schedule_configure(t->xdg_toplevel->base);
		astrix_log(WLR_DEBUG, "sent initial configure to a new toplevel");
	}

	astrix_server_invalidate(t->server);
}

/*
 * The single teardown path for a toplevel.
 *
 * Every way a toplevel can go away - the surface being destroyed or the role
 * object being destroyed - ends up here, and the flag makes the second call
 * a no-op. Crucially this unregisters *every* listener before freeing: a
 * listener left behind points into freed memory, and the next signal emit on
 * that list dereferences it.
 *
 * scene_tree is destroyed here rather than in one of the handlers because a
 * node for a dead surface would otherwise stay in the scene graph, leaving an
 * app's last frame frozen on screen.
 */
static void toplevel_finish(struct astrix_toplevel *t) {
	if (!t || t->torn_down) {
		return;
	}
	t->torn_down = true;

	if (t->focused) {
		/*
		 * Clear the flag before asking for focus to be dropped. The record
		 * is still on server->toplevels, so leaving it marked focused would
		 * let astrix_shell_input_target() hand input to a toplevel that is
		 * being freed on this very call.
		 */
		t->focused = false;
		set_focused_toplevel(t->server, NULL);
	}
	if (t->server && t->server->shell_toplevel == t->xdg_toplevel) {
		t->server->shell_toplevel = NULL;
	}
	if (t->scene_tree) {
		wlr_scene_node_destroy(&t->scene_tree->node);
		t->scene_tree = NULL;
	}
	if (t->server) {
		astrix_server_invalidate(t->server);
	}

	/* Safe to remove an already-removed wl_list link only once, so each
	 * listener is removed exactly here and nowhere else. */
	wl_list_remove(&t->map.link);
	wl_list_remove(&t->unmap.link);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->surface_destroy.link);
	wl_list_remove(&t->request_maximize.link);
	wl_list_remove(&t->request_fullscreen.link);
	wl_list_remove(&t->request_move.link);
	wl_list_remove(&t->request_resize.link);
	wl_list_remove(&t->request_minimize.link);
	wl_list_remove(&t->set_title.link);
	wl_list_remove(&t->set_app_id.link);
	wl_list_remove(&t->toplevel_destroy.link);
	wl_list_remove(&t->link);
	free(t);
}

static void toplevel_surface_destroy_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, surface_destroy);
	toplevel_finish(t);
}

static void toplevel_role_destroy_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, toplevel_destroy);
	/* Read the title before the teardown frees the record. */
	astrix_log(WLR_INFO, "app closed: %s",
	           t->xdg_toplevel && t->xdg_toplevel->title ? t->xdg_toplevel->title
	                                                    : "(untitled)");
	/*
	 * A client can destroy its xdg_toplevel role without ever unmapping
	 * the surface, and can be destroyed surface-first as well, so this
	 * must be the same idempotent teardown rather than a second free.
	 */
	toplevel_finish(t);
}

/* --- toplevel requests --------------------------------------------------- */

static void toplevel_request_maximize_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, request_maximize);
	wlr_xdg_toplevel_set_maximized(t->xdg_toplevel, true);
}

static void toplevel_request_fullscreen_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, request_fullscreen);
	wlr_xdg_toplevel_set_fullscreen(t->xdg_toplevel, true);
}

static void toplevel_request_minimize_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, request_minimize);
	/* "Minimise" on a phone means go home: the shell redraws over us. */
	astrix_log(WLR_INFO, "app requested minimise (treating as home)");
	set_focused_toplevel(t->server, NULL);
}

static void toplevel_request_move_handler(struct wl_listener *listener, void *data) {
	/* No window moving on a phone. Answering with a configure is still
	 * required by xdg-shell, so schedule one at the current size. */
	struct astrix_toplevel *t = wl_container_of(listener, t, request_move);
	wlr_xdg_surface_schedule_configure(t->xdg_toplevel->base);
}

static void toplevel_request_resize_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, request_resize);
	/* Resizing is not supported; confirm fullscreen instead. */
	wlr_xdg_toplevel_set_fullscreen(t->xdg_toplevel, true);
	wlr_xdg_surface_schedule_configure(t->xdg_toplevel->base);
}

static void toplevel_set_title_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, set_title);
	astrix_log(WLR_DEBUG, "app title: %s",
	           t->xdg_toplevel->title ? t->xdg_toplevel->title : "(none)");
	astrix_server_invalidate(t->server);
}

static void toplevel_set_app_id_handler(struct wl_listener *listener, void *data) {
	struct astrix_toplevel *t = wl_container_of(listener, t, set_app_id);
	astrix_log(WLR_DEBUG, "app id: %s",
	           t->xdg_toplevel->app_id ? t->xdg_toplevel->app_id : "(none)");
	astrix_server_invalidate(t->server);
}

/* --- construction -------------------------------------------------------- */

static void handle_new_toplevel(struct wl_listener *listener, void *data) {
	struct astrix_server *server = wl_container_of(listener, server, new_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct astrix_toplevel *t = calloc(1, sizeof(*t));
	if (!t) {
		astrix_log(WLR_ERROR, "out of memory allocating toplevel");
		return;
	}
	t->server = server;
	t->xdg_toplevel = xdg_toplevel;
	wl_list_insert(&server->toplevels, &t->link);

	struct wlr_surface *surface = xdg_toplevel->base->surface;

	/* wlroots 0.18: map/unmap/commit are surface events. */
	t->map.notify = toplevel_map_handler;
	wl_signal_add(&surface->events.map, &t->map);
	t->unmap.notify = toplevel_unmap_handler;
	wl_signal_add(&surface->events.unmap, &t->unmap);
	t->commit.notify = toplevel_commit_handler;
	wl_signal_add(&surface->events.commit, &t->commit);
	t->surface_destroy.notify = toplevel_surface_destroy_handler;
	wl_signal_add(&surface->events.destroy, &t->surface_destroy);

	/* Role-level requests are toplevel events. */
	t->request_maximize.notify = toplevel_request_maximize_handler;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &t->request_maximize);
	t->request_fullscreen.notify = toplevel_request_fullscreen_handler;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &t->request_fullscreen);
	t->request_minimize.notify = toplevel_request_minimize_handler;
	wl_signal_add(&xdg_toplevel->events.request_minimize, &t->request_minimize);
	t->request_move.notify = toplevel_request_move_handler;
	wl_signal_add(&xdg_toplevel->events.request_move, &t->request_move);
	t->request_resize.notify = toplevel_request_resize_handler;
	wl_signal_add(&xdg_toplevel->events.request_resize, &t->request_resize);
	t->set_title.notify = toplevel_set_title_handler;
	wl_signal_add(&xdg_toplevel->events.set_title, &t->set_title);
	t->set_app_id.notify = toplevel_set_app_id_handler;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &t->set_app_id);
	t->toplevel_destroy.notify = toplevel_role_destroy_handler;
	wl_signal_add(&xdg_toplevel->events.destroy, &t->toplevel_destroy);

	/*
	 * Add the surface to the scene graph now, not on map.
	 *
	 * The ordering here is the subtle part. In wlroots a wlr_surface only
	 * becomes `mapped` once it is in a scene tree and has committed a
	 * buffer. Creating the node inside the map handler would therefore
	 * never run: the map event would never fire in the first place. So the
	 * node is created as soon as the role object exists, and the map event
	 * then tells us the first real frame is on its way.
	 *
	 * wlr_scene_xdg_surface_create is used rather than the lower-level
	 * surface helper because it also tracks the surface's subsurfaces and
	 * its xdg_popup children, which is most of what a toolkit draws.
	 */
	if (server->scene_root) {
		t->scene_tree = wlr_scene_xdg_surface_create(server->scene_root, xdg_toplevel->base);
		if (!t->scene_tree) {
			astrix_log(WLR_ERROR, "failed to add a new surface to the scene graph");
		}
	}

	astrix_log(WLR_INFO, "new app registered (app_id=%s)",
	           xdg_toplevel->app_id ? xdg_toplevel->app_id : "unset");
	astrix_server_invalidate(server);
}

/* --- lifecycle ----------------------------------------------------------- */

void astrix_shell_init(struct astrix_server *server) {
	wl_list_init(&server->toplevels);
	server->new_toplevel.notify = handle_new_toplevel;
	/*
	 * In 0.18 the signal is `new_toplevel` on xdg_shell->events; the
	 * `new_surface` signal fires first and only carries the role-less
	 * xdg_surface.
	 */
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_toplevel);
}

void astrix_shell_finish(struct astrix_server *server) {
	/*
	 * Tear every toplevel down through the normal path, unregistering its
	 * listeners as it goes. The clients are destroyed *after* this (see
	 * main.c), and destroying them fires surface/role destroy again - so
	 * freeing the records here without unregistering first would leave the
	 * clients' signal lists pointing at freed memory during shutdown.
	 * toplevel_finish is idempotent, so the second pass is a no-op.
	 */
	struct astrix_toplevel *t, *tmp;
	wl_list_for_each_safe(t, tmp, &server->toplevels, link) {
		toplevel_finish(t);
	}
}
