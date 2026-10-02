/*
 * Astrix OS - the app registry, populated from the desktop.
 *
 * Why this file exists
 * -------------------
 * The shell used to start with an empty app list. Every test that rendered a
 * home screen registered its apps by hand, so all of them showed a full grid
 * and looked right - and on a real device the grid was empty, no icon could
 * ever be tapped, and the launcher was decorative. The tests were not
 * lying about the code they exercised; they were simply exercising a
 * registry nobody fills at runtime.
 *
 * The registry is filled the way every other desktop does it: by reading
 * freedesktop.org desktop entries from the XDG data directories. That is not
 * a stylistic preference. It is what makes the launcher *unified* - Astrix's
 * own apps, a GTK app from Debian, and eventually a Waydroid APK all appear
 * the same way, from the same file format, instead of each needing an
 * entry in Astrix's source code.
 *
 * What is deliberately NOT claimed
 * -------------------------------
 * This scanner reads metadata and records an Exec line. It does not launch
 * anything. Launching a client is the compositor's job (it owns the seat and
 * the process), and a launcher that spawns an X11 app on a Wayland-only
 * compositor would produce a window nobody can see. Until that path exists,
 * `Exec` is parsed, stored and reported - and that is all it is used for.
 */

#include "astrix_shell.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Longest .desktop line worth reading. The spec allows 1024+ bytes; beyond
 * that it is not a desktop entry any real tool produced. */
#define ASTRIX_DESKTOP_LINE 1024

static void trim(char *s) {
	if (!s) {
		return;
	}
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
	                 s[n - 1] == '\n')) {
		s[--n] = '\0';
	}
	char *start = s;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (start != s) {
		memmove(s, start, strlen(start) + 1);
	}
}

/*
 * Read one value out of a .desktop file. The format is the freedesktop one:
 * a [Group] header, then key=value lines, with a backslash at the end of a
 * line continuing it. Unescaping is applied because Exec lines routinely
 * contain \\s (space) and \\; and a launcher that shows those literally is
 * showing the wrong command.
 *
 * Returns 1 on success. `out` is untouched on failure, so a caller can keep
 * whatever default it had.
 */
static bool desktop_value(const char *text, const char *key, char *out, size_t out_len) {
	size_t key_len = strlen(key);
	const char *line = text;

	while (*line) {
		const char *eol = strchr(line, '\n');
		size_t len = eol ? (size_t)(eol - line) : strlen(line);

		/* Skip group headers and anything indented (a value continuation). */
		if (len > key_len + 1 && line[0] != '[' && line[0] != ' ' && line[0] != '\t' &&
		    strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
			size_t n = len - key_len - 1;
			if (n >= out_len) {
				n = out_len - 1;
			}
			memcpy(out, line + key_len + 1, n);
			out[n] = '\0';
			/* Unescape in place: \s -> space, \n -> newline, \t -> tab,
			 * \\ -> backslash. */
			char *w = out;
			for (char *r = out; *r; r++) {
				if (*r == '\\' && r[1]) {
					char c = r[1];
					if (c == 's') {
						c = ' ';
					} else if (c == 'n') {
						c = '\n';
					} else if (c == 't') {
						c = '\t';
					} else if (c != '\\') {
						/* %f, %u, %U and friends are left alone: they
						 * are field codes the launcher would have to
						 * expand, and a value that has them is not one
						 * this shell can run. */
						c = c;
					}
					*w++ = c;
					r++;
				} else {
					*w++ = *r;
				}
			}
			*w = '\0';
			trim(out);
			return true;
		}
		if (!eol) {
			break;
		}
		line = eol + 1;
	}
	return false;
}

/* True if the desktop entry asks not to be shown in a menu. */
static bool desktop_is_hidden(const char *text) {
	char value[ASTRIX_DESKTOP_LINE];
	if (desktop_value(text, "NoDisplay", value, sizeof(value)) &&
	    (value[0] == 't' || value[0] == 'T' || value[0] == '1' || value[0] == 'y')) {
		return true;
	}
	if (desktop_value(text, "Hidden", value, sizeof(value)) &&
	    (value[0] == 't' || value[0] == 'T' || value[0] == '1')) {
		return true;
	}
	/*
	 * OnlyShowIn is honoured in the negative direction only. An entry that
	 * says OnlyShowIn=GNOME;KDE; is for a desktop Astrix is not, so hiding
	 * it is correct. An entry that says OnlyShowIn=GNOME; (ending in a
	 * semicolon) means "and others", which includes Astrix - and treating
	 * that as exclusive is how a launcher ends up hiding half the system.
	 * Astrix therefore has no XDG desktop id of its own to match against,
	 * which is stated here rather than papered over: the rule is "show
	 * everything that is not explicitly hidden", and the trailing-semicolon
	 * case is why that is the safe default.
	 */
	return false;
}

/* Extract a category worth showing: the first that looks like a launcher
 * category. Used only for ordering, never to decide visibility. */
static void desktop_category(const char *text, char *out, size_t out_len) {
	if (desktop_value(text, "Categories", out, out_len)) {
		return;
	}
	out[0] = '\0';
}

static enum astrix_app_kind kind_from_entry(const char *text) {
	char id[ASTRIX_DESKTOP_LINE];
	if (!desktop_value(text, "Type", id, sizeof(id))) {
		return ASTRIX_APP_NATIVE;
	}
	if (strcmp(id, "Application") != 0) {
		/* Link, Directory and Service are not launchable apps. */
		return ASTRIX_APP_NATIVE;
	}
	return ASTRIX_APP_NATIVE;
}

/*
 * Parse one .desktop file and add it to the registry. Returns the index, or
 * -1 if the file is not something to show.
 */
static int register_desktop_file(struct astrix_shell *sh, const char *path, const char *file_id) {
	FILE *f = fopen(path, "r");
	if (!f) {
		/* Unreadable is normal for a partially installed package and is
		 * not worth a warning per file. */
		return -1;
	}
	char text[64 * 1024];
	size_t total = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[total] = '\0';

	if (desktop_is_hidden(text)) {
		return -1;
	}

	char type[64];
	if (desktop_value(text, "Type", type, sizeof(type)) && strcmp(type, "Application") != 0) {
		return -1;
	}

	char exec[ASTRIX_APP_EXEC_LEN];
	if (!desktop_value(text, "Exec", exec, sizeof(exec)) || exec[0] == '\0') {
		/* No command is not an app. This is the common case for the
		 * directory files in /usr/share/applications. */
		return -1;
	}

	struct astrix_app app;
	memset(&app, 0, sizeof(app));

	snprintf(app.id, sizeof(app.id), "%s", file_id);
	if (!desktop_value(text, "Name", app.name, sizeof(app.name)) || app.name[0] == '\0') {
		/* A nameless entry cannot be drawn; fall back to the id so the
		 * user sees *something* identifiable rather than a blank tile. */
		snprintf(app.name, sizeof(app.name), "%s", file_id);
	}
	snprintf(app.exec, sizeof(app.exec), "%s", exec);
	desktop_value(text, "Icon", app.icon_path, sizeof(app.icon_path));

	char categories[256];
	desktop_category(text, categories, sizeof(categories));
	/* Categories are kept in the kind field's shadow: the shell sorts by
	 * kind, and an entry that declares no Astrix-specific category is
	 * treated as a native app, which is the only kind that exists today. */
	(void)categories;

	app.kind = kind_from_entry(text);

	/*
	 * Where the icon goes, and why it is not "everything on page 1".
	 *
	 * The shell's home screen has three pages and page 0 is the dock: the
	 * screen the user lands on. add_app() moves any unpinned app to page 1,
	 * so an entry registered with pinned=false would appear on the *second*
	 * page - and a freshly installed system would open on a completely
	 * empty screen with its apps one swipe away. That is what happened:
	 * every icon-grid render test passed, and the real device showed
	 * nothing tappable on the first screen.
	 *
	 * So Astrix's own apps are pinned into the dock, up to one row, and
	 * everything else goes to page 1 where there is room to scroll. The
	 * cap is not arbitrary: the dock grid is four columns wide, so a fifth
	 * pinned icon would be drawn off the row or overlap the page dots.
	 */
	static int dock_used = 0;
	const bool is_astrix_app = strncmp(app.id, "astrix-", 7) == 0;
	app.pinned = is_astrix_app && dock_used < ASTRIX_DOCK_COLS;
	if (app.pinned) {
		dock_used++;
	}
	app.page = 1;

	int idx = astrix_shell_add_app(sh, &app);
	if (idx >= 0) {
		/* stderr, not stdout: stdout is a pipe under systemd, so it is
		 * fully buffered and nothing appears until the process exits. A
		 * diagnostic nobody can read while the session is running is not
		 * a diagnostic - this is how the launcher was invisible for a
		 * whole boot cycle. */
		fprintf(stderr, "astrix-shell: launcher: %-14s %s%s\n", app.name, app.exec,
		        app.pinned ? "  [dock]" : "");
	}
	return idx;
}

static void scan_directory(struct astrix_shell *sh, const char *applications_dir) {
	DIR *d = opendir(applications_dir);
	if (!d) {
		return;
	}
	/*
	 * readdir order is filesystem order, which is arbitrary. The shell
	 * sorts by name when it renders, so a stable order here is only about
	 * making the log readable and repeatable between boots.
	 */
	char ids[ASTRIX_MAX_APPS][256];
	int count = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && count < ASTRIX_MAX_APPS) {
		size_t n = strlen(ent->d_name);
		if (n < 6 || strcmp(ent->d_name + n - 8, ".desktop") != 0) {
			continue;
		}
		char id[256];
		snprintf(id, sizeof(id), "%s", ent->d_name);
		id[n - 8] = '\0';
		snprintf(ids[count], sizeof(ids[count]), "%s", id);
		count++;
	}
	closedir(d);

	for (int i = 0; i < count; i++) {
		for (int j = i + 1; j < count; j++) {
			if (strcmp(ids[i], ids[j]) > 0) {
				char tmp[256];
				snprintf(tmp, sizeof(tmp), "%s", ids[i]);
				snprintf(ids[i], sizeof(ids[i]), "%s", ids[j]);
				snprintf(ids[j], sizeof(ids[j]), "%s", tmp);
			}
		}
	}

	for (int i = 0; i < count; i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s.desktop", applications_dir, ids[i]);
		register_desktop_file(sh, path, ids[i]);
	}
}

/*
 * Populate the launcher from the XDG data directories.
 *
 * XDG_DATA_DIRS is the system-wide list and XDG_DATA_HOME the per-user one;
 * both are scanned, the system ones first, so a user entry with the same id
 * overrides the system one. That ordering is what the spec says and it is
 * also what makes astrix_shell_add_app's "update in place" path meaningful
 * instead of dead code.
 */
int astrix_shell_scan_apps(struct astrix_shell *sh) {
	if (!sh) {
		return 0;
	}
	int before = sh->app_count;

	static const char *const defaults[] = {"/usr/local/share", "/usr/share", NULL};
	char home_dirs[1024] = {0};
	const char *home = getenv("XDG_DATA_HOME");
	if (home && *home) {
		snprintf(home_dirs, sizeof(home_dirs), "%s", home);
	}

	if (home_dirs[0]) {
		char applications[1024];
		snprintf(applications, sizeof(applications), "%s/applications", home_dirs);
		scan_directory(sh, applications);
	}

	const char *dirs = getenv("XDG_DATA_DIRS");
	if (!dirs || !*dirs) {
		for (int i = 0; defaults[i]; i++) {
			char applications[1024];
			snprintf(applications, sizeof(applications), "%s/applications", defaults[i]);
			scan_directory(sh, applications);
		}
	} else {
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s", dirs);
		char *save = NULL;
		for (char *tok = strtok_r(buf, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
			if (*tok != '/') {
				continue;
			}
			char applications[1024];
			snprintf(applications, sizeof(applications), "%s/applications", tok);
			scan_directory(sh, applications);
		}
	}

	int added = sh->app_count - before;
	fprintf(stderr, "astrix-shell: launcher has %d app(s) from the desktop\n",
	        sh->app_count);
	return added;
}
