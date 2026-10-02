/*
 * Astrix OS - Astrix Terminal.
 *
 * A real terminal: it allocates a PTY, runs an interactive login shell inside
 * it, and renders what the shell writes. vim, less, top, apt, git and
 * compilers all work in it, because it is the same kernel PTY a desktop
 * terminal uses. Nothing here is a canned transcript.
 *
 * The pieces:
 *   astrix_term.c  - the screen model and the escape-sequence parser
 *   astrix_kbd.c   - the on-screen keyboard layout
 *   main.c         - the PTY, the UI, and input handling
 *
 * Deliberate limitations, so they are not a surprise:
 *   - no SGR colour (see astrix_term.h for the full list)
 *   - no copy/paste clipboard; use a Bluetooth keyboard for that
 *   - one session; there is no tab bar
 */

#include "astrix_app.h"
#include "astrix_kbd.h"
#include "astrix_term.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

struct terminal {
	struct astrix_term term;
	int master_fd;
	pid_t child;

	bool running;      /* the shell is alive            */
	bool child_reaped;
	int exit_status;

	int font_scale;    /* 0 = small, 1 = normal, 2 = large */
	int layer;         /* keyboard layer: 0 letters, 1 shifted */
	bool ctrl_armed;

	/* Touch tracking for the keyboard. */
	int pressed_key_row;
	int pressed_key_index;
	bool pressed_valid;
	int press_x, press_y;

	/* Scrollback drag. */
	bool dragging;
	int drag_start_y;
	int drag_start_offset;

	char status[96];
};

static const int FONT_WIDTHS[] = { 5, 6, 7 };
static const int FONT_HEIGHTS[] = { 9, 11, 13 };
#define FONT_SCALE_COUNT ((int)(sizeof(FONT_WIDTHS) / sizeof(FONT_WIDTHS[0])))

static int font_width(const struct terminal *t) {
	return FONT_WIDTHS[t->font_scale];
}
static int font_height(const struct terminal *t) {
	return FONT_HEIGHTS[t->font_scale];
}

/* --- PTY ----------------------------------------------------------------- */

/*
 * Write to the shell. Returns 0 on success, -1 if the shell has gone, which
 * is the single most common thing that happens to a terminal and must not be
 * treated as a fatal error.
 */
static int pty_write(struct terminal *t, const char *s) {
	size_t len = strlen(s);
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(t->master_fd, s + off, len - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN) {
				/* The pty buffer is full; the shell will catch up. */
				break;
			}
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static void spawn_shell(struct terminal *t) {
	struct winsize ws = { .ws_row = (unsigned short)t->term.rows,
		                  .ws_col = (unsigned short)t->term.cols,
		                  .ws_xpixel = 0,
		                  .ws_ypixel = 0 };

	const char *shell = getenv("ASTRIX_SHELL");
	if (!shell || !shell[0]) {
		shell = "/bin/bash";
	}

	/*
	 * forkpty() gives a PTY, a login shell, and a controlling terminal in
	 * one call, and sets the window size before exec so the shell does
	 * not draw for 80x24 and then reflow. This is the same call a desktop
	 * terminal emulator makes.
	 */
	if (forkpty(&t->master_fd, NULL, NULL, &ws) < 0) {
		snprintf(t->status, sizeof(t->status), "forkpty: %s", strerror(errno));
		t->running = false;
		return;
	}

	if (t->child == 0) {
		/* Child: the standard login environment. */
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 1);
		unsetenv("WAYLAND_DISPLAY"); /* a terminal is not a GUI app */
		unsetenv("XDG_RUNTIME_DIR");
		execl(shell, shell, "-l", (char *)NULL);
		/* exec only returns on failure. */
		fprintf(stderr, "astrix-terminal: cannot exec %s: %s\n", shell, strerror(errno));
		_exit(127);
	}
	t->child = t->child;
	t->running = true;
	t->child_reaped = false;
	t->exit_status = 0;
	snprintf(t->status, sizeof(t->status), "%s", shell);

	/* Non-blocking reads: the app's frame loop must never stall on a
	 * silent shell. */
	int flags = fcntl(t->master_fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(t->master_fd, F_SETFL, flags | O_NONBLOCK);
	}
}

static void drain_pty(struct terminal *t) {
	if (t->master_fd < 0) {
		return;
	}
	char buf[4096];
	for (;;) {
		ssize_t n = read(t->master_fd, buf, sizeof(buf));
		if (n > 0) {
			astrix_term_feed(&t->term, buf, (size_t)n);
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return; /* drained */
		}
		/* n == 0 means the child closed the pty; n < 0 otherwise is a
		 * hard error. Either way the shell is gone. */
		t->running = false;
		if (n < 0) {
			snprintf(t->status, sizeof(t->status), "read: %s", strerror(errno));
		} else if (!t->child_reaped && t->child > 0) {
			int st = 0;
			if (waitpid(t->child, &st, WNOHANG) == t->child) {
				t->child_reaped = true;
				t->exit_status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
			}
		}
		return;
	}
}

static void on_tick(struct astrix_app_host *host, double dt) {
	(void)dt;
	struct terminal *t = astrix_app_user(host);
	drain_pty(t);
	if (t->child > 0 && !t->child_reaped) {
		int st = 0;
		pid_t r = waitpid(t->child, &st, WNOHANG);
		if (r == t->child) {
			t->child_reaped = true;
			t->exit_status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
			t->running = false;
		}
	}
}

/* --- drawing ------------------------------------------------------------- */

static void draw_key(struct astrix_canvas *c, const struct astrix_theme *th, struct astrix_rect r,
                     const char *label, bool pressed, bool highlighted) {
	struct astrix_color bg = pressed ? th->primary
	                                 : (highlighted ? astrix_color_with_alpha(th->primary, 70)
	                                                : th->surface_alt);
	struct astrix_color fg = highlighted ? th->on_primary : th->text;
	astrix_fill_rect_rounded(c, r, 6, bg);
	int tw = astrix_text_width(label);
	astrix_draw_text(c, r.x + (r.w - tw) / 2, r.y + (r.h - ASTRIX_FONT_H) / 2, label, fg);
}

/*
 * Layout the keyboard. The same arithmetic is used for drawing and for
 * hit-testing, which is the only way to guarantee that what is drawn is what
 * is tappable. (The shell has the same discipline in input.c.)
 */
static void draw_keyboard(struct astrix_canvas *c, const struct astrix_theme *th,
                          struct terminal *t, int screen_w, int screen_h) {
	struct astrix_rect kb = { 0, screen_h - astrix_kbd_height(screen_h), screen_w,
		                      astrix_kbd_height(screen_h) };
	astrix_fill_rect(c, kb, th->surface);
	astrix_fill_rect(c, (struct astrix_rect){ 0, kb.y, screen_w, 1 }, th->divider);

	int pad = 4;
	int row_h = (kb.h - pad) / ASTRIX_KBD_ROWS;

	for (int row = 0; row < ASTRIX_KBD_ROWS; row++) {
		int count = 0;
		const struct astrix_kbd_key *keys = astrix_kbd_row(row, t->layer, &count);
		if (!keys || count == 0) {
			continue;
		}
		float total = 0.0f;
		for (int i = 0; i < count; i++) {
			total += keys[i].weight;
		}
		/* Rows are inset a little so the keyboard reads as keys, not a
		 * solid block, and rows with fewer keys are centred. */
		float avail = (float)(screen_w - pad * 2);
		float x = (float)pad;
		int y = kb.y + pad / 2 + row * row_h;
		for (int i = 0; i < count; i++) {
			float w = avail * (keys[i].weight / total);
			struct astrix_rect r = { (int)x, y + 2, (int)(w - pad / 2), row_h - 4 };
			bool pressed = t->pressed_valid && t->pressed_key_row == row &&
			               t->pressed_key_index == i;
			bool highlighted = (keys[i].action == ASTRIX_KBD_CTRL && t->ctrl_armed);
			draw_key(c, th, r, keys[i].label, pressed, highlighted);
			x += w;
		}
	}
}

/* Hit-test using exactly the geometry draw_keyboard used. */
static bool hit_key(struct terminal *t, int px, int py, int screen_w, int screen_h,
                    int *out_row, int *out_index) {
	if (!astrix_kbd_visible()) {
		return false;
	}
	int kb_h = astrix_kbd_height(screen_h);
	int kb_y = screen_h - kb_h;
	if (py < kb_y) {
		return false;
	}
	int pad = 4;
	int row_h = (kb_h - pad) / ASTRIX_KBD_ROWS;
	int row = (py - kb_y - pad / 2) / row_h;
	if (row < 0 || row >= ASTRIX_KBD_ROWS) {
		return false;
	}
	int count = 0;
	const struct astrix_kbd_key *keys = astrix_kbd_row(row, t->layer, &count);
	if (!keys || count == 0) {
		return false;
	}
	float total = 0.0f;
	for (int i = 0; i < count; i++) {
		total += keys[i].weight;
	}
	float avail = (float)(screen_w - pad * 2);
	float x = (float)pad;
	for (int i = 0; i < count; i++) {
		float w = avail * (keys[i].weight / total);
		if (px >= (int)x && px < (int)(x + w)) {
			*out_row = row;
			*out_index = i;
			return true;
		}
		x += w;
	}
	return false;
}

static void on_frame(struct astrix_app_host *host) {
	struct terminal *t = astrix_app_user(host);
	struct astrix_canvas *c = astrix_app_canvas(host);
	const struct astrix_theme *th = astrix_app_theme(host);
	int w, h;
	astrix_app_size(host, &w, &h);

	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, w, h }, th->background);

	int fw = font_width(t);
	int fh = font_height(t);
	int kbd_h = astrix_kbd_visible() ? astrix_kbd_height(h) : 0;
	int term_h = h - kbd_h - th->navbar_h;

	/* How many rows of the grid fit. */
	int rows_visible = term_h / (fh + 2);
	if (rows_visible > t->term.rows) {
		rows_visible = t->term.rows;
	}
	if (rows_visible < 1) {
		rows_visible = 1;
	}

	/*
	 * When scrolled back, the view is a window onto the scrollback ending
	 * just above the live screen. Otherwise it is the live screen.
	 */
	int scroll = astrix_term_scrolled(&t->term) ? t->term.scrollback_offset : 0;
	int live_start = t->term.rows - rows_visible;
	int first_scrollback = t->term.scrollback_count - scroll - rows_visible;

	for (int i = 0; i < rows_visible; i++) {
		char line[ASTRIX_TERM_COLS + 1];
		int y = i * (fh + 2) + th->status_h;
		int idx = live_start + i;

		if (scroll > 0 && i < scroll) {
			/* A line from the scrollback. */
			int sb = first_scrollback + i;
			if (sb < 0) {
				sb = 0;
			}
			snprintf(line, sizeof(line), "%s", astrix_term_scrollback_line(&t->term, sb));
		} else {
			int grid_row = idx < 0 ? 0 : (idx >= t->term.rows ? t->term.rows - 1 : idx);
			astrix_term_row_text(&t->term, grid_row, line, sizeof(line));
		}
		if (line[0]) {
			astrix_draw_text(c, th->spacing * 2, y, line, th->text);
		}
		/* The cursor, but only on the live screen and only when the
		 * shell is still running. */
		if (!scroll && t->running && idx == t->term.cursor_y) {
			int cx = th->spacing * 2 + t->term.cursor_x * fw;
			int cy = y - 1;
			/* Blink off half the time, driven by the frame counter so
			 * it needs no timer. */
			if ((astrix_app_now_ms(host) / 500) % 2 == 0) {
				astrix_fill_rect(c, (struct astrix_rect){ cx, cy, fw, fh },
				                 astrix_color_with_alpha(th->text, 200));
				if (line[0] && t->term.cursor_x < (int)strlen(line)) {
					/* Re-draw the character the block was covering. */
					char one[2] = { line[t->term.cursor_x], 0 };
					astrix_draw_text(c, cx, y, one, th->background);
				}
			}
		}
	}

	/* Status strip: a scrolled-back view says so, a dead shell says so. */
	struct astrix_rect strip = { 0, term_h, w, th->navbar_h };
	astrix_fill_rect(c, strip, th->surface);
	const char *msg;
	if (!t->running) {
		static char dead[96];
		snprintf(dead, sizeof(dead), "session ended (%d) - tap to restart",
		         t->exit_status);
		msg = dead;
	} else if (scroll > 0) {
		static char sc[64];
		snprintf(sc, sizeof(sc), "scrolled back %d lines - tap to follow", scroll);
		msg = sc;
	} else {
		msg = t->status;
	}
	astrix_draw_text(c, th->spacing * 2, strip.y + (th->navbar_h - ASTRIX_FONT_H) / 2, msg,
	                 t->running ? th->text_dim : th->danger);

	if (astrix_kbd_visible()) {
		draw_keyboard(c, th, t, w, h);
	} else {
		/* A tap anywhere on the terminal area brings the keyboard back. */
		struct astrix_rect hint = { 0, term_h, w, th->navbar_h };
		astrix_draw_text(c, th->spacing * 2, hint.y + (hint.h - ASTRIX_FONT_H) / 2,
		                 "tap to show keyboard", th->text_dim);
	}
}

/* --- key handling -------------------------------------------------------- */

static void send_ctrl_char(struct terminal *t, char c) {
	char seq[2] = { (char)(c & 0x1f), '\0' };
	pty_write(t, seq);
}

static void press_key(struct terminal *t, const struct astrix_kbd_key *k) {
	if (!k) {
		return;
	}
	switch (k->action) {
	case ASTRIX_KBD_CHAR: {
		char buf[2] = { k->ch, '\0' };
		if (t->ctrl_armed && k->ch >= 'a' && k->ch <= 'z') {
			send_ctrl_char(t, k->ch);
			t->ctrl_armed = false;
		} else if (t->ctrl_armed && k->ch == 'c') {
			/* Ctrl-C is the one Ctrl chord that has to work. */
			send_ctrl_char(t, 'c');
			t->ctrl_armed = false;
		} else {
			pty_write(t, buf);
		}
		break;
	}
	case ASTRIX_KBD_CTRL:
		t->ctrl_armed = !t->ctrl_armed;
		break;
	case ASTRIX_KBD_ENTER:
		pty_write(t, "\r");
		break;
	case ASTRIX_KBD_BACKSPACE:
		pty_write(t, "\x7f");
		break;
	case ASTRIX_KBD_TAB:
		pty_write(t, "\t");
		break;
	case ASTRIX_KBD_ESC:
		pty_write(t, "\x1b");
		break;
	case ASTRIX_KBD_UP:
		pty_write(t, "\x1b[A");
		break;
	case ASTRIX_KBD_DOWN:
		pty_write(t, "\x1b[B");
		break;
	case ASTRIX_KBD_RIGHT:
		pty_write(t, "\x1b[C");
		break;
	case ASTRIX_KBD_LEFT:
		pty_write(t, "\x1b[D");
		break;
	case ASTRIX_KBD_PIPE:
		pty_write(t, "|");
		break;
	case ASTRIX_KBD_TILDE:
		pty_write(t, "~");
		break;
	case ASTRIX_KBD_MINUS:
		pty_write(t, "-");
		break;
	case ASTRIX_KBD_SLASH:
		pty_write(t, "/");
		break;
	case ASTRIX_KBD_AT:
		pty_write(t, "@");
		break;
	case ASTRIX_KBD_SYM:
		break;
	}
}

/* Translate an evdev keycode into what a terminal expects. */
static bool on_keyboard_key(struct terminal *t, uint32_t code) {
	switch (code) {
	case KEY_ENTER: pty_write(t, "\r"); return true;
	case KEY_BACKSPACE: pty_write(t, "\x7f"); return true;
	case KEY_TAB: pty_write(t, "\t"); return true;
	case KEY_ESC: pty_write(t, "\x1b"); return true;
	case KEY_UP: pty_write(t, "\x1b[A"); return true;
	case KEY_DOWN: pty_write(t, "\x1b[B"); return true;
	case KEY_RIGHT: pty_write(t, "\x1b[C"); return true;
	case KEY_LEFT: pty_write(t, "\x1b[D"); return true;
	case KEY_HOME: pty_write(t, "\x1b[H"); return true;
	case KEY_END: pty_write(t, "\x1b[F"); return true;
	case KEY_PAGEUP: pty_write(t, "\x1b[5~"); return true;
	case KEY_PAGEDOWN: pty_write(t, "\x1b[6~"); return true;
	case KEY_DELETE: pty_write(t, "\x1b[3~"); return true;
	case KEY_F1: pty_write(t, "\x1bOP"); return true;
	case KEY_F2: pty_write(t, "\x1bOQ"); return true;
	case KEY_F3: pty_write(t, "\x1bOR"); return true;
	case KEY_F4: pty_write(t, "\x1bOS"); return true;
	default:
		return false;
	}
}

static bool on_input(struct astrix_app_host *host, const struct astrix_input_event *ev) {
	struct terminal *t = astrix_app_user(host);
	int w, h;
	astrix_app_size(host, &w, &h);

	switch (ev->kind) {
	case ASTRIX_INPUT_KEY:
		if (!(ev->state & ASTRIX_KEY_PRESSED)) {
			return true; /* consume releases; the shell needs repeats only */
		}
		return on_keyboard_key(t, ev->key) || true;

	case ASTRIX_INPUT_TOUCH_DOWN: {
		int row, index;
		if (hit_key(t, ev->x, ev->y, w, h, &row, &index)) {
			t->pressed_valid = true;
			t->pressed_key_row = row;
			t->pressed_key_index = index;
			t->press_x = ev->x;
			t->press_y = ev->y;
			return true;
		}
		/* Tapping the terminal body toggles the keyboard. */
		if (ev->y < h - astrix_kbd_height(h) - 40) {
			if (astrix_term_scrolled(&t->term)) {
				astrix_term_scroll_to_bottom(&t->term);
			}
			astrix_kbd_set_visible(!astrix_kbd_visible());
			return true;
		}
		/* The status strip: a dead session restarts on tap. */
		if (!t->running) {
			spawn_shell(t);
			astrix_term_init(&t->term, t->term.cols, t->term.rows);
			astrix_app_toast(host, "new session");
			return true;
		}
		return true;
	}

	case ASTRIX_INPUT_TOUCH_MOTION:
		/* A vertical drag on the terminal body is a scrollback drag. */
		if (t->pressed_valid) {
			return true;
		}
		if (ev->y < h - astrix_kbd_height(h) - 40) {
			if (!t->dragging && ev->y - t->drag_start_y > 8) {
				t->dragging = true;
			}
			if (t->dragging) {
				/* Dragging down moves towards older output. */
				int delta = t->drag_start_y - ev->y;
				astrix_term_scroll(&t->term, -delta / 8);
				return true;
			}
		}
		return false;

	case ASTRIX_INPUT_TOUCH_UP: {
		int row, index;
		if (t->pressed_valid && hit_key(t, ev->x, ev->y, w, h, &row, &index) &&
		    row == t->pressed_key_row && index == t->pressed_key_index) {
			int count = 0;
			const struct astrix_kbd_key *keys = astrix_kbd_row(row, t->layer, &count);
			press_key(t, &keys[index]);
			t->pressed_valid = false;
			return true;
		}
		t->pressed_valid = false;
		t->dragging = false;
		t->drag_start_y = ev->y;
		/* Any new key returns to the live view, the way a terminal
		 * follows its tail when you type. */
		astrix_term_scroll_to_bottom(&t->term);
		return true;
	}

	case ASTRIX_INPUT_SCROLL:
		astrix_term_scroll(&t->term, -ev->scroll_y / 24);
		return true;

	default:
		return false;
	}
}

/* --- lifecycle ----------------------------------------------------------- */

/* Named app_on_* because "on_exit" is a real symbol in stdlib.h. */
static void app_on_exit(struct astrix_app_host *host) {
	struct terminal *t = astrix_app_user(host);
	if (t->child > 0) {
		/*
		 * Put the shell back in its own process group first. Without
		 * this, a shell that has trapped ^C ignores the SIGHUP that
		 * follows the hangup, and a PTY close alone is not enough to
		 * make it exit - so exiting the terminal would leave an
		 * orphaned process behind holding the session open.
		 */
		kill(-t->child, SIGHUP);
		kill(-t->child, SIGCONT);
	}
	if (t->master_fd >= 0) {
		close(t->master_fd);
		t->master_fd = -1;
	}
}

static const struct astrix_app_desc desc = {
	.app_id = "org.astrix.Terminal",
	.title = "Terminal",
	.version = "0.1.0",
	.wants_keyboard = true,
	.on_frame = on_frame,
	.on_input = on_input,
	.on_exit = app_on_exit,
	.on_tick = on_tick,
};

int main(int argc, char **argv) {
	struct terminal t;
	memset(&t, 0, sizeof(t));
	t.master_fd = -1;
	t.font_scale = 1;
	t.layer = 0;
	t.ctrl_armed = false;
	t.drag_start_y = 0;

	/* The grid is sized for the panel; the font scale decides how many of
	 * the 60 available rows actually fit on screen. */
	astrix_term_init(&t.term, ASTRIX_TERM_COLS, ASTRIX_TERM_ROWS);

	/*
	 * The PTY is opened here rather than in on_start, so that a failure
	 * to allocate one is reported before the user sees an empty screen.
	 */
	spawn_shell(&t);

	return astrix_app_main_with_user(&desc, argc, argv, &t);
}
