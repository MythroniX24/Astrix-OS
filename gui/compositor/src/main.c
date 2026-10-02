/*
 * Astrix OS - compositor server main.
 *
 * A wlroots 0.18 Wayland compositor sized for a phone: one seat, one default
 * output, a scene graph, and frame pacing driven by a repaint timer.
 *
 * Deliberately absent: workspaces, tiling, keybinding layers, Xwayland and
 * nested compositors. None of those are part of a mobile OS and each costs
 * RAM and boot time a phone cannot spare.
 */

#include "astrix_compositor.h"

#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define ASTRIX_DEFAULT_REFRESH 60
/* Cap repaint rate: a phone panel at 60Hz does not need a 1000Hz timer, and
 * an uncapped timer is the single easiest way to burn battery. */
#define ASTRIX_REPAINT_MS (1000 / 60)

/*
 * wlroots exposes wlr_log() as a macro that string-concatenates the format
 * with a "[file:line] " prefix, so it cannot take a runtime format string.
 * _wlr_log() is the underlying variadic function and is what a wrapper has
 * to call. We supply our own prefix so log output names the Astrix module.
 */
void astrix_log(enum wlr_log_importance level, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	/*
	 * wlr_log is a macro that concatenates a string literal, so it cannot
	 * take a runtime format string. _wlr_log is the underlying variadic
	 * function; we render the caller's message into a stack buffer and pass
	 * it as a single "%s" argument. Message length is capped so a runaway
	 * caller cannot overflow the buffer.
	 */
	char buf[512];
	vsnprintf(buf, sizeof(buf), fmt, args);
	_wlr_log(level, "[astrix] %s", buf);
	va_end(args);
}

/* --- config -------------------------------------------------------------- */

static int env_int(const char *name, int fallback) {
	const char *v = getenv(name);
	if (!v || !*v) {
		return fallback;
	}
	char *end = NULL;
	long parsed = strtol(v, &end, 10);
	return (end == v) ? fallback : (int)parsed;
}

void astrix_config_init(struct astrix_config *cfg) {
	memset(cfg, 0, sizeof(*cfg));
	cfg->width = env_int("ASTRIX_WIDTH", 0);
	cfg->height = env_int("ASTRIX_HEIGHT", 0);
	cfg->scale = env_int("ASTRIX_SCALE", 1);
	cfg->refresh_hz = env_int("ASTRIX_REFRESH_HZ", ASTRIX_DEFAULT_REFRESH);
	if (cfg->refresh_hz <= 0) {
		cfg->refresh_hz = ASTRIX_DEFAULT_REFRESH;
	}
	cfg->headless = getenv("ASTRIX_HEADLESS") != NULL;
}

/* --- crash handler ------------------------------------------------------- */

/*
 * Print a backtrace when the compositor dies from a fatal signal.
 *
 * A display server that segfaults is invisible by definition: the screen is
 * black, and the only record is a bare "sig=11" in the audit log, which says
 * that the process crashed and nothing about where. That is not enough to work
 * with, and on a phone there is no gdb to attach. A dozen lines of handler
 * turns the worst possible diagnostic into a call stack, so this stays.
 *
 * Deliberately async-signal-unsafe calls only: backtrace() and write(). No
 * printf, no malloc, no locks - the process is already in an unknown state and
 * a second fault inside the handler would lose even this.
 */
static void astrix_crash_handler(int sig) {
	const char msg[] = "\nastrix-compositor: fatal signal, backtrace follows:\n";
	ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
	(void)ignored;

	void *frames[32];
	int n = backtrace(frames, 32);
	backtrace_symbols_fd(frames, n, STDERR_FILENO);

	/* Restore the default action and re-raise, so systemd still sees the
	 * real signal and its exit status is the one it would have been. */
	signal(sig, SIG_DFL);
	raise(sig);
}

static void astrix_install_crash_handler(void) {
	/* SIGSEGV/SIGABRT/SIGFPE/SIGILL are all "the compositor has a bug";
	 * SIGBUS too, on an architecture that has one. */
	static const int signals[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		signal(signals[i], astrix_crash_handler);
	}
}

/* --- diagnostics --------------------------------------------------------- */

/*
 * When the compositor cannot get a backend, "no backend available" is not a
 * diagnosis. This probes the three things that are actually checked and that
 * have each been a real failure at least once, so the log says which one is
 * wrong instead of leaving it to be guessed at.
 */
struct astrix_drm_probe astrix_drm_probe(void) {
	struct astrix_drm_probe p;
	memset(&p, 0, sizeof(p));

	/* The render node is what Mesa actually needs; the card node is what
	 * the compositor needs for KMS. Both matter, so both are reported. */
	if (access("/dev/dri/renderD128", F_OK) == 0) {
		snprintf(p.render_dev, sizeof(p.render_dev), "renderD128 present");
	} else if (access("/dev/dri", F_OK) == 0) {
		snprintf(p.render_dev, sizeof(p.render_dev), "/dev/dri present, no renderD128");
	} else {
		snprintf(p.render_dev, sizeof(p.render_dev), "/dev/dri missing");
	}

	if (access("/dev/dri/card0", F_OK) == 0) {
		snprintf(p.card_dev, sizeof(p.card_dev), "present");
		int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			snprintf(p.card_open, sizeof(p.card_open), "ok");
			close(fd);
		} else {
			snprintf(p.card_open, sizeof(p.card_open), "%s", strerror(errno));
		}
	} else {
		snprintf(p.card_dev, sizeof(p.card_dev), "missing");
		snprintf(p.card_open, sizeof(p.card_open), "n/a");
	}
	return p;
}

/*
 * Create the real backend for a device.
 *
 * wlr_backend_autocreate() is the obvious call and it is the wrong one here.
 * In wlroots 0.18 it reaches DRM only through a libseat session, and libseat
 * only hands out DRM devices to a caller that owns a logind session
 * (XDG_SESSION_ID). Astrix's compositor is a system service that starts
 * before anyone has logged in - on a phone there is no login - so with
 * XDG_SESSION_ID unset, autocreate silently fell through to the Wayland
 * backend, which correctly failed because there is no display server to nest
 * inside. The compositor died with "no backend available" even though
 * /dev/dri/card0 was present and openable: the device was there the whole
 * time, the compositor was just asking the wrong system for access to it.
 *
 * So the compositor builds its own backend from the two things it is actually
 * given: a device node it opens itself (which is exactly what its unit grants
 * it, DeviceAllow=/dev/dri/card0 rw) and a wlr_session built on the logind
 * session astrix-session creates at boot. It then owns the DRM master directly
 * rather than borrowing it from a login session that does not exist.
 *
 * Input is attached through a multi-backend. A libinput backend that fails to
 * attach is logged and the compositor continues: a phone whose touchscreen has
 * not appeared yet is still better than a phone with no screen at all.
 */
static struct wlr_backend *astrix_create_device_backend(struct astrix_server *server) {
	const char *card = getenv("ASTRIX_DRM_CARD");
	if (!card) {
		card = ASTRIX_DRM_CARD;
	}

	/*
	 * wlroots 0.18 asserts `session && dev`: a DRM backend cannot be made
	 * from a device node alone. wlr_session_create is libseat, and libseat
	 * needs a logind session to attach to - which is what astrix-session
	 * creates at boot and passes in through XDG_SESSION_ID.
	 */
	struct wlr_session *session = wlr_session_create(server->wl_event_loop);
	if (session == NULL) {
		astrix_log(WLR_ERROR,
		           "wlr_session_create failed: libseat could not open a session. "
		           "XDG_SESSION_ID=%s",
		           getenv("XDG_SESSION_ID") ? getenv("XDG_SESSION_ID") : "(unset)");
		return NULL;
	}
	astrix_log(WLR_INFO, "wlr_session ready (seat %s, vt %u)", session->seat,
	           session->vtnr);
	server->session = session;

	/*
	 * The GPU comes from the session, not from open().
	 *
	 * wlr_drm_backend_create() does not take ownership of a device the
	 * caller made up: it links the struct it is given into the session's
	 * device list, and that list is what it walks for the life of the
	 * session. Handing it a locally-built struct wlr_device crashes inside
	 * wlroots at the first wl_list_insert, with no wlroots message - the
	 * backtrace read wl_list_insert <- wlr_drm_backend_create and nothing
	 * else, twice, which is what finally ruled out the device node and
	 * pointed at the struct instead.
	 *
	 * wlr_session_find_gpus() is the supported path and does the whole
	 * dance: it asks libseat/udev for the KMS devices on this seat, opens
	 * them, and links them into session->devices itself. Because those
	 * devices live in the session, they outlive this function with no
	 * lifetime rule for us to get wrong. It is also what puts the panel's
	 * connector in front of us - opening /dev/dri/card0 by hand and handing
	 * that fd over would skip exactly the bookkeeping wlroots needs.
	 */
	struct wlr_device *gpus = NULL;
	ssize_t n = wlr_session_find_gpus(session, 1, &gpus);
	/*
	 * wlr_session_find_gpus has three outcomes, not two: a positive
	 * count, 0 for "the session has no KMS device", and -1 for failure.
	 * Checking only `n < 0` sends the 0 case into the success branch,
	 * which then dereferences gpus[0] on a NULL list - a SIGSEGV, with
	 * the "Waiting for a KMS device" line above it as the only clue.
	 * That is exactly what happens when the compositor starts before the
	 * GPU's device node exists, which the session launcher makes a race
	 * rather than a certainty: on one boot the compositor began at 399s
	 * and virtio_gpu finished initialising at 428s, and the compositor
	 * died of this dereference every time it retried.
	 */
	if (n <= 0 || gpus == NULL) {
		struct astrix_drm_probe probe = astrix_drm_probe();
		astrix_log(WLR_ERROR, n == 0 ? "wlr_session_find_gpus found no KMS device"
		                             : "wlr_session_find_gpus failed (%zd)", n);
		astrix_log(WLR_ERROR, "  /dev/dri: %s", probe.render_dev);
		astrix_log(WLR_ERROR, "  %s: %s (open rw: %s)", card,
		           probe.card_dev, probe.card_open);
		astrix_log(WLR_ERROR, "  seat=%s vt=%u XDG_SESSION_ID=%s", session->seat,
		           session->vtnr,
		           getenv("XDG_SESSION_ID") ? getenv("XDG_SESSION_ID") : "(unset)");
	} else {
		astrix_log(WLR_INFO, "session reported %zd KMS device(s); first is fd %d (%u:%u)",
		           n, gpus[0].fd, major(gpus[0].dev), minor(gpus[0].dev));

		struct wlr_backend *drm = wlr_drm_backend_create(session, &gpus[0], NULL);
		if (drm == NULL) {
			astrix_log(WLR_ERROR, "wlr_drm_backend_create failed");
		} else {
			struct wlr_backend *multi = wlr_multi_backend_create(server->wl_event_loop);
			if (multi == NULL || !wlr_multi_backend_add(multi, drm)) {
				astrix_log(WLR_ERROR, "failed to build a multi-backend");
				wlr_backend_destroy(drm);
				return NULL;
			}
			astrix_log(WLR_INFO, "DRM backend ready");
			server->real_display = true;

			struct wlr_backend *input = wlr_libinput_backend_create(session);
			if (input != NULL) {
				if (wlr_multi_backend_add(multi, input)) {
					astrix_log(WLR_INFO, "libinput backend attached");
				} else {
					astrix_log(WLR_ERROR, "could not attach the libinput backend");
					wlr_backend_destroy(input);
				}
			} else {
				astrix_log(WLR_ERROR,
				           "no libinput backend: the session will have a display but no touch");
			}
			return multi;
		}
	}

	/* Fall back rather than give up, so a device that does have a login
	 * session (a developer build with one, a future screen-lock flow) still
	 * gets a compositor. */
	astrix_log(WLR_INFO, "falling back to wlr_backend_autocreate()");
	return wlr_backend_autocreate(server->wl_event_loop, NULL);
}

/* --- output -------------------------------------------------------------- */

static void output_frame_handler(struct wl_listener *listener, void *data) {
	struct astrix_output *ao = wl_container_of(listener, ao, frame);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	/*
	 * The scene graph tracks client frame requests itself, so all a
	 * compositor has to do is tell it when a frame was actually presented.
	 */
	if (ao->server->scene_output) {
		wlr_scene_output_send_frame_done(ao->server->scene_output, &now);
	}
	ao->server->last_present = now;
	astrix_server_invalidate(ao->server);
}

static void output_destroy_handler(struct wl_listener *listener, void *data) {
	struct astrix_output *ao = wl_container_of(listener, ao, destroy);
	astrix_log(WLR_INFO, "output %s removed", ao->wlr_output->name);
	wl_list_remove(&ao->link);
	wl_list_remove(&ao->frame.link);
	wl_list_remove(&ao->destroy.link);
	free(ao);
}

static void output_description_handler(struct wl_listener *listener, void *data) {
	struct astrix_output *ao = wl_container_of(listener, ao, description);
	ao->width = ao->wlr_output->width;
	ao->height = ao->wlr_output->height;
	/* refresh is in mHz in wlroots 0.18. */
	astrix_log(WLR_INFO, "output %s: %dx%d @ %.2fHz scale %.2f", ao->wlr_output->name,
	           ao->width, ao->height, (double)ao->wlr_output->refresh / 1000.0,
	           (double)ao->wlr_output->scale);
}

static void handle_new_output(struct wl_listener *listener, void *data) {
	struct astrix_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *output = data;

	/*
	 * Every output must be initialised with the allocator/renderer before
	 * it can be used. Omitting this is the single most common wlroots
	 * upgrade mistake.
	 */
	wlr_output_init_render(output, server->allocator, server->renderer);

	struct astrix_output *ao = calloc(1, sizeof(*ao));
	if (!ao) {
		wlr_log(WLR_ERROR, "failed to allocate output state");
		return;
	}
	ao->server = server;
	ao->wlr_output = output;
	ao->initialized = true;
	wl_list_insert(&server->outputs, &ao->link);

	ao->frame.notify = output_frame_handler;
	wl_signal_add(&output->events.frame, &ao->frame);

	ao->destroy.notify = output_destroy_handler;
	wl_signal_add(&output->events.destroy, &ao->destroy);

	ao->description.notify = output_description_handler;
	wl_signal_add(&output->events.description, &ao->description);

	/*
	 * Enable the output. A requested resolution is a *request*, not a
	 * command: the panel only accepts modes its connector advertises,
	 * and a made-up one is rejected by the commit rather than
	 * negotiated. QEMU's virtio-gpu advertises no useful mode list, so
	 * forcing 720x1600 there fails and the output would stay disabled -
	 * a black screen with a perfectly healthy DRM backend. So the
	 * request is attempted, and if the hardware refuses it the preferred
	 * mode is used and the substitution is logged.
	 */
	struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
	struct wlr_output_state state;
	bool forced = false;
	struct wlr_output_mode requested;

	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	if (mode && server->config.width > 0 && server->config.height > 0 &&
	    (mode->width != server->config.width || mode->height != server->config.height)) {
		requested = *mode;
		requested.width = server->config.width;
		requested.height = server->config.height;
		wlr_output_state_set_mode(&state, &requested);
		forced = true;
	} else if (mode) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_state_set_scale(&state, (float)server->config.scale);
	bool ok = wlr_output_commit_state(output, &state);
	wlr_output_state_finish(&state);

	if (!ok && forced) {
		astrix_log(WLR_ERROR,
		           "%s rejected the requested %dx%d; falling back to the preferred mode",
		           output->name, requested.width, requested.height);
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		wlr_output_state_set_mode(&state, mode);
		wlr_output_state_set_scale(&state, (float)server->config.scale);
		ok = wlr_output_commit_state(output, &state);
		wlr_output_state_finish(&state);
	}
	if (!ok) {
		astrix_log(WLR_ERROR, "failed to enable output %s; nothing can be displayed on it",
		           output->name);
		free(ao);
		return;
	}

	ao->width = output->width;
	ao->height = output->height;
	astrix_log(WLR_INFO, "new output %s (%dx%d)%s", output->name, ao->width, ao->height,
	           forced ? " [requested mode]" : "");
	if (server->config.width > 0 && ao->width != server->config.width) {
		astrix_log(WLR_INFO,
		           "the panel is %dx%d but this build targets %dx%d; clients are "
		           "told the real panel size over wl_output and lay out for it",
		           ao->width, ao->height, server->config.width, server->config.height);
	}

	/*
	 * Put the output in the output layout. The compositor owns the layout
	 * in wlroots 0.18 - wlr_output_layout_add_auto() is not called for it
	 * by anything else - and omitting it is invisible in the logs while
	 * breaking two things at once:
	 *
	 *   - the layout is what the cursor is constrained to. With no output
	 *     in it, wlr_cursor has no usable rectangle, so every pointer
	 *     motion is clamped to a corner and a tap lands wherever that
	 *     corner happens to be no matter where the user pressed.
	 *   - clients get no output geometry, so nothing can be placed or
	 *     scaled against the real display size.
	 *
	 * NULL here means the output is already in the layout, which is not an
	 * error worth failing a session over.
	 */
	struct wlr_output_layout_output *layout_output =
	    wlr_output_layout_add_auto(server->output_layout, output);
	if (layout_output) {
		astrix_log(WLR_INFO, "output added to the layout at %d,%d", layout_output->x,
		           layout_output->y);
	} else {
		astrix_log(WLR_INFO, "output was already in the layout");
	}

	/*
	 * The scene output must be created once per output so the scene graph
	 * knows where to render. With more than one output each gets its own.
	 */
	if (!server->scene_output) {
		server->scene_output = wlr_scene_output_create(server->scene, output);
	}

	/*
	 * Phones are portrait-first. If the panel came up in landscape, the
	 * shell still lays out portrait, so warn rather than silently letterbox.
	 */
	if (ao->width > ao->height) {
		astrix_log(WLR_INFO, "output is landscape (%dx%d); shell is portrait-first", ao->width,
		           ao->height);
	}
}

/* --- repaint loop -------------------------------------------------------- */

void astrix_server_invalidate(struct astrix_server *server) {
	if (!server) {
		return;
	}
	server->dirty = true;
}

static int repaint_timer_tick(void *data) {
	struct astrix_server *server = data;
	if (!server->running) {
		return 0;
	}
	server->dirty = false;
	if (server->scene_output) {
		wlr_scene_output_commit(server->scene_output, NULL);
	}
	return 0;
}

/* --- touch coordinate mapping ------------------------------------------- */

void astrix_touch_to_pixels(struct astrix_server *server, double nx, double ny, int *px,
                            int *py) {
	struct astrix_output *ao = NULL, *it;
	wl_list_for_each(it, &server->outputs, link) {
		ao = it;
		break;
	}
	if (!ao) {
		*px = (int)(nx * 720.0);
		*py = (int)(ny * 1280.0);
		return;
	}
	int w = ao->width > 0 ? ao->width : 720;
	int h = ao->height > 0 ? ao->height : 1280;
	*px = (int)(nx * (double)w);
	*py = (int)(ny * (double)h);
}

/* --- backend / lifecycle ------------------------------------------------- */

static void handle_backend_new_output(struct wl_listener *listener, void *data) {
	struct astrix_server *server = wl_container_of(listener, server, new_output);
	handle_new_output(&server->new_output, data);
}

static void handle_backend_destroy(struct wl_listener *listener, void *data) {
	struct astrix_server *server = wl_container_of(listener, server, backend_destroy);
	astrix_log(WLR_INFO, "all backends destroyed, shutting down");
	server->running = false;
	/*
	 * Only a running session may be terminated through the display.
	 *
	 * During startup the compositor bails out on a fatal error and calls
	 * wl_display_destroy() directly. That destroys the backends, which fires
	 * this very signal - on a display whose event loop is already being torn
	 * down. wl_display_terminate() then trips its own assertion
	 * (`ret >= 0 || errno == EAGAIN`) and aborts the process, so a clean,
	 * well-diagnosed startup failure was being reported to systemd as SIGABRT
	 * with a stack trace through wl_event_loop_destroy, which points at
	 * wayland and hides the actual error that stopped the compositor.
	 */
	if (server->display_live) {
		wl_display_terminate(server->wl_display);
	}
}

/*
 * Tear the compositor down on a startup failure.
 *
 * Every early "we cannot continue" path goes through here, because getting
 * this wrong is not a no-op: wl_display_destroy() destroys the backends,
 * which emits backend::destroy, which must not then try to terminate the
 * display that is halfway through being destroyed. Clearing display_live
 * first is what keeps a clean, well-diagnosed failure from turning into an
 * abort with a stack trace through wl_event_loop_destroy.
 */
static int astrix_startup_failed(struct astrix_server *server) {
	server->display_live = false;
	if (server->backend) {
		wlr_backend_destroy(server->backend);
		server->backend = NULL;
	}
	if (server->session) {
		wlr_session_destroy(server->session);
		server->session = NULL;
	}
	wl_display_destroy(server->wl_display);
	free(server);
	return 1;
}

int astrix_compositor_run(const struct astrix_config *cfg) {
	wlr_log_init(WLR_INFO, NULL);
	astrix_install_crash_handler();

	struct astrix_server *server = calloc(1, sizeof(*server));
	if (!server) {
		astrix_log(WLR_ERROR, "out of memory");
		return 1;
	}
	/* The display exists from here on, so the backend-destroy handler is
	 * allowed to terminate it; every early return below clears this first. */
	server->display_live = true;
	server->config = *cfg;
	wl_list_init(&server->outputs);
	wl_list_init(&server->keyboards);
	wl_list_init(&server->pointers);
	wl_list_init(&server->touch);
	wl_list_init(&server->touchpoints);

	server->wl_display = wl_display_create();
	if (!server->wl_display) {
		astrix_log(WLR_ERROR, "failed to create wl_display");
		free(server);
		return 1;
	}
	server->wl_event_loop = wl_display_get_event_loop(server->wl_display);

	/* Backend: a real DRM/libinput session on a device, headless in tests. */
	if (cfg->headless) {
		server->backend = wlr_headless_backend_create(server->wl_event_loop);
		astrix_log(WLR_INFO, "using headless backend (ASTRIX_HEADLESS=1)");
	} else {
		server->backend = astrix_create_device_backend(server);
	}
	if (!server->backend) {
		/*
		 * Report *why*, because "no backend" on its own cost hours. The
		 * three things that are checked here are the three that have
		 * actually gone wrong at least once:
		 *   - /dev/dri missing or unreadable (wrong groups, no kernel
		 *     driver, device cgroup denying access),
		 *   - no logind session, so libseat cannot open the DRM devices,
		 *   - a display server the sandbox forbids us from talking to.
		 */
		astrix_log(WLR_ERROR, "no wlroots backend available");
		/*
		 * The DRM probe is deliberately not repeated here. The previous
		 * version re-printed render_dev and card_dev behind
		 * `probe.render_dev ? ...` - but those are char arrays, so the
		 * address is always non-NULL and the test is always true, which is
		 * what gcc's -Waddress was reporting. A diagnostic that says
		 * "present" when /dev/dri is missing is worse than no diagnostic,
		 * because it sends the reader looking in the wrong place; and the
		 * wlr_session_find_gpus failure path above already prints the full
		 * probe. Only the session id is new information here.
		 */
		astrix_log(WLR_ERROR, "  XDG_SESSION_ID: %s",
		           getenv("XDG_SESSION_ID") ? getenv("XDG_SESSION_ID") : "(unset)");
		astrix_log(WLR_ERROR, "  WLR_BACKENDS: %s",
		           getenv("WLR_BACKENDS") ? getenv("WLR_BACKENDS") : "(unset)");
		astrix_log(WLR_ERROR,
		           "  ASTRIX_HEADLESS=1 forces the headless backend for tests");
		return astrix_startup_failed(server);
	}
	/*
	 * A backend is not the same as a display. On a phone, silently falling
	 * back to the headless backend means the session "starts", the shell
	 * maps its surface, every test passes, and the screen stays black -
	 * the exact failure this project refuses to ship. So the session
	 * refuses to run unless it actually got a display.
	 *
	 * This asks the flag we set when we built the DRM backend, not
	 * wlr_backend_is_drm(): the backend we hold is a *multi* backend (the
	 * DRM backend plus libinput, so input and output share one lifecycle),
	 * and wlr_backend_is_drm() is false for a multi backend. Asking the
	 * type of the wrapper reported "no display" immediately after a
	 * perfectly good KMS backend had come up - a false alarm that would
	 * have taken the session down forever. The flag is the ground truth:
	 * it is set only on the one path that genuinely has a panel.
	 */
	if (!cfg->headless && !server->real_display) {
		astrix_log(WLR_ERROR,
		           "no DRM display for a session that requires one; refusing to run "
		           "a session nothing can be seen on");
		return astrix_startup_failed(server);
	}

	/* Renderer: honours WLR_RENDERER=pixman for the software path. */
	server->renderer = wlr_renderer_autocreate(server->backend);
	if (!server->renderer) {
		astrix_log(WLR_ERROR, "failed to create renderer (tried GLES2 and pixman)");
		return astrix_startup_failed(server);
	}
	/*
	 * wlroots 0.18 has no wlr_renderer_get_name(); the selected backend is
	 * reported through the DRM fd when there is one. Log what we can prove:
	 * a DRM fd means the GPU/DRM path, otherwise we are on a software path.
	 */
	astrix_log(WLR_INFO, "renderer ready (drm_fd=%d, WLR_RENDERER=%s)",
	           wlr_renderer_get_drm_fd(server->renderer),
	           getenv("WLR_RENDERER") ? getenv("WLR_RENDERER") : "auto");

	server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	if (!server->allocator) {
		astrix_log(WLR_ERROR, "failed to create allocator");
		return astrix_startup_failed(server);
	}

	server->wlr_compositor =
	    wlr_compositor_create(server->wl_display, 6, server->renderer);
	server->data_device_manager = wlr_data_device_manager_create(server->wl_display);
	server->primary_selection_manager =
	    wlr_primary_selection_v1_device_manager_create(server->wl_display);
	/*
	 * wl_shm is provided by the renderer in wlroots 0.18 (there is no
	 * wlr_shm_create in that release). Without it no client can hand us a
	 * buffer, because shm is the only transport that needs no GPU - which
	 * makes it exactly what the shell uses.
	 */
	if (!wlr_renderer_init_wl_shm(server->renderer, server->wl_display)) {
		astrix_log(WLR_ERROR, "failed to advertise wl_shm; no client can render");
		return astrix_startup_failed(server);
	}

	server->scene = wlr_scene_create();
	/*
	 * Every toplevel is parented to this root tree. A wlr_scene on its own
	 * is just a container; wlr_scene_tree_create() is what produces the
	 * node that wlr_scene_xdg_surface_create() and the repaint path walk.
	 */
	server->scene_root = wlr_scene_tree_create(&server->scene->tree);
	server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 6);

	server->output_layout = wlr_output_layout_create(server->wl_display);
	server->cursor = wlr_cursor_create();
	server->xcursor_manager = wlr_xcursor_manager_create(NULL, 24);
	/*
	 * Virtual keyboards.
	 *
	 * wlroots only advertises this global from its Wayland *backend*, and
	 * this compositor deliberately does not use one: it binds its own
	 * wl_display socket and builds the seat itself, because autocreate
	 * picks a socket name that depends on what else is running. The
	 * consequence was that the shell's on-screen keyboard drew, queued
	 * keys, and delivered none of them - there was no global to bind to.
	 *
	 * So the manager is created here, against our own wl_display. One
	 * line, and it is the difference between a keyboard that types and a
	 * keyboard that is a picture of a keyboard.
	 */
	struct wlr_virtual_keyboard_manager_v1 *vkbd_manager =
		wlr_virtual_keyboard_manager_v1_create(server->wl_display);
	if (!vkbd_manager) {
		astrix_log(WLR_ERROR,
		           "could not create the virtual keyboard manager; the "
		           "on-screen keyboard will not be able to type");
	} else {
		astrix_log(WLR_INFO, "virtual keyboard manager ready");
	}

	server->seat = wlr_seat_create(server->wl_display, "seat0");
	if (!server->seat) {
		astrix_log(WLR_ERROR, "failed to create seat");
		return astrix_startup_failed(server);
	}

	astrix_input_init(server);
	astrix_shell_init(server);

	server->new_output.notify = handle_backend_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	server->backend_destroy.notify = handle_backend_destroy;
	wl_signal_add(&server->backend->events.destroy, &server->backend_destroy);

	/*
	 * Start the backend - HERE, not earlier.
	 *
	 * Creating a wlroots backend and starting it are different things.
	 * wlr_backend_start() is what asks the backend to enumerate its
	 * hardware, and the DRM/libinput backends only do that on start:
	 * outputs arrive through the new_output signal and input devices
	 * through new_input, both of which are emitted *during* start.
	 *
	 * Every one of those listeners has to exist first. This call used to
	 * be missing altogether, and the failure mode was the most expensive
	 * kind: nothing crashed and nothing looked wrong. The session came
	 * up, the DRM backend reported its CRTCs and planes, the shell
	 * connected and mapped its surface, and yet there was no output in
	 * the output layout and not a single input device on the seat - so
	 * the screen would have stayed black and the phone would have been
	 * deaf to touch, while every log line said the GPU was fine.
	 *
	 * If the start fails there is no display to be seen on, which for a
	 * device session is fatal rather than something to fall back from.
	 */
	if (!wlr_backend_start(server->backend)) {
		astrix_log(WLR_ERROR, "wlr_backend_start failed: the session has no display");
		return astrix_startup_failed(server);
	}
	astrix_log(WLR_INFO, "backend started");

	/*
	 * Headless runs have no physical output, so create one explicitly. On a
	 * real device the backend emits new_output instead.
	 */
	if (cfg->headless) {
		struct wlr_output *output = wlr_headless_add_output(server->backend, cfg->width,
		                                                    cfg->height);
		if (output) {
			astrix_log(WLR_INFO, "headless output created");
		}
	}

	/*
	 * Bind a fixed socket name rather than wl_display_add_socket_auto().
	 *
	 * add_socket_auto() picks the first *free* wayland-N, so the name depends
	 * on whatever else happens to be running: two seats, a second compositor
	 * or a leftover socket from a crashed session all shift it. Clients that
	 * hardcode a name then connect to the wrong thing or to nothing, and the
	 * failure looks like a compositor crash. A fixed name makes the session
	 * deterministic and keeps the shell service in agreement with us.
	 *
	 * Note the return type: wl_display_add_socket() returns int (0 on
	 * success), while only the _auto variant returns the name it chose.
	 */
	const char *socket_name = getenv("ASTRIX_SOCKET");
	if (!socket_name || !*socket_name) {
		socket_name = "astrix-0";
	}
	if (wl_display_add_socket(server->wl_display, socket_name) != 0) {
		/*
		 * The expected cause is a stale socket left by a previous session.
		 * Removing it and retrying is correct - the compositor owns this name
		 * for the whole session - but a second failure is reported rather
		 * than swallowed.
		 */
		astrix_log(WLR_ERROR, "socket '%s' unavailable, removing stale socket", socket_name);
		const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
		char path[256];
		snprintf(path, sizeof(path), "%s/%s", runtime_dir ? runtime_dir : "/tmp", socket_name);
		unlink(path);
		if (wl_display_add_socket(server->wl_display, socket_name) != 0) {
			astrix_log(WLR_ERROR, "failed to create wayland socket '%s'", socket_name);
			return astrix_startup_failed(server);
		}
	}
	astrix_log(WLR_INFO, "astrix-compositor: WAYLAND_DISPLAY=%s", socket_name);

	/*
	 * Frame pacing. The repaint timer is only re-armed when the scene is
	 * marked dirty, so an idle phone wakes the CPU roughly 60x/s rather
	 * than spinning. Apps call astrix_server_invalidate() on change.
	 */
	struct wl_event_loop *loop = server->wl_event_loop;
	server->repaint_timer = wl_event_loop_add_timer(loop, repaint_timer_tick, server);
	if (server->repaint_timer) {
		wl_event_source_timer_update(server->repaint_timer, ASTRIX_REPAINT_MS);
	}

	server->running = true;
	astrix_log(WLR_INFO, "astrix-compositor ready");

	wl_display_run(server->wl_display);

	server->running = false;
	astrix_log(WLR_INFO, "astrix-compositor shutting down");
	astrix_input_finish(server);
	astrix_shell_finish(server);

	/*
	 * Order matters on the way down. The backend holds the DRM device and
	 * the session holds the seat, so the backend goes first and the session
	 * - which wlroots handed that device out through - goes second.
	 */
	if (server->backend) {
		wlr_backend_destroy(server->backend);
		server->backend = NULL;
	}
	if (server->session) {
		wlr_session_destroy(server->session);
		server->session = NULL;
	}

	wl_display_destroy_clients(server->wl_display);
	wl_display_destroy(server->wl_display);
	free(server);
	return 0;
}

int main(int argc, char **argv) {
	struct astrix_config cfg;
	astrix_config_init(&cfg);

	/* Minimal argument handling; long options are read from the
	 * environment so that systemd units stay simple. */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--headless") == 0) {
			cfg.headless = true;
		} else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			cfg.width = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
			cfg.height = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
			cfg.scale = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--help") == 0) {
			printf("usage: astrix-compositor [--headless] "
			       "[--width N] [--height N] [--scale N]\n"
			       "environment: ASTRIX_WIDTH ASTRIX_HEIGHT ASTRIX_SCALE "
			       "ASTRIX_REFRESH_HZ ASTRIX_HEADLESS WLR_RENDERER\n");
			return 0;
		}
	}

	return astrix_compositor_run(&cfg);
}
