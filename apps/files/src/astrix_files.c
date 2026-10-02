/*
 * Astrix OS - file manager model implementation.
 */

#include "astrix_files.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void astrix_dir_init(struct astrix_dir *d) {
	memset(d, 0, sizeof(*d));
	d->selected = -1;
	d->sort_key = ASTRIX_SORT_NAME;
	d->show_hidden = false;
	d->capacity = 0;
}

void astrix_dir_free(struct astrix_dir *d) {
	free(d->entries);
	d->entries = NULL;
	d->count = 0;
	d->capacity = 0;
}

void astrix_files_join(char *out, size_t out_size, const char *dir, const char *name) {
	if (!dir || !*dir) {
		dir = "/";
	}
	if (strcmp(dir, "/") == 0) {
		snprintf(out, out_size, "/%s", name);
		return;
	}
	size_t dl = strlen(dir);
	if (dl > 0 && dir[dl - 1] == '/') {
		snprintf(out, out_size, "%s%s", dir, name);
	} else {
		snprintf(out, out_size, "%s/%s", dir, name);
	}
}

void astrix_files_basename(const char *path, char *out, size_t out_size) {
	if (!path || !*path || strcmp(path, "/") == 0) {
		snprintf(out, out_size, "/");
		return;
	}
	const char *slash = strrchr(path, '/');
	if (slash && slash[1]) {
		snprintf(out, out_size, "%s", slash + 1);
	} else if (slash) {
		snprintf(out, out_size, "/");
	} else {
		snprintf(out, out_size, "%s", path);
	}
}

void astrix_files_format_size(int64_t bytes, char *out, size_t out_size) {
	if (bytes < 0) {
		snprintf(out, out_size, "-");
		return;
	}
	static const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	if (u == 0) {
		snprintf(out, out_size, "%lld B", (long long)bytes);
	} else {
		snprintf(out, out_size, "%.1f %s", v, unit[u]);
	}
}

bool astrix_files_is_home(const char *path) {
	const char *home = getenv("HOME");
	if (!home || !*home) {
		return false;
	}
	size_t hl = strlen(home);
	if (strncmp(path, home, hl) != 0) {
		return false;
	}
	return path[hl] == '\0' || path[hl] == '/';
}

/* --- sorting ------------------------------------------------------------- */

/* See astrix_dir_apply for why this is a file-scope pointer. */
static const struct astrix_dir *g_sort_ctx;

static int compare_entries(const void *pa, const void *pb, void *ctx);

static int compare_thunk(const void *a, const void *b) {
	return compare_entries(a, b, (void *)(uintptr_t)g_sort_ctx);
}

static int rank_kind(const struct astrix_entry *e) {
	/* Directories always sort above files, in every sort mode. A file
	 * manager that interleaves them makes the primary scan slow. */
	if (e->kind == ASTRIX_ENTRY_DIR) {
		return 0;
	}
	return 1;
}

static int compare_entries(const void *pa, const void *pb, void *ctx) {
	const struct astrix_entry *a = pa;
	const struct astrix_entry *b = pb;
	const struct astrix_dir *d = ctx;

	int ra = rank_kind(a), rb = rank_kind(b);
	if (ra != rb) {
		return ra - rb;
	}

	int r = 0;
	switch (d->sort_key) {
	case ASTRIX_SORT_SIZE:
		r = (a->size < b->size) ? -1 : (a->size > b->size ? 1 : 0);
		break;
	case ASTRIX_SORT_DATE:
		r = (a->mtime < b->mtime) ? -1 : (a->mtime > b->mtime ? 1 : 0);
		break;
	case ASTRIX_SORT_KIND: {
		/* Group by file extension, which is how people look for media
		 * and documents. */
		const char *ea = strrchr(a->name, '.');
		const char *eb = strrchr(b->name, '.');
		const char *xa = ea ? ea + 1 : "";
		const char *xb = eb ? eb + 1 : "";
		r = strcasecmp(xa, xb);
		break;
	}
	case ASTRIX_SORT_NAME:
	default:
		/* strcasecmp so a lowercase name does not sort after an
		 * uppercase one; phone users do not expect ASCII order. */
		r = strcasecmp(a->name, b->name);
		break;
	}
	if (r == 0) {
		r = strcasecmp(a->name, b->name);
	}
	return d->sort_descending ? -r : r;
}

void astrix_dir_apply(struct astrix_dir *d) {
	if (d->count <= 1) {
		return;
	}
	/*
	 * qsort_r's argument order differs between glibc and musl. Rather
	 * than write two versions of this file, the comparator state goes
	 * through this file-scope pointer. The list is never sorted from two
	 * threads - the app is single-threaded by design - so this is safe
	 * and keeps the rest of the comparator readable.
	 */
	g_sort_ctx = d;
	qsort(d->entries, (size_t)d->count, sizeof(d->entries[0]), compare_thunk);
	g_sort_ctx = NULL;
}

const struct astrix_entry *astrix_dir_at(const struct astrix_dir *d, int index) {
	if (index < 0 || index >= d->count) {
		return NULL;
	}
	return &d->entries[index];
}

int astrix_dir_move_selection(struct astrix_dir *d, int delta) {
	if (d->count == 0) {
		d->selected = -1;
		return -1;
	}
	if (d->selected < 0) {
		d->selected = delta > 0 ? 0 : d->count - 1;
		return d->selected;
	}
	d->selected += delta;
	if (d->selected < 0) {
		d->selected = 0;
	}
	if (d->selected >= d->count) {
		d->selected = d->count - 1;
	}
	return d->selected;
}

/* --- reading ------------------------------------------------------------- */

static bool entry_matches(const struct astrix_entry *e, const struct astrix_dir *d) {
	if (!d->show_hidden && e->name[0] == '.') {
		return false;
	}
	if (d->filter[0] && !strcasestr(e->name, d->filter)) {
		return false;
	}
	return true;
}

static void push_entry(struct astrix_dir *d, const struct astrix_entry *e) {
	if (d->count == d->capacity) {
		int cap = d->capacity ? d->capacity * 2 : 64;
		struct astrix_entry *ne = realloc(d->entries, (size_t)cap * sizeof(*ne));
		if (!ne) {
			snprintf(d->error, sizeof(d->error), "out of memory");
			return;
		}
		d->entries = ne;
		d->capacity = cap;
	}
	d->entries[d->count++] = *e;
}

int astrix_dir_read(struct astrix_dir *d, const char *path) {
	d->count = 0;
	d->selected = -1;
	d->truncated = false;
	d->error[0] = '\0';
	snprintf(d->path, sizeof(d->path), "%s", path);

	DIR *dir = opendir(path);
	if (!dir) {
		snprintf(d->error, sizeof(d->error), "%s", strerror(errno));
		return -1;
	}
	struct dirent *de;
	errno = 0;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		if (d->count >= ASTRIX_FILES_MAX_ENTRIES) {
			d->truncated = true;
			break;
		}
		struct astrix_entry e;
		memset(&e, 0, sizeof(e));
		snprintf(e.name, sizeof(e.name), "%s", de->d_name);
		astrix_files_join(e.full_path, sizeof(e.full_path), path, de->d_name);

		/*
		 * d_type is often DT_UNKNOWN on filesystems that do not fill it
		 * in, and it is a hint rather than a guarantee. stat() is the
		 * only way to be correct, and correctness matters here because
		 * the type decides whether a tap opens or descends.
		 */
		struct stat st;
		if (lstat(e.full_path, &st) == 0) {
			e.mtime = (int64_t)st.st_mtime;
			e.mode = st.st_mode;
			e.size = S_ISREG(st.st_mode) ? (int64_t)st.st_size : -1;
			if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
				e.kind = ASTRIX_ENTRY_DIR;
			} else if (S_ISLNK(st.st_mode)) {
				e.kind = ASTRIX_ENTRY_LINK;
				/* A broken link is common and should be visible. */
				struct stat target;
				if (stat(e.full_path, &target) != 0) {
					e.size = -1;
				}
			} else if (S_ISREG(st.st_mode)) {
				e.kind = ASTRIX_ENTRY_FILE;
			} else {
				e.kind = ASTRIX_ENTRY_OTHER;
				e.size = -1;
			}
		} else {
			/* Raced with a delete, or unreadable. Show it, marked. */
			e.kind = ASTRIX_ENTRY_OTHER;
			e.size = -1;
		}

		if (!entry_matches(&e, d)) {
			continue;
		}
		push_entry(d, &e);
		errno = 0;
	}
	if (errno != 0 && d->count == 0) {
		snprintf(d->error, sizeof(d->error), "%s", strerror(errno));
	}
	closedir(dir);

	astrix_dir_apply(d);
	if (d->count > 0) {
		d->selected = 0;
	}
	return d->error[0] ? -1 : 0;
}
