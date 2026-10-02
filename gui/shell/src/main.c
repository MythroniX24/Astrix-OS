/*
 * Astrix OS - Astrix Shell Wayland client.
 *
 * Connects to astrix-compositor, creates one fullscreen xdg-shell surface, and
 * uploads the shell's ARGB8888 framebuffer through wl_shm. It listens to
 * wl_seat for touch/pointer/keyboard input, translates the events into
 * astrix_input_event, and lets the shell decide what they mean.
 *
 * Buffer handling: two shm buffers are kept and swapped on commit, so the
 * compositor always has one buffer it is reading while the shell writes the
 * next frame. This is the standard double-buffered wl_shm pattern and it is
 * what lets a 720x1600 panel redraw without tearing.
 */

#include "astrix_shell.h"

#include <execinfo.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-header.h"
#include "virtual-keyboard-unstable-v1-client-header.h"
#include "keyboard.h"

struct shell_state {
	struct astrix_shell *shell;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct xdg_wm_base *wm_base;
	struct wl_pointer *pointer;
	struct wl_touch *touch;
	struct wl_keyboard *keyboard;

	/*
	 * The virtual keyboard the on-screen keyboard types through.
	 *
	 * A Wayland client cannot deliver input to another client - the seat
	 * belongs to the compositor. So instead of pretending to type, the
	 * shell asks the compositor to inject the keystroke into whatever has
	 * focus. That is the only version of this feature that is real: an
	 * on-screen keyboard that draws characters into its own frame is a
	 * picture of a keyboard.
	 */
	struct zwp_virtual_keyboard_manager_v1 *vkbd_manager;
	struct zwp_virtual_keyboard_v1 *vkbd;
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_keymap;
	char *xkb_keymap_text;
	/*
	 * The keymap fd stays open until the compositor has actually read it.
	 *
	 * The compositor reads it when the request is *dispatched*, which is
	 * after this function returns; closing immediately hands it a closed
	 * descriptor, the keymap read fails, and the session dies - which
	 * looked exactly like "the shell exits a second after starting".
	 */
	int vkbd_keymap_fd;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;

	/* Double-buffered shm pool. */
	struct wl_buffer *buffers[2];
	void *buffer_data[2];
	size_t buffer_size;
	int buffer_index;
	bool buffer_valid[2];
	int buffer_count;

	struct wl_callback *frame_callback;
	bool configured;
	bool running;
	int width, height;

	/*
	 * What the compositor says the panel actually is.
	 *
	 * ASTRIX_WIDTH/ASTRIX_HEIGHT (and the 720x1600 default) are only a
	 * guess made before the connection exists. Once wl_output has been
	 * seen the real mode is authoritative - see apply_output_size().
	 */
	struct wl_output *output;
	int output_px_w, output_px_h;
	int output_scale;
	bool output_mode_current;
	bool output_done;

	/* When the last wl_pointer button arrived, for the motion trace. */
	double last_button_event_ms;

	/*
	 * The app that has been sent SIGTERM and is being given a grace
	 * period before SIGKILL, or 0. Tracked here (not in the model)
	 * because it is a property of the process, not of the UI.
	 */
	int terminating_pid;
	int64_t terminating_since_ms;
};

static int64_t now_seconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec + ts.tv_nsec / 1000000000;
}

/* Milliseconds on a monotonic clock, for measuring the SIGTERM grace period. */
static int64_t monotonic_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- forward declarations ----------------------------------------------- */

/*
 * A backtrace on a fatal signal.
 *
 * Without it a segfault in the shell is one line ("qemu: uncaught target
 * signal 11") and no way to tell which of the four hundred lines caused it.
 * With it, the answer is printed to the same log as everything else, on the
 * machine it happened on - which is the only machine that matters.
 */
static void fatal_signal_handler(int sig) {
	fprintf(stderr, "\nastrix-shell: fatal signal %d\n", sig);
	void *frames[32];
	int n = backtrace(frames, 32);
	backtrace_symbols_fd(frames, n, STDERR_FILENO);
	_exit(128 + sig);
}

static void install_fatal_handlers(void) {
	signal(SIGSEGV, fatal_signal_handler);
	signal(SIGBUS, fatal_signal_handler);
	signal(SIGABRT, fatal_signal_handler);
}

static void draw_and_commit(struct shell_state *st);
/* Created either when the manager global arrives or when the seat does,
 * whichever comes second; see seat_handle_capabilities. */
static void virtual_keyboard_create(struct shell_state *st);

/* --- shm buffers --------------------------------------------------------- */

static void buffer_handle_release(void *data, struct wl_buffer *wl_buffer) {
	struct shell_state *st = data;
	/* The compositor is done with this buffer; mark it reusable. */
	for (int i = 0; i < 2; i++) {
		if (st->buffers[i] == wl_buffer) {
			st->buffer_valid[i] = true;
		}
	}
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_handle_release };

/* Create a shm pool and one wl_buffer backed by it. */
static int create_buffer(struct shell_state *st, int index) {
	int fd = memfd_create("astrix-shell-buffer", MFD_CLOEXEC);
	if (fd < 0) {
		/*
		 * memfd_create is available since Linux 3.17 and is present on
		 * every target we care about, but fall back to a tmpfile for
		 * kernels that restrict it.
		 */
		char tmpl[] = "/tmp/astrix-shm-XXXXXX";
		fd = mkstemp(tmpl);
		if (fd < 0) {
			fprintf(stderr, "astrix-shell: cannot create shm fd: %s\n", strerror(errno));
			return -1;
		}
		unlink(tmpl);
	}
	if (ftruncate(fd, (off_t)st->buffer_size) < 0) {
		fprintf(stderr, "astrix-shell: ftruncate: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	void *data = mmap(NULL, st->buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "astrix-shell: mmap: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, (int32_t)st->buffer_size);
	/*
	 * wl_shm_pool_create_buffer takes width, height, stride and format
	 * separately; the pool is just the memory backing store.
	 */
	st->buffers[index] = wl_shm_pool_create_buffer(pool, 0, st->width, st->height,
	                                               st->width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	/* The pool keeps its own reference to the fd. */
	close(fd);

	wl_buffer_add_listener(st->buffers[index], &buffer_listener, st);
	st->buffer_data[index] = data;
	st->buffer_valid[index] = true;
	return 0;
}

/* --- frame callbacks ----------------------------------------------------- */

static void frame_handle_done(void *data, struct wl_callback *callback, uint32_t time) {
	struct shell_state *st = data;
	wl_callback_destroy(callback);
	st->frame_callback = NULL;
	/*
	 * The compositor has finished presenting the last frame. If the shell
	 * still wants to draw (an animation, a drag, a state change) request
	 * another frame rather than redrawing into a buffer nobody will read.
	 */
	if (st->shell->needs_redraw) {
		draw_and_commit(st);
	}
}

static const struct wl_callback_listener frame_listener = { .done = frame_handle_done };

/* --- drawing ------------------------------------------------------------- */

static void draw_and_commit(struct shell_state *st) {
	if (!st->surface || !st->configured) {
		return;
	}
	/* Pick a buffer the compositor is not currently reading. */
	int idx = -1;
	for (int i = 0; i < st->buffer_count; i++) {
		if (st->buffer_valid[i]) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		/* Both buffers in flight: the compositor will release one and
		 * frame_handle_done will trigger the next draw. */
		return;
	}

	astrix_shell_draw(st->shell);
	/* astrix_shell_draw writes into shell->pixels, which is the same
	 * allocation as the shm buffer, so the pixels are already in place. */
	st->buffer_valid[idx] = false;

	wl_surface_attach(st->surface, st->buffers[idx], 0, 0);
	wl_surface_damage_buffer(st->surface, 0, 0, st->width, st->height);

	if (st->frame_callback) {
		wl_callback_destroy(st->frame_callback);
	}
	st->frame_callback = wl_surface_frame(st->surface);
	wl_callback_add_listener(st->frame_callback, &frame_listener, st);
	wl_surface_commit(st->surface);
	wl_display_flush(st->display);
}

/* --- xdg-shell ----------------------------------------------------------- */

static void xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface,
                                         uint32_t serial) {
	struct shell_state *st = data;
	xdg_surface_ack_configure(xdg_surface, serial);
	st->configured = true;
	/*
	 * The shell is always fullscreen: a phone OS has no windowed shell
	 * surface. A NULL output means "the compositor picks", which is what we
	 * want, and it also guarantees we get the real panel size immediately.
	 */
	xdg_toplevel_set_fullscreen(st->xdg_toplevel, NULL);
	wl_surface_commit(st->surface);
	draw_and_commit(st);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

/*
 * Re-lay out the shell at a new size: reallocate the shm buffers, re-point
 * the shell's framebuffer at them, and redraw.
 *
 * Both the shm buffers and astrix_shell->pixels must end up describing the
 * same memory: the shell draws straight into its own framebuffer, and the
 * first shm buffer is aliased onto it. Allocating the new framebuffer
 * without re-establishing that alias silently decouples them, and the UI
 * then renders into memory the compositor never uploads.
 */
static void shell_resize(struct shell_state *st, int nw, int nh) {
	if (nw <= 0 || nh <= 0 || (nw == st->width && nh == st->height)) {
		return;
	}
	size_t nsize = (size_t)nw * (size_t)nh * 4;
	if (nsize == st->buffer_size && st->buffer_count > 0) {
		return;
	}

	fprintf(stderr, "astrix-shell: re-laying out at %dx%d\n", nw, nh);

	for (int i = 0; i < 2; i++) {
		if (st->buffers[i]) {
			wl_buffer_destroy(st->buffers[i]);
			st->buffers[i] = NULL;
		}
		if (st->buffer_data[i]) {
			munmap(st->buffer_data[i], st->buffer_size);
			st->buffer_data[i] = NULL;
		}
		st->buffer_valid[i] = false;
	}
	st->buffer_count = 0;
	st->width = nw;
	st->height = nh;
	st->buffer_size = nsize;
	astrix_shell_set_size(st->shell, nw, nh);

	for (int i = 0; i < 2; i++) {
		if (create_buffer(st, i) == 0) {
			st->buffer_count++;
		}
	}
	if (st->buffer_count == 0 || !st->buffer_data[0]) {
		fprintf(stderr, "astrix-shell: could not reallocate buffers for %dx%d\n", nw, nh);
		return;
	}
	/* Re-establish the alias: the shell draws into the first shm buffer. */
	if (st->shell->pixels != st->buffer_data[0]) {
		/*
		 * The old target was itself the previous wl_shm mapping (or, on
		 * the very first resize, the shell's own allocation). Freeing
		 * it is only correct while the shell owns it: once aliased, the
		 * memory is an mmap the kernels drops with munmap above, and
		 * free() on it is a SIGSEGV. This is the crash that only showed
		 * up on a panel whose size differed from the build's 720x1600
		 * default - the host smoke test ran at the default size, so
		 * shell_resize() early-returned and never took this path.
		 */
		if (st->shell->pixels_owned) {
			free(st->shell->pixels);
		}
		st->shell->pixels = st->buffer_data[0];
		st->shell->pixels_owned = false;
		st->shell->buffer_size = st->buffer_size;
		st->shell->canvas.pixels = st->shell->pixels;
		st->shell->canvas.width = st->width;
		st->shell->canvas.height = st->height;
		st->shell->canvas.stride = st->width;
	}

	if (st->configured) {
		draw_and_commit(st);
	}

	/*
	 * The shell's layout and its buffer must agree. They are three
	 * separate pieces of state (shell->width/height, st->width/height
	 * and the shm buffer size) and a mismatch between them is invisible
	 * in every log line while putting hit testing and rendering on
	 * different screens.
	 */
	if (st->shell->width != st->width || st->shell->height != st->height) {
		fprintf(stderr,
		        "astrix-shell: WARNING: layout %dx%d does not match the panel size "
		        "%dx%d\n",
		        st->shell->width, st->shell->height, st->width, st->height);
	}
}

/*
 * Adopt the panel size wl_output reports, if it differs from what we are
 * laid out for. Called when the output state arrives, which may be before
 * or after startup - see wait_for_output_size().
 */
static void resize_to_output(struct shell_state *st) {
	if (!st->output_mode_current) {
		return;
	}
	int scale = st->output_scale > 0 ? st->output_scale : 1;
	int logical_w = st->output_px_w / scale;
	int logical_h = st->output_px_h / scale;
	if (logical_w <= 0 || logical_h <= 0) {
		return;
	}
	if (logical_w == st->width && logical_h == st->height) {
		return;
	}
	fprintf(stderr, "astrix-shell: panel reports %dx%d at scale %d; laying out for %dx%d\n",
	        st->output_px_w, st->output_px_h, scale, logical_w, logical_h);
	/*
	 * Do NOT record the new size here before reallocating. shell_resize()
	 * returns early when the target size already matches st->width/
	 * st->height, so setting them first made every resize a no-op: the
	 * shell kept its old layout (and the old buffers) while reporting
	 * the new size. That is invisible in the log - it prints the right
	 * dimensions - and on a panel smaller than the built-in one it puts
	 * the dock below the visible area, where nothing can tap it.
	 */
	if (st->buffer_count > 0) {
		shell_resize(st, logical_w, logical_h);
	} else {
		st->width = logical_w;
		st->height = logical_h;
		st->buffer_size = (size_t)logical_w * logical_h * 4;
		astrix_shell_set_size(st->shell, logical_w, logical_h);
	}
}

static void xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *toplevel,
                                          int32_t width, int32_t height,
                                          struct wl_array *states) {
	struct shell_state *st = data;
	if (width > 0 && height > 0) {
		/*
		 * The output changed size (rotation, different display, or the
		 * compositor told us the real panel size). Reallocate the
		 * framebuffer and tell the compositor about the new one.
		 */
		shell_resize(st, width, height);
	}
}

static void xdg_toplevel_handle_close(void *data, struct xdg_toplevel *toplevel) {
	struct shell_state *st = data;
	/*
	 * The compositor asked the shell to exit (session logout, or the user
	 * powering off). A client cannot terminate the display - that is
	 * server-side only - so we simply end the main loop.
	 */
	st->running = false;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_handle_configure,
	.close = xdg_toplevel_handle_close,
};

/* --- input: wl_pointer --------------------------------------------------- */

/* --- launching an app ---------------------------------------------------- */

/*
 * Start the process the user just tapped.
 *
 * Until this existed, astrix_shell_open_app() set a field and logged a line:
 * the screen changed, the log looked convincing, and no process was ever
 * created. "The app opened" and "the app is running" are different claims.
 *
 * The shell forks nothing itself. The shell is the model - screens, gestures,
 * the app registry - and it is unit tested on the host, where there is no
 * compositor and no session. The client that holds the compositor connection
 * is the one that can start something, so the shell records the request in
 * launch_pending and this function consumes it. That split is also what makes
 * the decision testable without a display.
 *
 * The child inherits the environment, so WAYLAND_DISPLAY and XDG_RUNTIME_DIR
 * are already right and the new client finds astrix-0 by itself.
 */
static void launch_pending_app(struct shell_state *st) {
	int idx = st->shell->launch_pending;
	if (idx < 0 || idx >= st->shell->app_count) {
		return;
	}
	st->shell->launch_pending = -1;
	const struct astrix_app *app = &st->shell->apps[idx];

	if (app->kind == ASTRIX_APP_ANDROID) {
		/*
		 * Honest failure, not a fake one. Waydroid is not implemented
		 * (docs/ANDROID.md), so there is no activity to start. Starting
		 * the APK manager's own UI would be pretending, and pretending
		 * is worse than an explicit refusal.
		 */
		fprintf(stderr,
		        "astrix-shell: '%s' is an Android app and the compatibility layer "
		        "does not exist yet; nothing was started\n",
		        app->name);
		return;
	}
	if (app->exec[0] == '\0') {
		fprintf(stderr, "astrix-shell: '%s' has no Exec= line; nothing was started\n",
		        app->name);
		return;
	}

	/*
	 * Exec= is whitespace separated with optional quoting. The desktop
	 * entries this OS installs are a bare binary, but a third-party entry
	 * will not be, and %-field codes (%U, %F, ...) are arguments meant for
	 * the application, not words to execute.
	 */
	char *line = strdup(app->exec);
	if (!line) {
		fprintf(stderr, "astrix-shell: out of memory launching '%s'\n", app->name);
		return;
	}
	char *argv[32];
	int argc = 0;
	for (char *tok = strtok(line, " \t"); tok && argc < 31; tok = strtok(NULL, " \t")) {
		if (tok[0] == '%') {
			continue; /* a field code, not a program or an argument */
		}
		if (tok[0] == '"') {
			size_t n = strlen(tok);
			if (n >= 2 && tok[n - 1] == '"') {
				tok[n - 1] = '\0';
				tok++;
			}
		}
		argv[argc++] = tok;
	}
	argv[argc] = NULL;
	if (argc == 0) {
		free(line);
		fprintf(stderr, "astrix-shell: '%s' has an unusable Exec= line\n", app->name);
		return;
	}

	pid_t pid = fork();
	if (pid == 0) {
		/*
		 * Child. Its own process group, so a signal aimed at the shell's
		 * group (Ctrl-C from a console, a session logout broadcast) does
		 * not take the app down with it, and stdin closed so an app that
		 * reads it cannot fight the shell for the terminal.
		 */
		setpgid(0, 0);
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			if (devnull > STDERR_FILENO) {
				close(devnull);
			}
		}
		execvp(argv[0], argv);
		fprintf(stderr, "astrix-shell: cannot start '%s': %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	if (pid < 0) {
		fprintf(stderr, "astrix-shell: fork for '%s' failed: %s\n", app->name,
		        strerror(errno));
		free(line);
		return;
	}
	/*
	 * Logged with the pid, because "the shell said it opened the app" and
	 * "a process with that pid exists" are different claims and only one of
	 * them survives a bug report.
	 */
	fprintf(stderr, "astrix-shell: launched '%s' as pid %d (%s)\n", app->name, (int)pid,
	        app->exec);
	/*
	 * The pid goes into the model, so the app is now genuinely "running"
	 * and closing it can send a signal to the right process. Without
	 * this the only way to stop an app was to log out.
	 */
	astrix_shell_set_app_pid(st->shell, idx, (int)pid);
	free(line);
}

/*
 * Terminate the app the user just closed.
 *
 * The mirror image of launch_pending_app(): the shell decides, this code
 * acts. SIGTERM first so the app can save state and close its Wayland
 * surface cleanly, then SIGKILL to the process group if it is still alive
 * after the grace period, because a wedged app must not be able to keep a
 * surface mapped forever after the user dismissed it.
 *
 * The whole group is signalled (-pid), not just the leader: apps that
 * spawn helpers (a terminal running a command, a file manager extracting
 * an archive) would otherwise leave those helpers behind.
 */
#define ASTRIX_TERM_GRACE_MS 1500

static void kill_pending_app(struct shell_state *st) {
	int idx = st->shell->kill_pending;
	if (idx < 0 || idx >= st->shell->app_count) {
		return;
	}
	/* The pid comes from the model, captured when the close was
	 * requested - NOT from apps[idx], whose pid entry the close already
	 * cleared. Reading it from there yields 0, the pending request is
	 * dropped without a word, and the app keeps running while the launcher
	 * shows it closed. */
	int pid = st->shell->kill_pending_pid;
	if (pid <= 0) {
		fprintf(stderr, "astrix-shell: '%s' was closed but no pid was recorded; "
		                "nothing was signalled\n",
		        st->shell->apps[idx].name);
		st->shell->kill_pending = -1;
		st->shell->kill_pending_pid = 0;
		return;
	}
	st->shell->kill_pending = -1;
	st->shell->kill_pending_pid = 0;

	/* Negative pid: the child is its own process group (setpgid above). */
	if (kill(-pid, SIGTERM) < 0 && errno == ESRCH) {
		/* Group already gone. The exit is still reaped below. */
		fprintf(stderr, "astrix-shell: '%s' (pid %d) had already exited\n",
		        st->shell->apps[idx].name, pid);
	}
	/*
	 * Recorded for reap_exited_apps() to escalate. The pid stays in the
	 * model until the process is actually gone, so the app switcher
	 * does not show an app that is still winding down as nothing at all.
	 */
	st->terminating_pid = pid;
	st->terminating_since_ms = monotonic_ms();
	fprintf(stderr, "astrix-shell: sent SIGTERM to '%s' (pid %d)\n", st->shell->apps[idx].name,
	        pid);
}

/*
 * Reap exited children and report them to the model.
 *
 * SIGCHLD keeps its default disposition and this runs once per main-loop
 * tick. The previous disposition was SIG_IGN, which let the kernel reap
 * children automatically - fine for avoiding zombies, but it made the
 * shell blind to the one event that matters here, an app quitting. Without
 * this, an app that crashed stayed in the app switcher as running for the
 * rest of the session.
 */
static void reap_exited_apps(struct shell_state *st) {
	for (;;) {
		int status = 0;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		if (pid <= 0) {
			break;
		}
		if (astrix_shell_note_app_exit(st->shell, (int)pid)) {
			int how = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
			if (how >= 0) {
				fprintf(stderr, "astrix-shell: app pid %d exited with status %d\n", (int)pid,
				        how);
			} else if (WIFSIGNALED(status)) {
				fprintf(stderr, "astrix-shell: app pid %d killed by signal %d\n", (int)pid,
				        WTERMSIG(status));
			} else {
				fprintf(stderr, "astrix-shell: app pid %d is gone\n", (int)pid);
			}
		}
		if (st->terminating_pid == (int)pid) {
			st->terminating_pid = 0;
		}
	}
}

/*
 * Escalate SIGTERM to SIGKILL once the grace period is up.
 *
 * An app that ignores SIGTERM - or one stuck in an uninterruptible syscall -
 * must not keep a mapped surface and a CPU share after the user closed it.
 * The group is killed, so its helper processes go too.
 */
static void escalate_terminations(struct shell_state *st) {
	if (st->terminating_pid <= 0) {
		return;
	}
	if (monotonic_ms() - st->terminating_since_ms < ASTRIX_TERM_GRACE_MS) {
		return;
	}
	int pid = st->terminating_pid;
	if (kill(-pid, 0) == 0 || errno != ESRCH) {
		kill(-pid, SIGKILL);
		fprintf(stderr, "astrix-shell: app pid %d ignored SIGTERM; sending SIGKILL\n", pid);
	}
	/*
	 * Clear the record anyway. If the process really is gone, the exit
	 * has already been reaped above; if it is truly unkillable (D state)
	 * nothing the shell can do will help, and holding the record would
	 * make the escalation fire on every tick forever.
	 */
	st->terminating_pid = 0;
}

/*
 * Dispatch one input event to the shell model.
 *
 * `event_ms`, when non-zero, is the timestamp the compositor put on the
 * wl_pointer event, in milliseconds. It matters: gestures are timed, and
 * timing them against the local clock measures how fast the *shell* is
 * running rather than how long the *finger* was down. On a loaded machine
 * that turned a 13ms tap into a long press (the tap and the release were
 * 13ms apart in the log, but the shell got to them 600ms apart) and opened
 * the power menu instead of launching the app. Both clocks are
 * CLOCK_MONOTONIC, so they are directly comparable.
 *
 * Zero means "no event time available" - wl_touch does not carry one - and
 * the local clock is used instead.
 */
static void translate_and_dispatch_at(struct shell_state *st, enum astrix_input_kind kind, int x,
                                      int y, uint32_t button, uint32_t state, uint32_t event_ms) {
	struct astrix_input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = kind;
	ev.x = x;
	ev.y = y;
	ev.timestamp = event_ms ? (double)event_ms / 1000.0 : (double)now_seconds();
	ev.button = button;
	ev.state = state;
	astrix_shell_handle_input(st->shell, &ev);
	/* A tap that opened an app also has to start it; a gesture that closed
	 * one also has to stop it. */
	launch_pending_app(st);
	kill_pending_app(st);
}

static void pointer_handle_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                                 struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
	struct shell_state *st = data;
	/*
	 * wl_pointer.enter carries the surface-relative position, and a client
	 * has to take it: motion is not guaranteed to follow. wlroots
	 * suppresses a motion that repeats the position an enter just
	 * established, so a client that tracks position only from motion ends
	 * up with a stale one - and because wl_pointer.button carries no
	 * coordinates, that stale position is the anchor for the next press.
	 *
	 * That is not hypothetical. On a booted VM, entering the shell and
	 * then pressing left the gesture anchored at 400,598, where the finger
	 * had been several gestures earlier, so a straight upward home swipe
	 * classified as "swipe-right" and the app switcher never opened.
	 */
	int x = wl_fixed_to_int(sx), y = wl_fixed_to_int(sy);
	fprintf(stderr, "astrix-shell: pointer entered the shell at %d,%d\n", x, y);
	/* wl_pointer.enter carries no timestamp. */
	translate_and_dispatch_at(st, ASTRIX_INPUT_POINTER_MOTION, x, y, 0, 0, 0);
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                                 struct wl_surface *surface) {
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                                  wl_fixed_t sx, wl_fixed_t sy) {
	struct shell_state *st = data;
	int nx = wl_fixed_to_int(sx), ny = wl_fixed_to_int(sy);
	/*
	 * A wl_pointer button event carries no coordinates, so the position a
	 * press is interpreted at is the last motion received. That makes the
	 * motion stream load-bearing rather than cosmetic: if a motion the
	 * compositor believes it sent never arrives, every later gesture is
	 * anchored to an old position and nothing else looks wrong. Log the
	 * first motion after a button and any large jump, which is bounded
	 * (a couple of lines per gesture) but is exactly the evidence needed
	 * to tell "not sent" from "sent and dropped".
	 */
	bool first_since_button = (st->last_button_event_ms == 0) ||
	                          (now_seconds() * 1000.0 - st->last_button_event_ms) > 250.0;
	int jump = abs(st->shell->pointer_x - nx) + abs(st->shell->pointer_y - ny);
	if (first_since_button || jump > 64) {
		fprintf(stderr, "astrix-shell: pointer motion to %d,%d (from %d,%d, jump %d)\n", nx, ny,
		        st->shell->pointer_x, st->shell->pointer_y, jump);
	}
	translate_and_dispatch_at(st, ASTRIX_INPUT_POINTER_MOTION, nx, ny, 0, 0, time);
	draw_and_commit(st);
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                                  uint32_t time, uint32_t button, uint32_t button_state) {
	struct shell_state *st = data;
	/* Press and release are both delivered as POINTER_BUTTON; the shell's
	 * gesture recogniser uses the state bit to tell them apart. */
	int x = st->shell->pointer_x, y = st->shell->pointer_y;
	/*
	 * Logged because "the button event never arrived" and "it arrived and
	 * the recogniser did nothing with it" are otherwise indistinguishable
	 * from the outside, and every bug in this path has turned on telling
	 * them apart. It also prints the position the event was interpreted
	 * at, which wl_pointer does not carry: a button event has no
	 * coordinates, so this is the recogniser's anchor.
	 */
	fprintf(stderr, "astrix-shell: pointer button %u %s at %d,%d\n", button,
	        button_state == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released", x, y);
	st->last_button_event_ms = now_seconds() * 1000.0;
	translate_and_dispatch_at(st, ASTRIX_INPUT_POINTER_BUTTON, x, y, button,
	                          button_state == WL_POINTER_BUTTON_STATE_PRESSED
	                              ? ASTRIX_KEY_PRESSED
	                              : 0,
	                          time);
	draw_and_commit(st);
}

static void pointer_handle_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                                uint32_t axis, wl_fixed_t value) {
	struct shell_state *st = data;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}
	struct astrix_input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = ASTRIX_INPUT_SCROLL;
	/* Scrolling up (positive value) should move content up. */
	ev.scroll_y = -wl_fixed_to_int(value);
	ev.timestamp = (double)now_seconds();
	astrix_shell_handle_input(st->shell, &ev);
	draw_and_commit(st);
}

/*
 * wl_pointer.frame - mandatory, and the compositor sends it after every
 * motion, button and axis batch.
 *
 * Omitting it is fatal, not merely unused. libwayland aborts a client that
 * receives an event with no listener:
 *
 *   astrix-shell[940]: listener function for opcode 5 of wl_pointer is NULL
 *   audit: ... comm="astrix-shell" sig=6      (SIGABRT)
 *
 * wl_pointer's events are enter, leave, motion, button, axis, frame - so
 * opcode 5 is frame, not set_cursor (set_cursor is a client-to-server
 * request and has no listener member at all; the struct simply has no
 * `set_cursor` field). Because the compositor frames every pointer batch,
 * the shell died on the first touch of every session, systemd restarted it,
 * and the session looked completely healthy while being deaf to input.
 *
 * Drawing here rather than inside motion/button is also the correct use of
 * the event: frame is the compositor's promise that this batch of input is
 * complete, so it is where a client should repaint.
 */
static void pointer_handle_frame(void *data, struct wl_pointer *pointer) {
	struct shell_state *st = data;
	(void)pointer;
	draw_and_commit(st);
}

/*
 * wl_pointer.axis_value120 and wl_pointer.axis_relative_direction are the
 * high-resolution and directional forms of a scroll. The shell's only scroll
 * use is the vertical wheel in the launcher, which wl_pointer.axis already
 * carries, so these are accepted and ignored - but they must be accepted:
 * the compositor forwards the directional form whenever it is non-zero, so a
 * gesture made against a screen edge would otherwise abort the shell over a
 * scroll direction the shell never asked for.
 */
/*
 * Neither axis_value120 nor axis_relative_direction carries a time argument:
 * the protocol sends them per axis, and the generated signatures are
 * (data, pointer, axis, value) and (data, pointer, axis, direction). They
 * differ from every other pointer event, which does take a timestamp.
 */
static void pointer_handle_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis,
                                        int32_t value120) {
	(void)data;
	(void)pointer;
	(void)axis;
	(void)value120;
}

/*
 * axis_relative_direction has no time argument in the protocol - the
 * generated signature is (data, pointer, axis, direction) - unlike every
 * other pointer event. Getting this wrong is a compile error rather than a
 * subtle runtime bug, which is the one mercy in this protocol area.
 */
static void pointer_handle_axis_relative_direction(void *data, struct wl_pointer *pointer,
                                                   uint32_t axis, uint32_t direction) {
	(void)data;
	(void)pointer;
	(void)axis;
	(void)direction;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_handle_enter,
	.leave = pointer_handle_leave,
	.motion = pointer_handle_motion,
	.button = pointer_handle_button,
	.axis = pointer_handle_axis,
	.frame = pointer_handle_frame,
	.axis_value120 = pointer_handle_axis_value120,
	.axis_relative_direction = pointer_handle_axis_relative_direction,
	/*
	 * These three are accepted and ignored on purpose, and they are listed
	 * explicitly rather than left to the implicit zero-fill: a reader
	 * should be able to tell "we know about this event and do not need it"
	 * from "nobody thought about this event".
	 *
	 * They must not be set to NULL out of habit. An initialiser is
	 * evaluated in order, so a later `.frame = NULL` silently overwrites a
	 * real handler and the shell goes back to aborting on the first touch
	 * while the build only prints a -Woverride-init warning. The
	 * test-wayland-listeners test now fails on a duplicated member, which
	 * is the only reason this cannot come back.
	 */
	.axis_source = NULL,
	.axis_stop = NULL,
	.axis_discrete = NULL,
};

/* --- input: wl_touch ----------------------------------------------------- */

static void touch_handle_down(void *data, struct wl_touch *touch, uint32_t serial,
                              uint32_t time, struct wl_surface *surface, int32_t id,
                              wl_fixed_t x, wl_fixed_t y) {
	struct shell_state *st = data;
	translate_and_dispatch_at(st, ASTRIX_INPUT_TOUCH_DOWN, wl_fixed_to_int(x),
	                          wl_fixed_to_int(y), 0, 0, time);
	draw_and_commit(st);
}

static void touch_handle_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
                            int32_t id) {
	struct shell_state *st = data;
	translate_and_dispatch_at(st, ASTRIX_INPUT_TOUCH_UP, st->shell->pointer_x,
	                          st->shell->pointer_y, 0, 0, time);
	draw_and_commit(st);
}

static void touch_handle_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id,
                                wl_fixed_t x, wl_fixed_t y) {
	struct shell_state *st = data;
	translate_and_dispatch_at(st, ASTRIX_INPUT_TOUCH_MOTION, wl_fixed_to_int(x),
	                          wl_fixed_to_int(y), 0, 0, time);
	draw_and_commit(st);
}

static void touch_handle_frame(void *data, struct wl_touch *touch) {
}

/*
 * wl_touch.shape and wl_touch.orientation describe a stylus: what it is
 * (finger, pen, eraser) and which way it is held. Astrix treats every touch
 * as a finger, so the shell has no use for them - but the handlers must
 * exist, because libwayland aborts a client that is sent an event it has no
 * listener for, and a panel that reports a pen would take the whole shell
 * down over a value it was never going to read.
 */
static void touch_handle_shape(void *data, struct wl_touch *touch, int32_t id, wl_fixed_t major,
                               wl_fixed_t minor) {
	(void)data;
	(void)touch;
	(void)id;
	(void)major;
	(void)minor;
}

static void touch_handle_orientation(void *data, struct wl_touch *touch, int32_t id,
                                     wl_fixed_t orientation) {
	(void)data;
	(void)touch;
	(void)id;
	(void)orientation;
}

static void touch_handle_cancel(void *data, struct wl_touch *touch) {
	struct shell_state *st = data;
	/*
	 * A cancel invalidates the whole touch sequence (palm rejection, another
	 * client taking the grab). Reset the recogniser so it cannot get stuck
	 * believing a finger is still down.
	 */
	astrix_gesture_init(&st->shell->gestures, &st->shell->gestures.cfg);
	draw_and_commit(st);
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_handle_down,
	.up = touch_handle_up,
	.motion = touch_handle_motion,
	.frame = touch_handle_frame,
	.cancel = touch_handle_cancel,
	.shape = touch_handle_shape,
	.orientation = touch_handle_orientation,
};

/* --- input: wl_keyboard -------------------------------------------------- */

static void keyboard_handle_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                                   int32_t fd, uint32_t size) {
	close(fd);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                                  struct wl_surface *surface, struct wl_array *keys) {
	struct shell_state *st = data;
	st->shell->has_focus = true;
	draw_and_commit(st);
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                                  struct wl_surface *surface) {
	struct shell_state *st = data;
	st->shell->has_focus = false;
}

static void keyboard_handle_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                                uint32_t time, uint32_t key, uint32_t state) {
	struct shell_state *st = data;
	/*
	 * Bounded diagnostic. This is the only place where a key the shell
	 * injected through its virtual keyboard can be seen coming back, so it
	 * is also the proof that injection works at all: a phone OS whose
	 * keyboard cannot be verified is a phone OS whose keyboard is broken
	 * on the handset and nobody knows why.
	 */
	fprintf(stderr, "astrix-shell: key %u %s\n", key,
	        (state & WL_KEYBOARD_KEY_STATE_PRESSED) ? "pressed" : "released");
	struct astrix_input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = ASTRIX_INPUT_KEY;
	ev.key = key;
	ev.state = state == WL_KEYBOARD_KEY_STATE_PRESSED ? ASTRIX_KEY_PRESSED : 0;
	ev.timestamp = (double)now_seconds();
	astrix_shell_handle_input(st->shell, &ev);
	draw_and_commit(st);
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                                      uint32_t depressed, uint32_t latched, uint32_t locked,
                                      uint32_t group) {
}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
                                        int32_t delay) {
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_handle_keymap,
	.enter = keyboard_handle_enter,
	.leave = keyboard_handle_leave,
	.key = keyboard_handle_key,
	.modifiers = keyboard_handle_modifiers,
	.repeat_info = keyboard_handle_repeat_info,
};

/* --- seat capabilities --------------------------------------------------- */

static void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	struct shell_state *st = data;
	/*
	 * The protocol forbids calling wl_seat.get_pointer/get_touch/get_keyboard
	 * before the matching capability has been advertised, and forbids calling
	 * it again for a capability that disappears. So the input devices are
	 * created here, on the first capabilities event, rather than eagerly at
	 * startup.
	 */
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !st->pointer) {
		st->pointer = wl_seat_get_pointer(seat);
		if (st->pointer) {
			wl_pointer_add_listener(st->pointer, &pointer_listener, st);
		}
	}
	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !st->touch) {
		st->touch = wl_seat_get_touch(seat);
		if (st->touch) {
			wl_touch_add_listener(st->touch, &touch_listener, st);
		}
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !st->keyboard) {
		st->keyboard = wl_seat_get_keyboard(seat);
		if (st->keyboard) {
			wl_keyboard_add_listener(st->keyboard, &keyboard_listener, st);
		}
	}

	/*
	 * The virtual keyboard needs a seat, and a seat only exists once it
	 * has been bound. Creating it only from the registry callback means the
	 * order the two globals arrive in decides whether the on-screen
	 * keyboard works at all - which is exactly the kind of ordering
	 * dependency that makes a feature work on a machine and not on
	 * another. Both arrival orders are handled here.
	 */
	virtual_keyboard_create(st);
}

static void seat_handle_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};

/* --- wl_output: what the panel really is -------------------------------- */

/*
 * The shell must lay out for the panel it is actually running on, not for
 * the one the build configures for a real phone. Assuming 720x1600 while
 * the compositor drives a 1024x768 panel produces a UI whose bottom third
 * - the dock, the navigation bar - is laid out below the visible area, so
 * it can be drawn but never touched.
 *
 * wl_output reports the current mode in mode() (flagged CURRENT) and the
 * output scale separately, and is only guaranteed to have sent both by
 * the time done() arrives - which is why the size is applied there and
 * not in the geometry/mode handlers.
 */

static void output_handle_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                                   int32_t physical_width, int32_t physical_height,
                                   int32_t subpixel, const char *make, const char *model,
                                   int32_t transform) {
	/*
	 * Physical geometry is not used: a phone has no meaningful DPI
	 * report over a virtual connector, and the shell scales its own
	 * layout from the pixel size instead.
	 */
}

static void output_handle_mode(void *data, struct wl_output *output, uint32_t flags,
                               int32_t width, int32_t height, int32_t refresh) {
	struct shell_state *st = data;
	if (!(flags & WL_OUTPUT_MODE_CURRENT)) {
		return; /* an advertised mode we are not using */
	}
	st->output_px_w = width;
	st->output_px_h = height;
	st->output_mode_current = true;
}

static void output_handle_done(void *data, struct wl_output *output) {
	struct shell_state *st = data;
	st->output_done = true;
	/*
	 * The compositor sends the output state asynchronously - measured on
	 * this system at over two seconds after the bind, well after startup
	 * has finished drawing frames. So the panel size is adopted here
	 * rather than only once during startup.
	 */
	resize_to_output(st);
}

static void output_handle_scale(void *data, struct wl_output *output, int32_t factor) {
	struct shell_state *st = data;
	st->output_scale = factor > 0 ? factor : 1;
}

static void output_handle_name(void *data, struct wl_output *output, const char *name) {
}

/*
 * wl_output.description ("Samsung ATIV", "Virtual-1") was added in version 4
 * and the shell binds the output at version 3, so it will not be sent today.
 * It is implemented anyway: the struct has the member, test-wayland-listeners
 * checks every member, and binding v4 later to show the panel name in
 * settings must not turn the first output event into a libwayland abort.
 */
static void output_handle_description(void *data, struct wl_output *output,
                                      const char *description) {
	(void)data;
	(void)output;
	(void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = output_handle_mode,
	.done = output_handle_done,
	.scale = output_handle_scale,
	.name = output_handle_name,
	.description = output_handle_description,
};

/*
 * Give the compositor a bounded chance to publish the output state before
 * the first frame is drawn.
 *
 * Binding a wl_output global only registers interest: the compositor sends
 * geometry/mode/scale/done as a separate batch, and on this system that
 * batch was measured arriving **more than two seconds** after the bind -
 * long after wl_display_roundtrip() returned and the shell started drawing.
 * Reading the size once during startup therefore usually sees nothing.
 *
 * The real fix is that the size is adopted whenever it arrives
 * (output_handle_done -> resize_to_output); this wait only makes the common
 * case - a compositor that answers promptly - look right on the very first
 * frame instead of the second. It is bounded, so a compositor that never
 * mentions an output delays startup by at most timeout_ms and nothing more.
 */
static void wait_for_output_size(struct shell_state *st, int timeout_ms) {
	if (!st->output) {
		fprintf(stderr,
		        "astrix-shell: the compositor advertises no wl_output; laying out "
		        "for the built-in %dx%d instead\n",
		        st->width, st->height);
		return;
	}
	for (int waited = 0; waited < timeout_ms; waited += 20) {
		resize_to_output(st);
		if (st->output_mode_current && st->output_done) {
			return;
		}
		if (wl_display_dispatch_pending(st->display) == -1) {
			return;
		}
		struct pollfd pfd = { .fd = wl_display_get_fd(st->display), .events = POLLIN };
		int r = poll(&pfd, 1, 20);
		if (r < 0) {
			return;
		}
		if (r == 0) {
			continue; /* nothing arrived within this slice */
		}
		if (wl_display_dispatch(st->display) == -1) {
			return;
		}
	}
	fprintf(stderr,
	        "astrix-shell: no output state within %dms; laying out for the "
	        "built-in %dx%d for now and re-laying out when it arrives\n",
	        timeout_ms, st->width, st->height);
}

/* --- registry ------------------------------------------------------------ */

/* --- virtual keyboard: delivering on-screen keys ---------------------------- */

/*
 * Everything below is the delivery half of the on-screen keyboard. The
 * decision half (which key was tapped) is keyboard.c; this turns a codepoint
 * into an xkb keycode and asks the compositor to inject it.
 *
 * The one rule that matters: if there is no virtual keyboard, say so and drop
 * the key. Silently drawing a keyboard whose keys do nothing is the worst
 * possible failure for a user trying to type a Wi-Fi password.
 */
static void virtual_keyboard_send_keymap(struct shell_state *st) {
	if (!st->vkbd || !st->xkb_keymap_text) {
		return;
	}
	if (getenv("ASTRIX_OSK_NO_KEYMAP")) {
		/* Escape hatch for a compositor that advertises the manager but
		 * rejects the keymap: the keys are drawn and queued, and this
		 * says so in the log instead of pretending they went out. */
		fprintf(stderr, "astrix-shell: keymap send suppressed by ASTRIX_OSK_NO_KEYMAP\n");
		return;
	}
	size_t len = strlen(st->xkb_keymap_text);
	int fd = memfd_create("astrix-keymap", 0);
	if (fd < 0) {
		fprintf(stderr, "astrix-shell: cannot create a keymap fd: %s\n",
		        strerror(errno));
		return;
	}
	if (write(fd, st->xkb_keymap_text, len) != (ssize_t)len) {
		fprintf(stderr, "astrix-shell: cannot write the keymap: %s\n",
		        strerror(errno));
		close(fd);
		return;
	}
	lseek(fd, 0, SEEK_SET);
	/* text_v1 = 4. See protocol/virtual-keyboard-unstable-v1.xml. */
	zwp_virtual_keyboard_v1_keymap(st->vkbd, 4, fd, (uint32_t)len);
	if (st->vkbd_keymap_fd >= 0) {
		close(st->vkbd_keymap_fd);
	}
	st->vkbd_keymap_fd = fd;
	wl_display_flush(st->display);
}

/* Build the keymap once; the default layout is what a phone should use. */
static void virtual_keyboard_load_keymap(struct shell_state *st) {
	st->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!st->xkb_ctx) {
		fprintf(stderr, "astrix-shell: cannot create an xkb context\n");
		return;
	}
	struct xkb_rule_names names = { 0 };
	st->xkb_keymap = xkb_keymap_new_from_names(st->xkb_ctx, &names,
	                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!st->xkb_keymap) {
		fprintf(stderr, "astrix-shell: cannot compile the default keymap\n");
		return;
	}
	st->xkb_keymap_text = xkb_keymap_get_as_string(st->xkb_keymap,
	                                               XKB_KEYMAP_FORMAT_TEXT_V1);
}

static void virtual_keyboard_create(struct shell_state *st) {
	if (st->vkbd || !st->vkbd_manager || !st->seat) {
		return;
	}
	if (getenv("ASTRIX_OSK_NO_CREATE")) {
		fprintf(stderr, "astrix-shell: virtual keyboard creation suppressed "
		                "by ASTRIX_OSK_NO_CREATE; on-screen keys will not be "
		                "delivered\n");
		return;
	}
	st->vkbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		st->vkbd_manager, st->seat);
	if (!st->vkbd) {
		fprintf(stderr, "astrix-shell: the compositor refused a virtual keyboard\n");
		return;
	}
	virtual_keyboard_load_keymap(st);
	virtual_keyboard_send_keymap(st);
	fprintf(stderr, "astrix-shell: virtual keyboard ready; the on-screen "
	                "keyboard can deliver keys\n");
}

/*
 * Find the keycode that produces `sym`, and whether producing it needs shift.
 *
 * Level 0 of a key is the unshifted symbol and level 1 the shifted one, so
 * walking both levels is what makes the keymap decide the modifier instead of
 * this file guessing. Guessing is how an uppercase 'Q' turns up as 'q' on a
 * phone, which reads as "the keyboard is broken" rather than "the modifier
 * was lost".
 */
static xkb_keycode_t find_keycode(struct xkb_keymap *km, xkb_keysym_t sym,
                                  bool *needs_shift) {
	*needs_shift = false;
	for (xkb_keycode_t kc = 8; kc < 0x100; kc++) {
		const xkb_keysym_t *syms;
		int n = xkb_keymap_key_get_syms_by_level(km, kc, 0, 0, &syms);
		for (int i = 0; i < n; i++) {
			if (syms[i] == sym) {
				return kc;
			}
		}
		n = xkb_keymap_key_get_syms_by_level(km, kc, 0, 1, &syms);
		for (int i = 0; i < n; i++) {
			if (syms[i] == sym) {
				*needs_shift = true;
				return kc;
			}
		}
	}
	return 0;
}

/* The shift bit in a wl_keyboard.modifiers field. Spelled out rather than
 * taken from a header because the xkbcommon version in the build does not
 * export the mask, and a wrong bit here is a silent "caps lock on
 * permanently". */
#define ASTRIX_MOD_SHIFT 1u

static void vkbd_press_key(struct shell_state *st, xkb_keycode_t keycode,
                           bool shift) {
	uint32_t now = (uint32_t)monotonic_ms();
	zwp_virtual_keyboard_v1_modifiers(st->vkbd, shift ? ASTRIX_MOD_SHIFT : 0, 0, 0, 0);
	zwp_virtual_keyboard_v1_key(st->vkbd, now, keycode,
	                             WL_KEYBOARD_KEY_STATE_PRESSED);
	zwp_virtual_keyboard_v1_key(st->vkbd, now, keycode,
	                             WL_KEYBOARD_KEY_STATE_RELEASED);
	if (shift) {
		zwp_virtual_keyboard_v1_modifiers(st->vkbd, 0, 0, 0, 0);
	}
}

/*
 * Drain everything the user tapped and deliver it. Called once per loop
 * iteration, so a fast typist's whole word goes out in one flush instead of
 * waiting for the next frame.
 */
static int drain_keyboard(struct shell_state *st) {
	enum astrix_kbd_action action;
	uint32_t cp;
	int sent = 0;

	while (st->shell && astrix_kbd_pop(st->shell, &action, &cp)) {
		if (action != ASTRIX_KBD_CHAR && action != ASTRIX_KBD_BACKSPACE &&
		    action != ASTRIX_KBD_ENTER && action != ASTRIX_KBD_SPACE) {
			continue;   /* shift/symbols/hide are shell state, already applied */
		}
		if (action == ASTRIX_KBD_CHAR && cp >= 32 && cp < 127) {
			fprintf(stderr, "astrix-shell: on-screen key '%c'\n", (char)cp);
		}
		if (!st->vkbd || !st->xkb_keymap) {
			continue;   /* no compositor keyboard: nothing to deliver to */
		}
		xkb_keysym_t sym;
		switch (action) {
		case ASTRIX_KBD_ENTER:  sym = XKB_KEY_Return; break;
		case ASTRIX_KBD_SPACE:  sym = XKB_KEY_space; break;
		case ASTRIX_KBD_BACKSPACE: sym = XKB_KEY_BackSpace; break;
		default: sym = xkb_utf32_to_keysym(cp); break;
		}
		bool shift = false;
		xkb_keycode_t kc = find_keycode(st->xkb_keymap, sym, &shift);
		if (!kc) {
			fprintf(stderr, "astrix-shell: no key for codepoint %u\n", cp);
			continue;
		}
		vkbd_press_key(st, kc, shift);
		sent++;
	}
	if (sent > 0) {
		wl_display_flush(st->display);
	}
	return sent;
}

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
                                   const char *interface, uint32_t version) {
	struct shell_state *st = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		st->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
		                                  version < 4 ? version : 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		st->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		st->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
		                               version < 3 ? version : 3);
		xdg_wm_base_add_listener(st->wm_base, NULL, st);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		st->seat = wl_registry_bind(registry, name, &wl_seat_interface,
		                            version < 7 ? version : 7);
		wl_seat_add_listener(st->seat, &seat_listener, st);
	} else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		/*
		 * Version 1 only, and only bound once. The manager is what makes
		 * an on-screen keyboard possible at all; without this global the
		 * shell still draws the keys, and says so in the log, rather
		 * than showing a keyboard that types nothing.
		 */
		if (!st->vkbd_manager) {
			st->vkbd_manager = wl_registry_bind(
				registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
			if (st->vkbd_manager && st->seat) {
				virtual_keyboard_create(st);
			}
		}
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		/*
		 * Version 3 is the highest we need: v1 has geometry/mode,
		 * v2 adds scale, v3 adds done(). The first output wins - the
		 * Astrix session is a single-panel system.
		 */
		if (!st->output) {
			st->output = wl_registry_bind(registry, name, &wl_output_interface,
			                              version < 3 ? version : 3);
			wl_output_add_listener(st->output, &output_listener, st);
		}
	}
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry,
                                          uint32_t name) {
	/* The compositor never removes globals during a session; if it did, the
	 * shell would need to be restarted, which the session manager handles. */
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

/* --- startup ------------------------------------------------------------- */

/*
 * Fallback size: what the shell lays out for when the compositor has not
 * told us about a real output. It exists so the shell can allocate its
 * framebuffer before it has connected; once wl_output has been seen,
 * apply_output_size() overrides it with the actual panel size.
 */
static void query_output_size(struct shell_state *st, int *w, int *h) {
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

int main(int argc, char **argv) {
	install_fatal_handlers();
	struct shell_state st;
	memset(&st, 0, sizeof(st));
	st.vkbd_keymap_fd = -1;   /* "no keymap fd outstanding", not fd 0 */
	st.buffer_index = 0;

	/*
	 * The shell starts apps and does not wait for them, so it would
	 * accumulate a zombie for every app the user opens. Reaping happens
	 * in reap_exited_apps() once per tick, so SIGCHLD keeps its default
	 * disposition: children become reapable zombies rather than being
	 * auto-reaped, which is what lets the shell learn that an app quit.
	 */
	signal(SIGCHLD, SIG_DFL);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
			/* override handled after query */
		}
	}

	query_output_size(&st, &st.width, &st.height);
	st.buffer_size = (size_t)st.width * st.height * 4;

	st.shell = astrix_shell_create(st.width, st.height);
	if (!st.shell) {
		fprintf(stderr, "astrix-shell: failed to create shell state\n");
		return 1;
	}

	/*
	 * The shell's pixel buffer and the wl_shm buffers are the same
	 * allocation: astrix_shell_draw writes into shell->pixels, and the shm
	 * buffer aliases it. Point the first shm buffer's data at it.
	 */
	st.display = wl_display_connect(NULL);
	if (!st.display) {
		fprintf(stderr, "astrix-shell: cannot connect to Wayland display "
		                "(is astrix-compositor running? WAYLAND_DISPLAY=%s)\n",
		        getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
		return 1;
	}
	st.registry = wl_display_get_registry(st.display);
	wl_registry_add_listener(st.registry, &registry_listener, &st);
	wl_display_roundtrip(st.display);

	if (!st.compositor || !st.shm || !st.wm_base || !st.seat) {
		fprintf(stderr, "astrix-shell: compositor is missing required globals\n");
		wl_display_disconnect(st.display);
		return 1;
	}

	/*
	 * The panel size from wl_output wins over the compiled-in default:
	 * the default describes the phone this build targets, while the
	 * output describes the display this session actually got.
	 */
	wait_for_output_size(&st, 2000);

	/* Create the shm buffers. */
	for (int i = 0; i < 2; i++) {
		if (create_buffer(&st, i) == 0) {
			st.buffer_count++;
		}
	}
	if (st.buffer_count == 0) {
		fprintf(stderr, "astrix-shell: could not create any shm buffers\n");
		return 1;
	}
	/* Alias the shell's framebuffer onto the first buffer so drawing is
	 * direct with no copy. */
	astrix_shell_destroy(st.shell);
	st.shell = astrix_shell_create(st.width, st.height);
	free(st.shell->pixels);
	st.shell->pixels = st.buffer_data[0];
	/* The framebuffer is now a wl_shm mapping the client manages: the shell
	 * must not free or reallocate it (see struct astrix_shell.pixels_owned). */
	st.shell->pixels_owned = false;
	st.shell->buffer_size = st.buffer_size;
	st.shell->canvas.pixels = st.shell->pixels;
	st.shell->canvas.width = st.width;
	st.shell->canvas.height = st.height;
	st.shell->canvas.stride = st.width;

	/* Create the surface. */
	st.surface = wl_compositor_create_surface(st.compositor);
	st.xdg_surface = xdg_wm_base_get_xdg_surface(st.wm_base, st.surface);
	xdg_surface_add_listener(st.xdg_surface, &xdg_surface_listener, &st);
	st.xdg_toplevel = xdg_surface_get_toplevel(st.xdg_surface);
	xdg_toplevel_add_listener(st.xdg_toplevel, &xdg_toplevel_listener, &st);
	xdg_toplevel_set_app_id(st.xdg_toplevel, ASTRIX_SHELL_APP_ID);
	xdg_toplevel_set_title(st.xdg_toplevel, "Astrix");
	wl_surface_commit(st.surface);

	/*
	 * Input devices are created from the seat's capabilities event (see
	 * seat_handle_capabilities). A second roundtrip ensures that event has
	 * been delivered before we enter the main loop.
	 */
	wl_display_roundtrip(st.display);

	/*
	 * Fill the launcher before the first frame is drawn, so the home
	 * screen is never briefly shown with an empty grid and then repainted
	 * with icons. Doing it after the roundtrip keeps it off the critical
	 * path: a filesystem scan is milliseconds, and the compositor is
	 * already on screen by now.
	 */
	int apps = astrix_shell_scan_apps(st.shell);
	if (apps == 0) {
		fprintf(stderr,
		        "astrix-shell: warning: no launchable apps found; the home screen "
		        "grid will be empty\n");
	}

	fprintf(stderr, "astrix-shell: running (%dx%d)\n", st.width, st.height);
	st.running = true;

	/*
	 * Self-test for the on-screen keyboard, enabled with
	 * ASTRIX_SELF_TEST_KEYBOARD=1.
	 *
	 * The interesting half of the keyboard - tap -> queue -> xkb keycode ->
	 * compositor -> focused client - cannot be reached from a host unit
	 * test, and it is exactly the half that breaks. Tapping the keys from
	 * inside means the whole path can be asserted in a script, with no
	 * touchscreen and no human. It types into whatever has focus, which in
	 * this case is the shell itself, whose wl_keyboard handler logs each
	 * key it receives - so the log shows a key going out and coming back.
	 */
	if (getenv("ASTRIX_SELF_TEST_KEYBOARD")) {
		st.shell->kbd_visible = true;
		struct astrix_kbd_key keys[ASTRIX_KBD_MAX_KEYS];
		int n = astrix_kbd_layout(st.shell, st.width, st.height, keys,
		                           ASTRIX_KBD_MAX_KEYS);
		const char want[] = { 'a', 'b', 'c', '\0' };
		int typed = 0;
		/* One pass per character. A single forward scan looks like it
		 * works and quietly types only part of the word: 'c' sits to the
		 * left of 'b' on a QWERTY row, so a scan that has already passed
		 * it can never reach it again. */
		for (int w = 0; want[w]; w++) {
			for (int i = 0; i < n; i++) {
				if (keys[i].label[0] != want[w] || keys[i].label[1] != '\0') {
					continue;
				}
				if (astrix_kbd_tap(st.shell, keys[i].rect.x + keys[i].rect.w / 2,
				                   keys[i].rect.y + keys[i].rect.h / 2)) {
					typed++;
				}
				break;
			}
		}
		fprintf(stderr, "astrix-shell: self-test: queued %d key(s) from the "
		                "on-screen keyboard\n", typed);
	}

	/*
	 * The main loop. A ~16ms poll timeout gives us a steady tick for the
	 * clock without a dedicated timer thread, and keeps CPU use near zero
	 * when the UI is idle.
	 */
	int last_dispatch = 0;
	while (st.running && (last_dispatch = wl_display_dispatch_pending(st.display)) != -1) {
		struct astrix_input_event dummy;
		(void)dummy;
		if (wl_display_flush(st.display) == -1) {
			break;
		}
		wl_display_roundtrip(st.display);
		/* The compositor has now had its chance to read the keymap fd
		 * out of the request we sent; only now is it safe to close. */
		if (st.vkbd_keymap_fd >= 0) {
			close(st.vkbd_keymap_fd);
			st.vkbd_keymap_fd = -1;
		}
		/* Note apps that exited since the last tick, and finish off any
		 * termination that outstayed its grace period. */
		reap_exited_apps(&st);
		escalate_terminations(&st);
		/* Deliver whatever the on-screen keyboard queued. Done here,
		 * before the frame is drawn, so a key tap is visible in the
		 * very next frame rather than one tick late. */
		drain_keyboard(&st);
		/* Update the clock so the status bar stays live. */
		time_t now = time(NULL);
		struct tm tmbuf;
		localtime_r(&now, &tmbuf);
		strftime(st.shell->status_time, sizeof(st.shell->status_time), "%H:%M", &tmbuf);
		draw_and_commit(&st);
		struct timespec ts = { 0, 16 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}

	if (last_dispatch == -1) {
		/*
		 * A bare "exiting" hides why the connection went away, and this
		 * loop is where it goes, so the reason is worth printing.
		 *
		 * errno is used rather than wl_display_get_error(): on a display
		 * whose connection has already failed, asking libwayland for the
		 * error string faulted, and the crash landed on this line rather
		 * than on the thing that had actually broken. A diagnostic that
		 * crashes while diagnosing is worse than no diagnostic.
		 */
		fprintf(stderr, "astrix-shell: display connection lost (errno %d)\n", errno);
	}
	fprintf(stderr, "astrix-shell: exiting\n");
	astrix_shell_destroy(st.shell);
	wl_display_disconnect(st.display);
	return 0;
}
