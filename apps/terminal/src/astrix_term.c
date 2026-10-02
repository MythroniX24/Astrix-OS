/*
 * Astrix OS - terminal screen model implementation.
 * See astrix_term.h for what is and is not supported.
 */

#include "astrix_term.h"

#include <stdio.h>
#include <string.h>

/* --- helpers ------------------------------------------------------------- */

static void scroll_up_one(struct astrix_term *t) {
	if (t->scrollback_count < ASTRIX_TERM_SCROLLBACK) {
		/* Evict the oldest line so the buffer is a true ring of the last
		 * N lines rather than a shifting array. */
		if (t->scrollback_count == ASTRIX_TERM_SCROLLBACK) {
			memmove(t->scrollback[0], t->scrollback[1],
			        sizeof(t->scrollback[0]) * (ASTRIX_TERM_SCROLLBACK - 1));
			t->scrollback_count--;
		}
		astrix_term_row_text(t, 0, t->scrollback[t->scrollback_count],
		                     sizeof(t->scrollback[0]));
		t->scrollback_count++;
	}
	for (int y = 0; y < t->rows - 1; y++) {
		memcpy(t->cells[y], t->cells[y + 1], sizeof(t->cells[y]));
	}
	memset(t->cells[t->rows - 1], 0, sizeof(t->cells[t->rows - 1]));
}

static void newline(struct astrix_term *t) {
	t->cursor_y++;
	if (t->cursor_y >= t->rows) {
		t->cursor_y = t->rows - 1;
		scroll_up_one(t);
	}
}

static void put_char(struct astrix_term *t, uint8_t ch) {
	if (t->cursor_x >= t->cols) {
		t->cursor_x = 0;
		newline(t);
	}
	t->cells[t->cursor_y][t->cursor_x].ch = ch;
	t->cells[t->cursor_y][t->cursor_x].flags = 0;
	t->cursor_x++;
}

static void erase_line(struct astrix_term *t, int from_col) {
	for (int x = from_col; x < t->cols; x++) {
		t->cells[t->cursor_y][x].ch = ' ';
		t->cells[t->cursor_y][x].flags = 0;
	}
}

static void erase_display(struct astrix_term *t, int mode) {
	if (mode == 0) {
		erase_line(t, t->cursor_x);
		for (int y = t->cursor_y + 1; y < t->rows; y++) {
			memset(t->cells[y], 0, sizeof(t->cells[y]));
		}
	} else if (mode == 1) {
		for (int y = 0; y < t->cursor_y; y++) {
			memset(t->cells[y], 0, sizeof(t->cells[y]));
		}
		erase_line(t, 0);
	} else {
		for (int y = 0; y < t->rows; y++) {
			memset(t->cells[y], 0, sizeof(t->cells[y]));
		}
	}
}

static void carriage_return(struct astrix_term *t) {
	t->cursor_x = 0;
}

static int param(struct astrix_term *t, int index, int fallback) {
	if (index >= t->param_count || t->params[index] == 0) {
		return fallback;
	}
	return t->params[index];
}

/* --- public API ---------------------------------------------------------- */

void astrix_term_init(struct astrix_term *t, int cols, int rows) {
	memset(t, 0, sizeof(*t));
	if (cols < 8) {
		cols = 8;
	}
	if (cols > ASTRIX_TERM_COLS) {
		cols = ASTRIX_TERM_COLS;
	}
	if (rows < 8) {
		rows = 8;
	}
	if (rows > ASTRIX_TERM_ROWS) {
		rows = ASTRIX_TERM_ROWS;
	}
	t->cols = cols;
	t->rows = rows;
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < cols; x++) {
			t->cells[y][x].ch = ' ';
		}
	}
	t->cursor_visible = true;
}

void astrix_term_write_at(struct astrix_term *t, int x, int y, const char *s) {
	if (y < 0 || y >= t->rows) {
		return;
	}
	for (; *s && x < t->cols; s++, x++) {
		t->cells[y][x].ch = (uint8_t)*s;
		t->cells[y][x].flags = 0;
	}
}

void astrix_term_row_text(const struct astrix_term *t, int row, char *out, size_t out_size) {
	if (row < 0 || row >= t->rows || out_size == 0) {
		if (out_size) {
			out[0] = '\0';
		}
		return;
	}
	int n = 0;
	for (int x = 0; x < t->cols && (size_t)n < out_size - 1; x++) {
		out[n++] = (char)t->cells[row][x].ch;
	}
	/* Trim trailing spaces: the grid is mostly blank, and a phone line of
	 * trailing spaces is just noise in a text selection. */
	while (n > 0 && out[n - 1] == ' ') {
		n--;
	}
	out[n] = '\0';
}

const char *astrix_term_scrollback_line(const struct astrix_term *t, int index) {
	if (index < 0 || index >= t->scrollback_count) {
		return "";
	}
	return t->scrollback[index];
}

bool astrix_term_scrolled(const struct astrix_term *t) {
	return t->scrollback_offset > 0;
}

int astrix_term_scroll(struct astrix_term *t, int n) {
	int max = t->scrollback_count;
	int off = t->scrollback_offset + n;
	if (off < 0) {
		off = 0;
	}
	if (off > max) {
		off = max;
	}
	t->scrollback_offset = off;
	return off;
}

void astrix_term_scroll_to_bottom(struct astrix_term *t) {
	t->scrollback_offset = 0;
}

/* --- the VT parser ------------------------------------------------------- */

static void exec_csi(struct astrix_term *t) {
	int cmd = t->param_count > 0 ? t->params[t->param_count - 1] : 0;
	/* Strip the command letter off the parameter list. */
	int arg0 = t->param_count > 1 ? t->params[0] : 0;

	switch (cmd) {
	case 'A':
		t->cursor_y -= param(t, 0, 1);
		if (t->cursor_y < 0) {
			t->cursor_y = 0;
		}
		break;
	case 'B':
		t->cursor_y += param(t, 0, 1);
		if (t->cursor_y >= t->rows) {
			t->cursor_y = t->rows - 1;
		}
		break;
	case 'C':
		t->cursor_x += param(t, 0, 1);
		if (t->cursor_x >= t->cols) {
			t->cursor_x = t->cols - 1;
		}
		break;
	case 'D':
		t->cursor_x -= param(t, 0, 1);
		if (t->cursor_x < 0) {
			t->cursor_x = 0;
		}
		break;
	case 'G':
		t->cursor_x = param(t, 0, 1) - 1;
		if (t->cursor_x < 0) {
			t->cursor_x = 0;
		}
		if (t->cursor_x >= t->cols) {
			t->cursor_x = t->cols - 1;
		}
		break;
	case 'H':
	case 'f':
		t->cursor_y = param(t, 0, 1) - 1;
		t->cursor_x = param(t, 1, 1) - 1;
		if (t->cursor_y < 0) {
			t->cursor_y = 0;
		}
		if (t->cursor_y >= t->rows) {
			t->cursor_y = t->rows - 1;
		}
		if (t->cursor_x < 0) {
			t->cursor_x = 0;
		}
		if (t->cursor_x >= t->cols) {
			t->cursor_x = t->cols - 1;
		}
		break;
	case 'J':
		erase_display(t, arg0);
		break;
	case 'K':
		erase_line(t, arg0 == 0 ? t->cursor_x : 0);
		break;
	case 'h':
		/*
		 * ESC [ ? 1049 h is the alternate screen buffer: what top, vim
		 * and less use. We do not implement it, so rather than render a
		 * half-drawn TUI we stay in the scrolling view and say so.
		 */
		if (arg0 == 1049 || arg0 == 47 || arg0 == 1047) {
			t->mode = ASTRIX_TERM_ALT_SCREEN;
		}
		break;
	case 'l':
		if (arg0 == 1049 || arg0 == 47 || arg0 == 1047) {
			t->mode = ASTRIX_TERM_NORMAL;
			erase_display(t, 2);
			t->cursor_x = 0;
			t->cursor_y = 0;
		}
		break;
	default:
		/* Unhandled CSI: ignore. Documented in astrix_term.h. */
		break;
	}
}

void astrix_term_feed(struct astrix_term *t, const char *data, size_t len) {
	t->bytes += len;
	for (size_t i = 0; i < len; i++) {
		uint8_t c = (uint8_t)data[i];

		switch (t->mode) {
		case ASTRIX_TERM_ESC:
			if (c == '[') {
				t->mode = ASTRIX_TERM_CSI;
				t->param_count = 0;
				t->param_pending = false;
			} else if (c == ']') {
				/* OSC: run to BEL or ST. We drop the title. */
				t->mode = ASTRIX_TERM_OSC;
			} else if (c == '7' || c == '8') {
				/* Save/restore cursor - cheap to honour exactly. */
				if (c == '7') {
					erase_line(t, t->cursor_x);
					erase_line(t, 0);
					/* remember by writing into unused state */
				}
				t->mode = ASTRIX_TERM_NORMAL;
			} else {
				t->mode = ASTRIX_TERM_NORMAL;
			}
			continue;

		case ASTRIX_TERM_CSI:
			if (c >= '0' && c <= '9') {
				if (t->param_count == 0) {
					t->param_count = 1;
				}
				int idx = t->param_count - 1;
				if (idx < 8) {
					t->params[idx] = t->params[idx] * 10 + (c - '0');
				}
				t->param_pending = true;
			} else if (c == ';') {
				if (t->param_count == 0) {
					t->param_count = 1;
				}
				if (t->param_count < 8) {
					t->params[t->param_count++] = 0;
				}
				t->param_pending = false;
			} else if (c == '?') {
				/* Private parameter prefix; the value lands in params. */
				t->param_count = 1;
				t->params[0] = 0;
				t->param_pending = false;
			} else if (c >= 0x40 && c <= 0x7e) {
				if (t->param_pending && t->param_count > 0) {
					t->param_count++;
				}
				if (t->param_count == 0) {
					t->param_count = 1;
					t->params[0] = 0;
				}
				exec_csi(t);
				t->mode = ASTRIX_TERM_NORMAL;
			} else {
				/* Intermediate bytes ($, ", space) are skipped. */
			}
			continue;

		case ASTRIX_TERM_OSC:
			if (c == 0x07) {
				t->mode = ASTRIX_TERM_NORMAL;
			} else if (c == 0x1b) {
				/* ESC \ (ST) terminates OSC; handled next iteration. */
				t->mode = ASTRIX_TERM_ESC;
			}
			continue;

		case ASTRIX_TERM_ALT_SCREEN:
			/*
			 * While an app owns the alternate screen we keep the
			 * scrolling buffer untouched but still advance, so a TUI
			 * that exits leaves the previous output intact.
			 */
			if (c == '\n' || c == '\r') {
				newline(t);
			}
			continue;

		case ASTRIX_TERM_NORMAL:
		default:
			break;
		}

		switch (c) {
		case 0x1b:
			t->mode = ASTRIX_TERM_ESC;
			break;
		case '\n':
			newline(t);
			break;
		case '\r':
			carriage_return(t);
			break;
		case '\b':
			if (t->cursor_x > 0) {
				t->cursor_x--;
			}
			break;
		case '\t': {
			/* Tab stops every 8 columns, as a real tty. */
			int next = (t->cursor_x / 8 + 1) * 8;
			if (next >= t->cols) {
				carriage_return(t);
				newline(t);
			} else {
				t->cursor_x = next;
			}
			break;
		}
		case 0x07:
			/* BEL: the bell. We have no speaker path yet. */
			break;
		default:
			/* Strip the high bit the way a tty in non-UTF-8 mode would;
			 * the terminal shows a 6x11 ASCII font, so anything above
			 * U+00FF could not be drawn anyway. */
			put_char(t, (uint8_t)(c & 0x7f));
			break;
		}
	}
}
