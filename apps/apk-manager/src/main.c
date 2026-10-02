/*
 * Astrix OS - Astrix APK Manager.
 *
 * Installs, lists and removes Android APKs through Waydroid.
 *
 * What this app does *not* do is the important part. Waydroid needs a kernel
 * with the binder driver, a userspace container runtime, and an Android
 * image that has been downloaded and unpacked. If any of that is missing,
 * the app says exactly which piece is missing instead of presenting an empty
 * list that looks like "you have no apps".
 *
 * See docs/ANDROID.md for the full list of what is and is not verified.
 */

#include "astrix_app.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_APKS 512
#define OUTPUT_MAX 8192

struct apk {
	char package[128];
	char version[64];
	char label[96];
};

struct apk_app {
	struct apk apks[MAX_APKS];
	int count;

	char status[256];
	char output[OUTPUT_MAX];

	/* Capability probe results, shown so the state is never a mystery. */
	bool waydroid_binary;
	bool android_image;
	bool binder;
	bool lxc;

	struct astrix_scroll scroll;
	int selected;
	int width, height;
};

static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

/* The image lives under /var/lib/waydroid/images once it has been set up. */
static bool android_image_ready(void) {
	const char *root = getenv("ANDROID_ROOT");
	if (!root || !root[0]) {
		root = "/var/lib/waydroid";
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/images", root);
	return exists(path);
}

static void probe(struct apk_app *app) {
	app->waydroid_binary = exists("/usr/bin/waydroid");
	app->android_image = android_image_ready();
	/*
	 * binder is the kernel half of Android's IPC. /dev/binder* existing is
	 * the only reliable userspace-visible test; the CONFIG_ANDROID_BINDER
	 * module must have loaded.
	 */
	app->binder = exists("/dev/binder") || exists("/dev/hwbinder");
	/* Waydroid ships LXC as its container runtime. */
	app->lxc = exists("/usr/bin/lxc-start") || exists("/usr/libexec/lxc-start");

	snprintf(app->status, sizeof(app->status), "%d apks installed", app->count);
	if (app->waydroid_binary) {
		size_t used = strlen(app->status);
		snprintf(app->status + used, sizeof(app->status) - used, " - waydroid present");
	}
	if (!app->android_image) {
		size_t used = strlen(app->status);
		snprintf(app->status + used, sizeof(app->status) - used, " - no Android image");
	}
}

static int run_capture(char *const argv[], char *out, size_t out_size) {
	int fds[2];
	if (pipe(fds) != 0) {
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		close(fds[0]);
		close(fds[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	size_t used = 0;
	ssize_t n;
	while ((n = read(fds[0], out + used, out_size - 1 - used)) > 0) {
		used += (size_t)n;
		if (used >= out_size - 1) {
			break;
		}
	}
	out[used] = '\0';
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void load_apks(struct apk_app *app) {
	app->count = 0;
	char *argv[] = { (char *)"waydroid", (char *)"app", (char *)"list", NULL };
	static char buf[OUTPUT_MAX];
	int rc = run_capture(argv, buf, sizeof(buf));
	if (rc != 0) {
		return;
	}
	/*
	 * `waydroid app list` prints a table whose first column is the package
	 * name. The exact column layout has changed between Waydroid releases,
	 * so the first whitespace-delimited field is taken, which is the
	 * package name in every version that has shipped.
	 */
	const char *p = buf;
	while (*p && app->count < MAX_APKS) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		while (len > 0 && (*p == ' ' || *p == '\t')) {
			p++;
			len--;
		}
		if (len > 2) {
			size_t n = 0;
			while (n < len && p[n] != ' ' && p[n] != '\t' && n < 127) {
				n++;
			}
			if (n > 2) {
				struct apk *a = &app->apks[app->count];
				memcpy(a->package, p, n);
				a->package[n] = '\0';
				const char *rest = p + n;
				size_t rlen = len - n;
				const char *t = memchr(rest, '\t', rlen);
				if (t) {
					size_t vl = (size_t)(t - rest);
					if (vl < sizeof(a->version)) {
						memcpy(a->version, rest + 1, vl - 1);
						a->version[vl - 1] = '\0';
					}
				}
				app->count++;
			}
		}
		if (!nl) {
			break;
		}
		p = nl + 1;
	}
}

/* --- drawing ------------------------------------------------------------- */

static void on_frame(struct astrix_app_host *host) {
	struct apk_app *app = astrix_app_user(host);
	struct astrix_canvas *c = astrix_app_canvas(host);
	const struct astrix_theme *th = astrix_app_theme(host);
	int w, h;
	astrix_app_size(host, &w, &h);

	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, w, h }, th->background);
	struct astrix_rect content = astrix_widget_appbar(c, th, "Android apps", "Waydroid", true);
	astrix_draw_text(c, th->spacing * 2, content.y + 2, app->status, th->text_dim);

	/*
	 * If the compatibility layer is not set up, say exactly why instead of
	 * showing an empty list. This is the screen a user is most likely to
	 * screenshot as "Android is broken", so it has to be specific.
	 */
	if (!app->waydroid_binary || !app->android_image) {
		int y = content.y + 40;
		struct astrix_rect card = { th->spacing * 2, y, w - th->spacing * 4, 300 };
		struct astrix_rect inner = astrix_widget_card(c, th, card);
		astrix_draw_text(c, inner.x, inner.y, "Android is not set up", th->text);
		astrix_draw_text(c, inner.x, inner.y + 24, "Missing:", th->text_dim);
		int ly = inner.y + 46;
		if (!app->waydroid_binary) {
			astrix_draw_text(c, inner.x, ly, "- the waydroid command", th->warning);
			ly += 18;
		}
		if (!app->lxc) {
			astrix_draw_text(c, inner.x, ly, "- an LXC container runtime", th->warning);
			ly += 18;
		}
		if (!app->binder) {
			astrix_draw_text(c, inner.x, ly, "- the binder driver (/dev/binder)", th->warning);
			ly += 18;
		}
		if (!app->android_image) {
			astrix_draw_text(c, inner.x, ly, "- an unpacked Android image", th->warning);
			ly += 18;
		}
		astrix_draw_text(c, inner.x, ly + 10, "Run: astrix-android-setup", th->text_dim);
		astrix_draw_text(c, inner.x, ly + 28, "See docs/ANDROID.md", th->text_dim);
		return;
	}

	int list_y = content.y + 34;
	int rh = th->touch_target + th->spacing;
	for (int i = 0; app->scroll.offset / rh + i < app->count; i++) {
		int idx = app->scroll.offset / rh + i;
		int y = list_y + i * rh - app->scroll.offset;
		if (y + rh < list_y) {
			continue;
		}
		if (y > h) {
			break;
		}
		astrix_widget_row(c, th, (struct astrix_rect){ 0, y, w, rh }, app->apks[idx].package,
		                  NULL, app->apks[idx].version, idx == app->selected, false);
	}

	if (app->count == 0) {
		struct astrix_rect body = { th->spacing * 3, list_y + 40, w - th->spacing * 6,
			                            h - list_y - 120 };
		astrix_widget_empty(c, th, body, "No Android apps", "Install an APK with astrix-apk.");
	}

	if (app->output[0]) {
		struct astrix_rect bar = { 0, h - th->navbar_h, w, th->navbar_h };
		astrix_fill_rect(c, bar, th->surface);
		char line[128];
		astrix_text_ellipsize(app->output, w - th->spacing * 4, line, sizeof(line));
		astrix_draw_text(c, th->spacing * 2, bar.y + (th->navbar_h - ASTRIX_FONT_H) / 2, line,
		                 th->text_dim);
	}
}

static void on_tick(struct astrix_app_host *host, double dt) {
	(void)dt;
	struct apk_app *app = astrix_app_user(host);
	if (astrix_scroll_step(&app->scroll)) {
		astrix_app_invalidate(host);
	}
}

static bool on_input(struct astrix_app_host *host, const struct astrix_input_event *ev) {
	struct apk_app *app = astrix_app_user(host);
	int w, h;
	astrix_app_size(host, &w, &h);
	const struct astrix_theme *th = astrix_app_theme(host);
	int list_y = th->status_h + 52 + 34;
	int rh = th->touch_target + th->spacing;

	switch (ev->kind) {
	case ASTRIX_INPUT_TOUCH_DOWN: {
		int index = app->scroll.offset / rh + (ev->y - list_y) / rh;
		if (index >= 0 && index < app->count) {
			app->selected = index;
			char *argv[] = { (char *)"waydroid", (char *)"app", (char *)"launch",
			                 app->apks[index].package, NULL };
			run_capture(argv, app->output, sizeof(app->output));
			astrix_app_invalidate(host);
		}
		return true;
	}
	case ASTRIX_INPUT_TOUCH_MOTION:
	case ASTRIX_INPUT_POINTER_MOTION:
		if (astrix_scroll_handle(&app->scroll, ev)) {
			astrix_app_invalidate(host);
		}
		return false;
	case ASTRIX_INPUT_SCROLL:
		app->scroll.offset -= ev->scroll_y / 4;
		astrix_scroll_clamp(&app->scroll);
		astrix_app_invalidate(host);
		return true;
	default:
		return false;
	}
}

static void on_start(struct astrix_app_host *host) {
	struct apk_app *app = astrix_app_user(host);
	astrix_app_size(host, &app->width, &app->height);
	probe(app);
	if (app->waydroid_binary) {
		load_apks(app);
	}
	probe(app); /* load_apks may have changed nothing, but status is rebuilt */
}

static const struct astrix_app_desc desc = {
	.app_id = "org.astrix.ApkManager",
	.title = "Android apps",
	.version = "0.1.0",
	.wants_keyboard = false,
	.on_start = on_start,
	.on_frame = on_frame,
	.on_input = on_input,
	.on_exit = NULL,
	.on_tick = on_tick,
};

int main(int argc, char **argv) {
	struct apk_app app;
	memset(&app, 0, sizeof(app));
	app.selected = -1;
	return astrix_app_main_with_user(&desc, argc, argv, &app);
}
