/*
 * Astrix OS - file manager model.
 *
 * Directory listing is separated from the UI for the same reason the terminal
 * separates its screen model: the interesting behaviour (sorting, filtering,
 * deciding what is safe to show) is pure and can be tested on the host, and
 * the UI is then a thin layer over it.
 */

#ifndef ASTRIX_FILES_H
#define ASTRIX_FILES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ASTRIX_FILES_MAX_ENTRIES 4096
#define ASTRIX_FILES_PATH_MAX 1024

enum astrix_entry_kind {
	ASTRIX_ENTRY_DIR,
	ASTRIX_ENTRY_FILE,
	ASTRIX_ENTRY_LINK,
	ASTRIX_ENTRY_OTHER,
};

struct astrix_entry {
	char name[256];
	char full_path[ASTRIX_FILES_PATH_MAX];
	enum astrix_entry_kind kind;
	int64_t size;        /* bytes; -1 when unknown (e.g. a dead symlink) */
	int64_t mtime;
	mode_t mode;
};

enum astrix_sort_key {
	ASTRIX_SORT_NAME,
	ASTRIX_SORT_SIZE,
	ASTRIX_SORT_DATE,
	ASTRIX_SORT_KIND,
};

struct astrix_dir {
	char path[ASTRIX_FILES_PATH_MAX];
	struct astrix_entry *entries;
	int count;
	int capacity;
	int selected;         /* index into entries, -1 = none */
	/* The scroll offset is in the UI, not the model. */
	enum astrix_sort_key sort_key;
	bool sort_descending;
	bool show_hidden;
	char filter[64];      /* case-insensitive substring match */
	bool truncated;       /* the directory had more entries than the cap */
	char error[128];
};

void astrix_dir_init(struct astrix_dir *d);
void astrix_dir_free(struct astrix_dir *d);

/*
 * Read a directory. Returns 0 on success, -1 on failure with the reason in
 * d->error. Hidden entries are skipped unless show_hidden is set; `filter`
 * further narrows the list.
 */
int astrix_dir_read(struct astrix_dir *d, const char *path);

/* Re-apply the sort and the filter in place. */
void astrix_dir_apply(struct astrix_dir *d);

/* The entry at `index`, or NULL. */
const struct astrix_entry *astrix_dir_at(const struct astrix_dir *d, int index);

/* Move the selection by `delta` entries, clamped. Returns the new index. */
int astrix_dir_move_selection(struct astrix_dir *d, int delta);

/* Join a directory and a name into `out`. Handles a trailing slash. */
void astrix_files_join(char *out, size_t out_size, const char *dir, const char *name);

/* The display name of the current directory: its basename, or "/" for root. */
void astrix_files_basename(const char *path, char *out, size_t out_size);

/* Human-readable size, e.g. "1.4 GB". */
void astrix_files_format_size(int64_t bytes, char *out, size_t out_size);

/* True if the path is inside $HOME - used to warn before a destructive op. */
bool astrix_files_is_home(const char *path);

#endif /* ASTRIX_FILES_H */
