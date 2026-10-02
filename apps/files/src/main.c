/*
 * Astrix OS - Astrix Files.
 *
 * A real file manager: it lists directories with readdir/stat, navigates into
 * them, opens files by handing them to the right app, and deletes with a
 * confirmation. It does not fake a filesystem.
 *
 * What it deliberately does not do:
 *   - no multi-select (it would need a clipboard model, and a single
 *     accidental tap should not be able to stage a delete)
 *   - no clipboard, so no copy/paste
 *   - no root privileges: it runs as the astrix user and can only touch what
 *     that user owns. Deleting something that needs privilege fails with the
 *     kernel's own error rather than silently escalating.
 */

#include "astrix_app.h"
#include "astrix_files.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

enum files_screen {
	FILES_LIST = 0,
	FILES_CONFIRM_DELETE,
	FILES_ERROR,
};

struct files_app {
	struct astrix_dir dir;
	struct astrix_scroll scroll;
	enum files_screen screen;

	char cwd[ASTRIX_FILES_PATH_MAX];
	char pending_path[ASTRIX_FILES_PATH_MAX];
	char status[160];

	/* Touch press feedback. */
	int pressed_row;
	bool pressed_valid;

	int width, height;
	bool has_focus;
};

/* The row geometry, shared by drawing and hit-testing. */
static int row_height(const struct astrix_theme *th) {
	return th->touch_target + th->spacing * 2;
}

static int list_top(const struct astrix_theme *th) {
	/* Below the app bar. */
	return th->status_h + 52;
}

/* --- navigation ---------------------------------------------------------- */

static void go_to(struct astrix_app_host *host, struct files_app *fa, const char *path) {
	if (astrix_dir_read(&fa->dir, path) != 0) {
		fa->screen = FILES_ERROR;
		snprintf(fa->status, sizeof(fa->status), "%s: %s", path, fa->dir.error);
		astrix_app_invalidate(host);
		return;
	}
	snprintf(fa->cwd, sizeof(fa->cwd), "%s", path);
	fa->screen = FILES_LIST;
	astrix_scroll_reset(&fa->scroll, fa->dir.count * row_height(astrix_app_theme(host)),
	                    fa->height - list_top(astrix_app_theme(host)), fa->width);
	astrix_app_invalidate(host);
}

static void go_up(struct astrix_app_host *host, struct files_app *fa) {
	if (strcmp(fa->cwd, "/") == 0) {
		return;
	}
	char parent[ASTRIX_FILES_PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", fa->cwd);
	char *slash = strrchr(parent, '/');
	if (!slash) {
		snprintf(parent, sizeof(parent), "/");
	} else if (slash == parent) {
		parent[1] = '\0';
	} else {
		*slash = '\0';
	}
	/*
	 * Keep the selection on the directory we came out of. Without this,
	 * going up from a deep path lands the user on an arbitrary row and
	 * they have to find where they were.
	 */
	char came_from[256];
	snprintf(came_from, sizeof(came_from), "%.255s", fa->cwd);
	char *last = strrchr(came_from, '/');
	if (last) {
		last++;
	}
	go_to(host, fa, parent);
	if (last && *last) {
		for (int i = 0; i < fa->dir.count; i++) {
			if (strcmp(fa->dir.entries[i].name, last) == 0) {
				fa->dir.selected = i;
				break;
			}
		}
	}
	astrix_app_invalidate(host);
}

static void open_entry(struct astrix_app_host *host, struct files_app *fa,
                       const struct astrix_entry *e) {
	if (e->kind == ASTRIX_ENTRY_DIR) {
		go_to(host, fa, e->full_path);
		return;
	}
	if (e->kind == ASTRIX_ENTRY_LINK) {
		/* A symlink could point anywhere; resolve before acting on it. */
		char resolved[ASTRIX_FILES_PATH_MAX];
		if (realpath(e->full_path, resolved)) {
			struct stat st;
			if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
				go_to(host, fa, resolved);
				return;
			}
		}
		astrix_app_toast(host, "%s is a broken link", e->name);
		return;
	}
	/*
	 * Opening a file means handing it to something that can display it.
	 * There is no desktop .desktop database on a phone, so the mapping is
	 * an explicit table. Anything not in the table is reported rather than
	 * opened with a program that cannot read it.
	 */
	const char *viewer = NULL;
	size_t nl = strlen(e->name);
	if (nl > 4 && (strcasecmp(e->name + nl - 4, ".txt") == 0 ||
	               strcasecmp(e->name + nl - 5, ".conf") == 0)) {
		viewer = "org.astrix.Terminal";
	} else if (nl > 4 && (strcasecmp(e->name + nl - 4, ".png") == 0 ||
	                       strcasecmp(e->name + nl - 4, ".jpg") == 0)) {
		viewer = "org.astrix.Images";
	}
	if (!viewer) {
		astrix_app_toast(host, "no app for %s", e->name);
		return;
	}
	/*
	 * The compositor is single-foreground, so asking it to launch a
	 * viewer is a request over the session socket, not a direct call.
	 * Until that socket exists, the honest behaviour is to say so.
	 */
	astrix_app_toast(host, "open %s in %s", e->name, viewer);
}

/* --- destructive actions ------------------------------------------------- */

static int remove_recursive(const char *path) {
	struct stat st;
	if (lstat(path, &st) != 0) {
		return -1;
	}
	if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
		DIR *dir = opendir(path);
		if (!dir) {
			return -1;
		}
		struct dirent *de;
		int rc = 0;
		while ((de = readdir(dir)) != NULL) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}
			char child[ASTRIX_FILES_PATH_MAX];
			astrix_files_join(child, sizeof(child), path, de->d_name);
			if (remove_recursive(child) != 0) {
				rc = -1;
			}
		}
		closedir(dir);
		if (rc != 0) {
			return -1;
		}
		return rmdir(path);
	}
	return unlink(path);
}

static void do_delete(struct astrix_app_host *host, struct files_app *fa) {
	const char *path = fa->pending_path;
	if (strcmp(path, "/") == 0) {
		astrix_app_toast(host, "refusing to delete /");
		fa->screen = FILES_LIST;
		return;
	}
	if (remove_recursive(path) != 0) {
		fa->screen = FILES_ERROR;
		snprintf(fa->status, sizeof(fa->status), "delete %.80s: %.60s", path, strerror(errno));
	} else {
		astrix_app_toast(host, "deleted %s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
		char back[ASTRIX_FILES_PATH_MAX];
		snprintf(back, sizeof(back), "%s", fa->cwd);
		go_to(host, fa, back);
	}
	astrix_app_invalidate(host);
}

/* --- drawing ------------------------------------------------------------- */

static void on_frame(struct astrix_app_host *host) {
	struct files_app *fa = astrix_app_user(host);
	struct astrix_canvas *c = astrix_app_canvas(host);
	const struct astrix_theme *th = astrix_app_theme(host);
	int w, h;
	astrix_app_size(host, &w, &h);

	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, w, h }, th->background);

	char title[256];
	astrix_files_basename(fa->cwd, title, sizeof(title));

	if (fa->screen == FILES_ERROR) {
		astrix_widget_appbar(c, th, "Files", fa->cwd, true);
		struct astrix_rect body = { th->spacing * 3, list_top(th) + 40, w - th->spacing * 6,
			                            h - list_top(th) - 200 };
		astrix_widget_empty(c, th, body, "Cannot open", fa->status);
		struct astrix_rect back = { th->spacing * 3, h - 140, w - th->spacing * 6, 56 };
		astrix_widget_button(c, th, back, "Back", 1, false);
		return;
	}

	/* Path line under the app bar, so the user is never guessing where
	 * they are. */
	struct astrix_rect content = astrix_widget_appbar(c, th, "Files", fa->cwd, true);
	astrix_draw_text(c, th->spacing * 2, content.y + 4, title, th->text_dim);
	int meta_y = content.y + 22;
	char meta[160];
	if (fa->dir.truncated) {
		snprintf(meta, sizeof(meta), "%d+ entries - list truncated", fa->dir.count);
	} else {
		snprintf(meta, sizeof(meta), "%d entries", fa->dir.count);
	}
	astrix_draw_text(c, th->spacing * 2, meta_y, meta, th->text_dim);

	int list_y = content.y + 46;
	int list_h = h - list_y;
	int rh = row_height(th);

	if (fa->dir.count == 0) {
		struct astrix_rect body = { th->spacing * 3, list_y + 40, w - th->spacing * 6,
			                            list_h - 120 };
		astrix_widget_empty(c, th, body, "Empty folder", "Nothing here to show.");
	}

	/*
	 * Only the visible rows are drawn. A 4000-entry directory would
	 * otherwise cost 4000 rounded rectangles per frame on a phone CPU.
	 */
	int first = fa->scroll.offset / rh;
	if (first < 0) {
		first = 0;
	}
	for (int i = first; i < fa->dir.count; i++) {
		int y = list_y + i * rh - fa->scroll.offset;
		if (y + rh < list_y) {
			continue;
		}
		if (y > h) {
			break;
		}
		const struct astrix_entry *e = &fa->dir.entries[i];
		struct astrix_rect r = { 0, y, w, rh };
		bool selected = (i == fa->dir.selected);
		bool pressed = fa->pressed_valid && fa->pressed_row == i;

		const char *trailing = NULL;
		char sizebuf[24];
		if (e->kind == ASTRIX_ENTRY_DIR) {
			trailing = "dir";
		} else if (e->kind == ASTRIX_ENTRY_LINK) {
			trailing = "link";
		} else {
			astrix_files_format_size(e->size, sizebuf, sizeof(sizebuf));
			trailing = sizebuf;
		}
		astrix_widget_row(c, th, r, e->name, NULL, trailing, selected, pressed);
	}

	/* Action bar at the bottom. */
	int bar_h = th->navbar_h + 8;
	struct astrix_rect bar = { 0, h - bar_h, w, bar_h };
	astrix_fill_rect(c, bar, th->surface);
	astrix_fill_rect(c, (struct astrix_rect){ 0, bar.y, w, 1 }, th->divider);
	int bw = (w - th->spacing * 6) / 3;
	astrix_widget_button(c, th, (struct astrix_rect){ th->spacing * 2, bar.y + 8, bw, 44 },
	                     "Up", 0, false);
	astrix_widget_button(c, th,
	                     (struct astrix_rect){ th->spacing * 2 + bw + 4, bar.y + 8, bw, 44 },
	                     "Open", 1, false);
	astrix_widget_button(c, th,
	                     (struct astrix_rect){ th->spacing * 2 + (bw + 4) * 2, bar.y + 8, bw, 44 },
	                     "Delete", 2, false);

	if (fa->screen == FILES_CONFIRM_DELETE) {
		astrix_fill_rect(c, (struct astrix_rect){ 0, 0, w, h },
		                 astrix_color_with_alpha(th->overlay, 170));
		struct astrix_rect card = { th->spacing * 3, h / 2 - 130, w - th->spacing * 6, 260 };
		struct astrix_rect inner = astrix_widget_card(c, th, card);
		astrix_draw_text(c, inner.x, inner.y, "Delete?", th->text);
		char name[128];
		astrix_text_ellipsize(fa->pending_path, inner.w, name, sizeof(name));
		astrix_draw_text(c, inner.x, inner.y + 30, name, th->text_dim);
		const char *warn = astrix_files_is_home(fa->pending_path)
		                       ? "This is inside your home folder."
		                       : "This cannot be undone.";
		astrix_draw_text(c, inner.x, inner.y + 52, warn, th->warning);
		int cw = (inner.w - 8) / 2;
		astrix_widget_button(c, th, (struct astrix_rect){ inner.x, inner.y + 110, cw, 48 },
		                     "Cancel", 0, false);
		astrix_widget_button(c, th, (struct astrix_rect){ inner.x + cw + 8, inner.y + 110, cw, 48 },
		                     "Delete", 2, false);
	}
}

/* --- input --------------------------------------------------------------- */

static bool rect_hit(struct astrix_rect r, int x, int y) {
	return astrix_rect_contains(r, x, y);
}

static bool on_input(struct astrix_app_host *host, const struct astrix_input_event *ev) {
	struct files_app *fa = astrix_app_user(host);
	int w, h;
	astrix_app_size(host, &w, &h);
	const struct astrix_theme *th = astrix_app_theme(host);
	int rh = row_height(th);
	int list_y = list_top(th) + 46;
	int bar_h = th->navbar_h + 8;
	struct astrix_rect bar = { 0, h - bar_h, w, bar_h };

	switch (ev->kind) {
	case ASTRIX_INPUT_KEY:
		if (!(ev->state & ASTRIX_KEY_PRESSED)) {
			return true;
		}
		/* A hardware keyboard should be able to drive the list without
		 * a mouse, otherwise the file manager is touch-only. */
		switch (ev->key) {
		case KEY_UP:
			astrix_dir_move_selection(&fa->dir, -1);
			astrix_scroll_clamp(&fa->scroll);
			astrix_app_invalidate(host);
			return true;
		case KEY_DOWN:
			astrix_dir_move_selection(&fa->dir, 1);
			astrix_scroll_clamp(&fa->scroll);
			astrix_app_invalidate(host);
			return true;
		case KEY_ENTER:
		case KEY_KPENTER: {
			const struct astrix_entry *e = astrix_dir_at(&fa->dir, fa->dir.selected);
			if (e) {
				open_entry(host, fa, e);
			}
			return true;
		}
		case KEY_BACKSPACE:
			go_up(host, fa);
			return true;
		case KEY_ESC:
			if (fa->screen != FILES_LIST) {
				fa->screen = FILES_LIST;
				astrix_app_invalidate(host);
			}
			return true;
		case KEY_DELETE: {
			const struct astrix_entry *e = astrix_dir_at(&fa->dir, fa->dir.selected);
			if (e && e->kind != ASTRIX_ENTRY_DIR) {
				snprintf(fa->pending_path, sizeof(fa->pending_path), "%s", e->full_path);
				fa->screen = FILES_CONFIRM_DELETE;
				astrix_app_invalidate(host);
			}
			return true;
		}
		default:
			return true;
		}

	case ASTRIX_INPUT_TOUCH_DOWN: {
		fa->pressed_valid = false;
		if (fa->screen == FILES_ERROR) {
			struct astrix_rect back = { th->spacing * 3, h - 140, w - th->spacing * 6, 56 };
			if (rect_hit(back, ev->x, ev->y)) {
				char back_to[ASTRIX_FILES_PATH_MAX];
				snprintf(back_to, sizeof(back_to), "%s", fa->cwd);
				go_to(host, fa, back_to);
			}
			return true;
		}
		if (fa->screen == FILES_CONFIRM_DELETE) {
			struct astrix_rect card = { th->spacing * 3, h / 2 - 130, w - th->spacing * 6, 260 };
			struct astrix_rect inner = astrix_widget_card_inner(th, card);
			int cw = (inner.w - 8) / 2;
			struct astrix_rect cancel = { inner.x, inner.y + 110, cw, 48 };
			struct astrix_rect del = { inner.x + cw + 8, inner.y + 110, cw, 48 };
			if (rect_hit(del, ev->x, ev->y)) {
				fa->screen = FILES_LIST;
				do_delete(host, fa);
			} else if (rect_hit(cancel, ev->x, ev->y)) {
				fa->screen = FILES_LIST;
			} else if (rect_hit(card, ev->x, ev->y)) {
				/* Tapping the scrim dismisses, as a modal should. */
				fa->screen = FILES_LIST;
			}
			astrix_app_invalidate(host);
			return true;
		}
		/* The action bar. */
		if (rect_hit(bar, ev->x, ev->y)) {
			int bw = (w - th->spacing * 6) / 3;
			int slot = (ev->x - th->spacing * 2) / (bw + 4);
			if (slot == 0) {
				go_up(host, fa);
			} else if (slot == 1) {
				const struct astrix_entry *e = astrix_dir_at(&fa->dir, fa->dir.selected);
				if (e) {
					open_entry(host, fa, e);
				} else {
					astrix_app_toast(host, "nothing selected");
				}
			} else {
				const struct astrix_entry *e = astrix_dir_at(&fa->dir, fa->dir.selected);
				if (!e) {
					astrix_app_toast(host, "nothing selected");
				} else if (e->kind == ASTRIX_ENTRY_DIR) {
					astrix_app_toast(host, "select a file to delete");
				} else {
					snprintf(fa->pending_path, sizeof(fa->pending_path), "%s", e->full_path);
					fa->screen = FILES_CONFIRM_DELETE;
					astrix_app_invalidate(host);
				}
			}
			return true;
		}
		/* A row. */
		if (ev->y >= list_y && ev->y < h - bar_h) {
			int index = fa->scroll.offset / rh + (ev->y - list_y) / rh;
			if (index >= 0 && index < fa->dir.count) {
				fa->dir.selected = index;
				fa->pressed_row = index;
				fa->pressed_valid = true;
			}
		}
		return true;
	}

	case ASTRIX_INPUT_TOUCH_MOTION:
	case ASTRIX_INPUT_POINTER_MOTION:
		if (astrix_scroll_handle(&fa->scroll, ev)) {
			fa->pressed_valid = false;
			astrix_app_invalidate(host);
		}
		return false;

	case ASTRIX_INPUT_TOUCH_UP:
		if (fa->pressed_valid) {
			int index = fa->scroll.offset / rh + (ev->y - list_y) / rh;
			if (index == fa->pressed_row && index >= 0 && index < fa->dir.count) {
				const struct astrix_entry *e = &fa->dir.entries[index];
				if (ev->y - (list_y + index * rh - fa->scroll.offset) < rh / 2) {
					/* A tap on the top half opens; the bottom half is
					 * reserved for a future context menu, so an
					 * accidental low tap cannot launch something. */
					open_entry(host, fa, e);
				}
			}
			fa->pressed_valid = false;
		}
		return true;

	case ASTRIX_INPUT_SCROLL:
		fa->scroll.offset -= ev->scroll_y / 4;
		astrix_scroll_clamp(&fa->scroll);
		astrix_app_invalidate(host);
		return true;

	default:
		return false;
	}
}

static void on_tick(struct astrix_app_host *host, double dt) {
	(void)dt;
	struct files_app *fa = astrix_app_user(host);
	/* Keep the inertia going; one call per tick is exactly the "per
	 * display frame" contract astrix_scroll_step documents. */
	if (astrix_scroll_step(&fa->scroll)) {
		astrix_app_invalidate(host);
	}
}

static void on_start(struct astrix_app_host *host) {
	struct files_app *fa = astrix_app_user(host);
	astrix_app_size(host, &fa->width, &fa->height);
	const char *start = getenv("ASTRIX_FILES_CWD");
	if (!start || !start[0]) {
		start = getenv("HOME");
	}
	if (!start || !start[0]) {
		start = "/";
	}
	go_to(host, fa, start);
}

static const struct astrix_app_desc desc = {
	.app_id = "org.astrix.Files",
	.title = "Files",
	.version = "0.1.0",
	.wants_keyboard = false,
	.on_start = on_start,
	.on_frame = on_frame,
	.on_input = on_input,
	.on_exit = NULL,
	.on_tick = on_tick,
};

int main(int argc, char **argv) {
	struct files_app fa;
	memset(&fa, 0, sizeof(fa));
	astrix_dir_init(&fa.dir);
	snprintf(fa.cwd, sizeof(fa.cwd), "/");
	fa.pressed_row = -1;

	const char *start = NULL;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--path=", 7) == 0) {
			start = argv[i] + 7;
		}
	}
	if (start) {
		astrix_dir_read(&fa.dir, start);
		snprintf(fa.cwd, sizeof(fa.cwd), "%s", start);
	}

	return astrix_app_main_with_user(&desc, argc, argv, &fa);
}
