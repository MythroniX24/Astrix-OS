/*
 * Astrix OS - Astrix Shell core.
 *
 * Owns the shell's state (screens, apps, notifications, status bar contents)
 * and the app registry. Rendering lives in render.c; gesture interpretation
 * lives in gesture.c. This file is the model both operate on.
 *
 * The app registry is the piece that makes native and Android apps feel like
 * one system: an app is identified only by its id, and the launcher, home
 * screen and app switcher all iterate the same list without caring whether an
 * entry launches an ELF binary or asks the compatibility layer to start an
 * Android activity.
 */

#define _POSIX_C_SOURCE 200809L
#include "astrix_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- helpers ------------------------------------------------------------- */

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *xcalloc(size_t n, size_t sz) {
	void *p = calloc(n, sz);
	if (!p) {
		fprintf(stderr, "astrix-shell: out of memory\n");
		exit(1);
	}
	return p;
}

/* --- lifecycle ----------------------------------------------------------- */

struct astrix_shell *astrix_shell_create(int width, int height) {
	if (width <= 0) {
		width = 720;
	}
	if (height <= 0) {
		height = 1600;
	}
	struct astrix_shell *sh = xcalloc(1, sizeof(*sh));
	/* The shell allocates its own framebuffer until a Wayland host aliases
	 * one onto it (see astrix_shell_set_size). */
	sh->pixels_owned = true;
	sh->width = width;
	sh->height = height;
	sh->foreground_app = -1;
	sh->launch_pending = -1;
	sh->kill_pending = -1;
	sh->kill_pending_pid = 0;
	sh->home_page = 0;
	sh->dark_mode = true;
	sh->theme = astrix_theme_dark();
	sh->screen = ASTRIX_SCREEN_HOME;
	sh->locked = false;
	sh->battery_percent = -1;   /* -1 = unknown; never show a fake value */
	sh->uptime_s = 0;
	sh->load_avg = 0.0f;
	sh->memory_used_kb = 0;
	sh->memory_total_kb = 0;
	sh->has_focus = true;
	/* No key is held, and the keyboard starts closed. */
	sh->kbd_press_x = -1;
	sh->kbd_press_y = -1;
	sh->kbd_visible = false;
	snprintf(sh->status_time, sizeof(sh->status_time), "--:--");
	snprintf(sh->status_battery, sizeof(sh->status_battery), "--%%");

	astrix_gesture_config_defaults(&sh->gestures.cfg);
	astrix_gesture_init(&sh->gestures, &sh->gestures.cfg);

	astrix_shell_set_size(sh, width, height);
	return sh;
}

void astrix_shell_set_size(struct astrix_shell *shell, int width, int height) {
	if (!shell || width <= 0 || height <= 0) {
		return;
	}
	shell->width = width;
	shell->height = height;
	/*
	 * The framebuffer is reallocated on resize. Doing this here (rather
	 * than in the draw path) keeps render functions free of allocation.
	 *
	 * Only an owned framebuffer is reallocated. When a Wayland host has
	 * aliased `pixels` onto its own wl_shm mapping, that mapping is the
	 * host's to manage: this function must leave it alone and let the
	 * host re-alias after it has created the new buffers. Freeing it here
	 * is free() on an mmap'd pointer (and, in the resize path, on memory
	 * that was already unmapped) - a SIGSEGV that only appeared once the
	 * shell ran on a panel whose size differed from the build's default.
	 */
	size_t need = (size_t)width * (size_t)height * sizeof(uint32_t);
	if (shell->pixels_owned && need != shell->buffer_size) {
		free(shell->pixels);
		shell->pixels = xcalloc(need / sizeof(uint32_t), sizeof(uint32_t));
		shell->buffer_size = need;
	}
	shell->stride = width;
	shell->canvas.pixels = shell->pixels;
	shell->canvas.width = width;
	shell->canvas.height = height;
	shell->canvas.stride = width;

	/*
	 * Scale the gesture thresholds with the panel so a swipe feels the same
	 * on a small phone and a tablet. These are derived from a reference
	 * 720x1600 screen.
	 */
	float scale = (float)width / 720.0f;
	if (scale < 0.75f) {
		scale = 0.75f;
	}
	if (scale > 2.0f) {
		scale = 2.0f;
	}
	astrix_gesture_config_defaults(&shell->gestures.cfg);
	shell->gestures.cfg.swipe_threshold_px = (int)(shell->gestures.cfg.swipe_threshold_px * scale);
	shell->gestures.cfg.edge_width_px = (int)(shell->gestures.cfg.edge_width_px * scale);
	shell->gestures.cfg.touch_slop_px = (int)(shell->gestures.cfg.touch_slop_px * scale);
	shell->gestures.cfg.long_press_slop_px =
	    (int)(shell->gestures.cfg.long_press_slop_px * scale);
	shell->needs_redraw = true;
}

void astrix_shell_destroy(struct astrix_shell *shell) {
	if (!shell) {
		return;
	}
	/* An aliased framebuffer belongs to the Wayland host, not to us. */
	if (shell->pixels_owned) {
		free(shell->pixels);
	}
	free(shell);
}

/* --- app registry -------------------------------------------------------- */

int astrix_shell_add_app(struct astrix_shell *shell, const struct astrix_app *app) {
	if (!shell || !app) {
		return -1;
	}
	if (shell->app_count >= ASTRIX_MAX_APPS) {
		fprintf(stderr, "astrix-shell: app registry full, ignoring '%s'\n", app->id);
		return -1;
	}
	/* An app with a duplicate id is a rediscovery, not a new app: update in
	 * place so an app reinstalled with new metadata does not appear twice
	 * in the launcher. */
	for (int i = 0; i < shell->app_count; i++) {
		if (strcmp(shell->apps[i].id, app->id) == 0) {
			/*
			 * Keep the process state. Refreshing an app's metadata
			 * (a reinstall, a changed desktop entry) must not make
			 * a running app lose its pid: with the pid gone the app
			 * could never be terminated, and the switcher would
			 * show it as running forever.
			 */
			bool running = shell->apps[i].running;
			int pid = shell->apps[i].pid;
			shell->apps[i] = *app;
			shell->apps[i].running = running;
			shell->apps[i].pid = pid;
			shell->needs_redraw = true;
			return i;
		}
	}

	int idx = shell->app_count++;
	shell->apps[idx] = *app;
	shell->apps[idx].running = false;
	if (shell->apps[idx].page == 0 && !shell->apps[idx].pinned) {
		shell->apps[idx].page = 1;
	}
	shell->needs_redraw = true;
	return idx;
}

static int app_kind_rank(const struct astrix_app *a) {
	/* Sort: system first, then native, then Android, each alphabetically.
	 * The user should not be able to tell which runtime an app uses from
	 * its position, so this is a stable, boring ordering. */
	switch (a->kind) {
	case ASTRIX_APP_SYSTEM: return 0;
	case ASTRIX_APP_NATIVE: return 1;
	case ASTRIX_APP_ANDROID: return 2;
	}
	return 3;
}

void astrix_shell_sort_apps(struct astrix_shell *shell) {
	if (!shell) {
		return;
	}
	/* Simple insertion sort: the registry is tiny (<64) and this avoids a
	 * dependency while keeping the ordering stable and obvious. */
	for (int i = 1; i < shell->app_count; i++) {
		struct astrix_app key = shell->apps[i];
		int j = i - 1;
		while (j >= 0) {
			int a = app_kind_rank(&shell->apps[j]);
			int b = app_kind_rank(&key);
			if (a < b || (a == b && strcmp(shell->apps[j].name, key.name) <= 0)) {
				break;
			}
			shell->apps[j + 1] = shell->apps[j];
			j--;
		}
		shell->apps[j + 1] = key;
	}
	shell->needs_redraw = true;
}

/* --- notifications ------------------------------------------------------- */

int astrix_shell_notify(struct astrix_shell *shell, const char *app_id, const char *app_name,
                        const char *title, const char *body,
                        enum astrix_notification_priority prio) {
	if (!shell) {
		return -1;
	}
	if (shell->notification_count >= ASTRIX_MAX_NOTIFICATIONS) {
		/* Drop the oldest low/normal notification rather than the newest
		 * one; a phone should surface what just happened. */
		int victim = -1;
		for (int i = 0; i < shell->notification_count; i++) {
			if (shell->notifications[i].priority <= ASTRIX_NOTIF_NORMAL) {
				victim = i;
				break;
			}
		}
		if (victim < 0) {
			return -1; /* all critical: refuse rather than lose one */
		}
		for (int i = victim; i < shell->notification_count - 1; i++) {
			shell->notifications[i] = shell->notifications[i + 1];
		}
		shell->notification_count--;
	}
	int idx = shell->notification_count++;
	struct astrix_notification *n = &shell->notifications[idx];
	memset(n, 0, sizeof(*n));
	snprintf(n->id, sizeof(n->id), "%s-%lld", app_id ? app_id : "astrix", (long long)now_ms());
	snprintf(n->app_id, sizeof(n->app_id), "%s", app_id ? app_id : "");
	snprintf(n->app_name, sizeof(n->app_name), "%s", app_name ? app_name : "System");
	snprintf(n->title, sizeof(n->title), "%s", title ? title : "");
	snprintf(n->body, sizeof(n->body), "%s", body ? body : "");
	n->priority = prio;
	n->timestamp_ms = now_ms();
	shell->needs_redraw = true;
	return idx;
}

void astrix_shell_dismiss_notification(struct astrix_shell *shell, int index) {
	if (!shell || index < 0 || index >= shell->notification_count) {
		return;
	}
	for (int i = index; i < shell->notification_count - 1; i++) {
		shell->notifications[i] = shell->notifications[i + 1];
	}
	shell->notification_count--;
	if (shell->notification_scroll > shell->notification_count) {
		shell->notification_scroll = shell->notification_count;
	}
	shell->needs_redraw = true;
}

void astrix_shell_clear_notifications(struct astrix_shell *shell) {
	if (!shell) {
		return;
	}
	shell->notification_count = 0;
	shell->notification_scroll = 0;
	shell->needs_redraw = true;
}

int astrix_shell_unread_count(const struct astrix_shell *shell) {
	if (!shell) {
		return 0;
	}
	int n = 0;
	for (int i = 0; i < shell->notification_count; i++) {
		if (!shell->notifications[i].dismissed) {
			n++;
		}
	}
	return n;
}

/* --- navigation ---------------------------------------------------------- */

void astrix_shell_go_home(struct astrix_shell *shell) {
	if (!shell) {
		return;
	}
	shell->screen = ASTRIX_SCREEN_HOME;
	shell->status_shade_open = false;
	shell->shade_progress = 0.0f;
	shell->needs_redraw = true;
}

void astrix_shell_open_app(struct astrix_shell *shell, int index) {
	if (!shell || index < 0 || index >= shell->app_count) {
		return;
	}
	/*
	 * Logged because "the app opened" is the single most useful line for
	 * judging whether touch works at all: a tap that lands on an icon and
	 * produces this line has gone through the hardware device, libinput,
	 * the compositor's forwarding, the shell's gesture recogniser and the
	 * hit test, in that order.
	 */
	fprintf(stderr, "astrix-shell: opening app '%s'\n", shell->apps[index].name);
	shell->foreground_app = index;
	/*
	 * `running` is not set here. It is set when a pid is recorded
	 * (astrix_shell_set_app_pid), because until then nothing has been
	 * started and claiming otherwise is what made a failed launch look
	 * like a successful one. An app that is already running is simply
	 * brought to the front.
	 */
	/*
	 * Record the request; the caller that owns the display connection
	 * consumes it and starts the process. Until that happens "opening an
	 * app" only changed a variable, and nothing on screen ever moved.
	 */
	shell->launch_pending = index;
	shell->screen = ASTRIX_SCREEN_APP;
	shell->status_shade_open = false;
	shell->needs_redraw = true;
}

void astrix_shell_close_app(struct astrix_shell *shell, int index) {
	if (!shell || index < 0 || index >= shell->app_count) {
		return;
	}
	/*
	 * If a process is actually running, ask for it to be terminated.
	 * Clearing `running` on its own would be the same lie as before:
	 * the app would be "closed" in the UI while its process kept its
	 * surface, its Wayland client and its share of the CPU.
	 */
	if (shell->apps[index].running && shell->apps[index].pid > 0) {
		shell->kill_pending = index;
		/*
		 * Captured here, because the two lines below clear the entry's
		 * pid. Whoever sends the signal is a separate translation unit
		 * with no way to recover it afterwards, and a pid of 0 there
		 * means the app is marked closed but is still running.
		 */
		shell->kill_pending_pid = shell->apps[index].pid;
	}
	shell->apps[index].running = false;
	shell->apps[index].pid = 0;
	if (shell->launch_pending == index) {
		shell->launch_pending = -1;
	}
	if (shell->foreground_app == index) {
		shell->foreground_app = -1;
		shell->screen = ASTRIX_SCREEN_HOME;
	}
	shell->needs_redraw = true;
}

void astrix_shell_set_app_pid(struct astrix_shell *shell, int index, int pid) {
	if (!shell || index < 0 || index >= shell->app_count) {
		return;
	}
	shell->apps[index].pid = pid;
	/*
	 * `running` follows the process, not the tap. The launch may have
	 * failed (fork() refused, Exec= missing), and in that case the
	 * caller reports it and never calls this; a caller that does call
	 * it really did fork, so this is the one place the flag becomes
	 * true. Closing an app is the only other way.
	 */
	shell->apps[index].running = pid > 0;
	if (pid <= 0 && shell->foreground_app == index) {
		shell->foreground_app = -1;
		shell->screen = ASTRIX_SCREEN_HOME;
	}
	shell->needs_redraw = true;
}

bool astrix_shell_note_app_exit(struct astrix_shell *shell, int pid) {
	if (!shell || pid <= 0) {
		return false;
	}
	for (int i = 0; i < shell->app_count; i++) {
		if (shell->apps[i].pid != pid) {
			continue;
		}
		shell->apps[i].pid = 0;
		shell->apps[i].running = false;
		if (shell->launch_pending == i) {
			shell->launch_pending = -1;
		}
		if (shell->kill_pending == i) {
			shell->kill_pending = -1;
			shell->kill_pending_pid = 0;
		}
		if (shell->foreground_app == i) {
			shell->foreground_app = -1;
			shell->screen = ASTRIX_SCREEN_HOME;
		}
		shell->needs_redraw = true;
		return true;
	}
	return false;
}

void astrix_shell_toggle_lock(struct astrix_shell *shell) {
	if (!shell) {
		return;
	}
	shell->locked = !shell->locked;
	shell->screen = shell->locked ? ASTRIX_SCREEN_LOCK : ASTRIX_SCREEN_HOME;
	shell->status_shade_open = false;
	shell->needs_redraw = true;
}
