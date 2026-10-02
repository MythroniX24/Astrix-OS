/*
 * Astrix OS - Astrix Packages (Debian apt).
 *
 * Lists, searches, installs and removes Debian packages from the Astrix
 * rootfs. The package database is the system's own dpkg database and the
 * work is done by apt, so what this screen shows is what apt will do.
 *
 * Privilege model
 * ---------------
 * A phone user must not be root, and an app must not be able to run an
 * arbitrary command as root. So every mutating operation goes through
 * pkexec, which uses polkit to ask for authentication. There is exactly one
 * privileged helper in the image (astrix-pkg-helper, see
 * system/astrix-pkg-helper) and it accepts a fixed set of subcommands rather
 * than a free-form command line.
 *
 * If polkit is not running, the app says so and does nothing. It does not
 * fall back to running apt directly, which would be the easy thing to do and
 * the wrong one.
 */

#include "astrix_app.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_PACKAGES 2048
#define OUTPUT_MAX 8192

struct package {
	char name[96];
	char version[48];
	char summary[128];
};

struct pkg_app {
	struct package packages[MAX_PACKAGES];
	int count;
	bool truncated;

	enum { PKG_LIST, PKG_SEARCH, PKG_BUSY, PKG_RESULT } screen;
	char status[256];
	char output[OUTPUT_MAX];

	struct astrix_scroll scroll;
	int selected;
	int pressed_row;
	bool pressed_valid;
	int width, height;
};

/* --- running apt --------------------------------------------------------- */

/*
 * Run a command, capturing its combined output. Returns the exit status, or
 * -1 if the child could not be started.
 *
 * apt is run with -y because there is no terminal to answer a prompt, and
 * with DEBIAN_FRONTEND=noninteractive so a maintainer script cannot block
 * waiting for a question that will never come.
 */
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
		setenv("DEBIAN_FRONTEND", "noninteractive", 1);
		setenv("LC_ALL", "C", 1);
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

static int polkit_available(void) {
	/* polkitd runs as a system service and exposes its authority on the
	 * system bus. If the socket is not there, no authenticated privileged
	 * operation is possible and the app must say so. */
	return access("/run/dbus/system_bus_socket", F_OK) == 0;
}

static int run_privileged(const char *verb, const char *package, char *out, size_t out_size) {
	char *argv[] = { (char *)"pkexec", (char *)"/usr/lib/astrix/astrix-pkg-helper", (char *)verb,
		             (char *)package, NULL };
	return run_capture(argv, out, out_size);
}

/* --- parsing ------------------------------------------------------------- */

static void parse_dpkg_listing(struct pkg_app *app, const char *text) {
	app->count = 0;
	app->truncated = false;
	/*
	 * dpkg-query -W -f gives "name<TAB>version<TAB>arch". Splitting on tabs
	 * rather than spaces matters because package names never contain a
	 * space, but the arch field is free-form.
	 */
	const char *p = text;
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (len > 3 && app->count < MAX_PACKAGES) {
			const char *t1 = memchr(p, '\t', len);
			if (t1) {
				size_t nlen = (size_t)(t1 - p);
				const char *rest = t1 + 1;
				size_t rlen = len - nlen - 1;
				const char *t2 = memchr(rest, '\t', rlen);
				size_t vlen = t2 ? (size_t)(t2 - rest) : rlen;
				if (nlen < sizeof(app->packages[0].name) && vlen < 64) {
					struct package *pkg = &app->packages[app->count];
					memcpy(pkg->name, p, nlen);
					pkg->name[nlen] = '\0';
					memcpy(pkg->version, rest, vlen);
					pkg->version[vlen] = '\0';
					snprintf(pkg->summary, sizeof(pkg->summary), "%.127s", "installed");
					app->count++;
				}
			}
		} else if (len > 3) {
			app->truncated = true;
		}
		if (!nl) {
			break;
		}
		p = nl + 1;
	}
}

static void load_installed(struct pkg_app *app) {
	static char buf[OUTPUT_MAX];
	char *argv[] = { (char *)"dpkg-query", (char *)"-W", (char *)"-f",
		             (char *)"${Package}\t${Version}\t${Architecture}\n", NULL };
	int rc = run_capture(argv, buf, sizeof(buf));
	if (rc != 0) {
		snprintf(app->status, sizeof(app->status), "dpkg-query failed (%d)", rc);
		return;
	}
	parse_dpkg_listing(app, buf);
	snprintf(app->status, sizeof(app->status), "%d packages installed%s", app->count,
	         app->truncated ? "+" : "");
}

/* --- drawing ------------------------------------------------------------- */

static int row_height(const struct astrix_theme *th) {
	return th->touch_target + th->spacing;
}

static void on_frame(struct astrix_app_host *host) {
	struct pkg_app *app = astrix_app_user(host);
	struct astrix_canvas *c = astrix_app_canvas(host);
	const struct astrix_theme *th = astrix_app_theme(host);
	int w, h;
	astrix_app_size(host, &w, &h);

	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, w, h }, th->background);
	struct astrix_rect content = astrix_widget_appbar(c, th, "Packages", "Debian apt", true);

	astrix_draw_text(c, th->spacing * 2, content.y + 2, app->status, th->text_dim);

	if (app->screen == PKG_RESULT || app->screen == PKG_BUSY) {
		int y = content.y + 26 - app->scroll.offset;
		const char *p = app->output;
		while (*p && y < h) {
			const char *nl = strchr(p, '\n');
			size_t len = nl ? (size_t)(nl - p) : strlen(p);
			char line[256];
			size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
			memcpy(line, p, n);
			line[n] = '\0';
			astrix_draw_text(c, th->spacing * 2, y, line, th->text);
			y += ASTRIX_FONT_H + 3;
			if (!nl) {
				break;
			}
			p = nl + 1;
		}
		return;
	}

	int list_y = content.y + 26;
	int rh = row_height(th);
	int first = app->scroll.offset / rh;
	if (first < 0) {
		first = 0;
	}
	for (int i = first; i < app->count; i++) {
		int y = list_y + i * rh - app->scroll.offset;
		if (y + rh < list_y) {
			continue;
		}
		if (y > h) {
			break;
		}
		struct astrix_rect r = { 0, y, w, rh };
		astrix_widget_row(c, th, r, app->packages[i].name, NULL, app->packages[i].version,
		                  i == app->selected,
		                  app->pressed_valid && app->pressed_row == i);
	}

	/* Action bar. */
	int bar_h = th->navbar_h + 8;
	struct astrix_rect bar = { 0, h - bar_h, w, bar_h };
	astrix_fill_rect(c, bar, th->surface);
	astrix_fill_rect(c, (struct astrix_rect){ 0, bar.y, w, 1 }, th->divider);
	int bw = (w - th->spacing * 6) / 2;
	astrix_widget_button(c, th, (struct astrix_rect){ th->spacing * 2, bar.y + 8, bw, 44 },
	                     "Details", 0, false);
	astrix_widget_button(c, th, (struct astrix_rect){ th->spacing * 2 + bw + 8, bar.y + 8, bw, 44 },
	                     "Remove", 2, false);
}

static void on_tick(struct astrix_app_host *host, double dt) {
	(void)dt;
	struct pkg_app *app = astrix_app_user(host);
	if (astrix_scroll_step(&app->scroll)) {
		astrix_app_invalidate(host);
	}
}

static bool on_input(struct astrix_app_host *host, const struct astrix_input_event *ev) {
	struct pkg_app *app = astrix_app_user(host);
	int w, h;
	astrix_app_size(host, &w, &h);
	const struct astrix_theme *th = astrix_app_theme(host);
	int rh = row_height(th);
	int list_y = th->status_h + 52 + 26;
	int bar_h = th->navbar_h + 8;

	switch (ev->kind) {
	case ASTRIX_INPUT_TOUCH_DOWN: {
		app->pressed_valid = false;
		if (app->screen == PKG_RESULT || app->screen == PKG_BUSY) {
			app->screen = PKG_LIST;
			astrix_app_invalidate(host);
			return true;
		}
		if (ev->y >= h - bar_h) {
			int bw = (w - th->spacing * 6) / 2;
			int slot = (ev->x - th->spacing * 2) / (bw + 8);
			if (slot == 0) {
				/* Details: the package's own description. */
				if (app->selected < 0 || app->selected >= app->count) {
					astrix_app_toast(host, "nothing selected");
					return true;
				}
				char *argv[] = { (char *)"apt-cache", (char *)"show",
				                 app->packages[app->selected].name, NULL };
				run_capture(argv, app->output, sizeof(app->output));
				app->screen = PKG_RESULT;
			} else {
				if (app->selected < 0 || app->selected >= app->count) {
					astrix_app_toast(host, "nothing selected");
					return true;
				}
				if (!polkit_available()) {
					astrix_app_toast(host, "polkit is not running - cannot remove");
					return true;
				}
				app->screen = PKG_BUSY;
				snprintf(app->status, sizeof(app->status), "removing %s...",
				         app->packages[app->selected].name);
				astrix_app_invalidate(host);
				int rc = run_privileged("remove", app->packages[app->selected].name,
				                        app->output, sizeof(app->output));
				app->screen = PKG_RESULT;
				snprintf(app->status, sizeof(app->status), "remove exited %d", rc);
				astrix_app_toast(host, rc == 0 ? "removed" : "remove failed");
				load_installed(app);
			}
			astrix_app_invalidate(host);
			return true;
		}
		int index = app->scroll.offset / rh + (ev->y - list_y) / rh;
		if (index >= 0 && index < app->count) {
			app->selected = index;
			app->pressed_row = index;
			app->pressed_valid = true;
		}
		return true;
	}

	case ASTRIX_INPUT_TOUCH_MOTION:
	case ASTRIX_INPUT_POINTER_MOTION:
		if (astrix_scroll_handle(&app->scroll, ev)) {
			app->pressed_valid = false;
			astrix_app_invalidate(host);
		}
		return false;

	case ASTRIX_INPUT_TOUCH_UP:
	case ASTRIX_INPUT_SCROLL:
		if (ev->kind == ASTRIX_INPUT_SCROLL) {
			app->scroll.offset -= ev->scroll_y / 4;
			astrix_scroll_clamp(&app->scroll);
		}
		app->pressed_valid = false;
		astrix_app_invalidate(host);
		return true;

	default:
		return false;
	}
}

static void on_start(struct astrix_app_host *host) {
	struct pkg_app *app = astrix_app_user(host);
	astrix_app_size(host, &app->width, &app->height);
	load_installed(app);
	if (!polkit_available()) {
		snprintf(app->status + strlen(app->status),
		         sizeof(app->status) - strlen(app->status), " - polkit not running (read-only)");
	}
}

static const struct astrix_app_desc desc = {
	.app_id = "org.astrix.Packages",
	.title = "Packages",
	.version = "0.1.0",
	.wants_keyboard = false,
	.on_start = on_start,
	.on_frame = on_frame,
	.on_input = on_input,
	.on_exit = NULL,
	.on_tick = on_tick,
};

int main(int argc, char **argv) {
	struct pkg_app app;
	memset(&app, 0, sizeof(app));
	app.selected = -1;
	app.pressed_row = -1;
	app.screen = PKG_LIST;
	return astrix_app_main_with_user(&desc, argc, argv, &app);
}
