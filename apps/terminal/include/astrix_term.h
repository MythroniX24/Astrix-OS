/*
 * Astrix OS - terminal screen model.
 *
 * The PTY gives us a byte stream; a screen needs cells. This file is the
 * state between the two: a grid of cells, a cursor, a scrollback, and a
 * small VT parser that turns the shell's output into grid writes.
 *
 * Scope of the escape-sequence support, stated honestly
 * ----------------------------------------------------
 * Implemented: printable text, CR, LF, BS, TAB, BEL (ignored), ESC [ K
 * (erase in line), ESC [ J / ESC [ 1 J (erase in display), ESC [ H and
 * ESC [ <r>;<c> H (cursor position), ESC [ <n> A/B/C/D (cursor moves).
 * Deliberately ignored: SGR (colour), scroll regions, alternate screen,
 * and everything else.
 *
 * That is enough for an interactive shell, ls, cat, apt, git, compilers and
 * pip. It is NOT a full VT100: `top`, `vim` and `less` will run but draw
 * incorrectly, because they use the alternate screen and SGR. Rather than
 * pretend otherwise, the terminal detects the alternate-screen switch
 * (ESC [ ? 1049 h) and puts the user in a plain scrolling view instead of
 * showing a mangled screen. See apps/terminal/README.md.
 */

#ifndef ASTRIX_TERM_H
#define ASTRIX_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ASTRIX_TERM_COLS 48
#define ASTRIX_TERM_ROWS 60
#define ASTRIX_TERM_SCROLLBACK 2000

/* One character cell. 8-bit glyph, so a UTF-8 sequence is assembled here. */
struct astrix_term_cell {
	uint8_t ch;      /* the low byte of the codepoint, 0 = space */
	uint8_t flags;   /* bit 0 = continuation of a wide/multi-byte char */
};

enum astrix_term_mode {
	ASTRIX_TERM_NORMAL = 0,
	ASTRIX_TERM_ESC,        /* saw ESC                    */
	ASTRIX_TERM_CSI,        /* inside ESC [ ...           */
	ASTRIX_TERM_OSC,        /* inside ESC ] ... (ignored) */
	ASTRIX_TERM_ALT_SCREEN, /* the app asked for a TUI    */
};

struct astrix_term {
	int cols, rows;
	struct astrix_term_cell cells[ASTRIX_TERM_ROWS][ASTRIX_TERM_COLS];

	int cursor_x, cursor_y;
	bool cursor_visible;

	/* Scrollback: oldest first. Only whole lines, which is all a phone
	 * screen can usefully show when you scroll up. */
	char scrollback[ASTRIX_TERM_SCROLLBACK][ASTRIX_TERM_COLS + 1];
	int scrollback_count;
	int scrollback_offset;   /* lines scrolled up from the live bottom */

	enum astrix_term_mode mode;
	/* Numeric parameters of an in-progress CSI sequence. */
	int params[8];
	int param_count;
	bool param_pending;
	/* A UTF-8 sequence spanning two feed() calls. */
	uint8_t utf8_buf[4];
	int utf8_len;

	/* Total bytes fed, so the app can show a live throughput figure. */
	uint64_t bytes;
};

void astrix_term_init(struct astrix_term *t, int cols, int rows);

/* Feed raw PTY output. */
void astrix_term_feed(struct astrix_term *t, const char *data, size_t len);

/* Write a string into the grid as if it had been typed (used for the prompt). */
void astrix_term_write_at(struct astrix_term *t, int x, int y, const char *s);

/* The text of one live row, NUL-terminated, trimmed of trailing spaces. */
void astrix_term_row_text(const struct astrix_term *t, int row, char *out, size_t out_size);

/* The text of one scrollback line. */
const char *astrix_term_scrollback_line(const struct astrix_term *t, int index);

/* Scroll up (positive) or down (negative) by n lines. Returns the new offset. */
int astrix_term_scroll(struct astrix_term *t, int n);

/* True when the view is scrolled back from the live output. */
bool astrix_term_scrolled(const struct astrix_term *t);

/* Return to the live bottom. */
void astrix_term_scroll_to_bottom(struct astrix_term *t);

#endif /* ASTRIX_TERM_H */
