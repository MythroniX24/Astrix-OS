/*
 * Astrix OS - Astrix Shell rendering.
 *
 * Draws the complete system UI into the shell's shm buffer. Everything is
 * immediate-mode: state lives in struct astrix_shell, and each frame is drawn
 * from it. A phone UI is small enough that this is both fast and far easier to
 * reason about than a retained widget tree, and it means a frame is always
 * internally consistent.
 *
 * Layout is portrait-first and derived from the panel size, so the same code
 * produces a sensible layout at 720x1600 and 1080x2400.
 */

#include "astrix_shell.h"
#include "keyboard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- layout -------------------------------------------------------------- */

/* Grid geometry for the home screen / app drawer. */
struct grid {
	int cols;
	int rows;
	int cell_w;
	int cell_h;
	int origin_x;
	int origin_y;
};

static struct grid compute_grid(const struct astrix_shell *sh, int top, int bottom) {
	struct grid g;
	int w = sh->width;
	/* Four columns is the sweet spot on a phone: enough density to be
	 * useful, wide enough that icons stay tappable. */
	g.cols = 4;
	int cell = w / (g.cols + 1);
	g.cell_w = cell;
	g.cell_h = cell + 12;
	g.rows = (bottom - top) / g.cell_h;
	if (g.rows < 1) {
		g.rows = 1;
	}
	int grid_h = g.rows * g.cell_h;
	g.origin_x = (w - g.cols * g.cell_w) / 2;
	g.origin_y = top + ((bottom - top) - grid_h) / 2;
	if (g.origin_y < top) {
		g.origin_y = top;
	}
	return g;
}

/* Width of one home-screen cell, shared by the grid and the dock. */
static int g_cell_width_for(const struct astrix_shell *sh) {
	return sh->width / 5;
}

static struct astrix_rect cell_rect(const struct grid *g, int index) {
	int col = index % g->cols;
	int row = index / g->cols;
	return (struct astrix_rect){ g->origin_x + col * g->cell_w, g->origin_y + row * g->cell_h,
		                         g->cell_w, g->cell_h };
}

/* Width available for an app label: a cell is ~w/5, so leave margins. */
static int g_cell_label_width(int cell_w) {
	int avail = cell_w - 12;
	return avail > 0 ? avail : 1;
}

/* Colour derived from an app id, so each app is visually stable and distinct. */
static struct astrix_color app_color(const struct astrix_app *app) {
	unsigned h = 5381;
	for (const char *p = app->id; *p; p++) {
		h = h * 33 + (unsigned char)*p;
	}
	return (struct astrix_color){ (uint8_t)(80 + h % 120), (uint8_t)(80 + (h >> 8) % 120),
		                          (uint8_t)(110 + (h >> 16) % 110), 0xFF };
}

/* Draws a small icon tile plus the app's name. Used by the switcher cards. */
static void draw_app_header(struct astrix_canvas *c, const struct astrix_shell *sh,
                            struct astrix_rect r, const struct astrix_app *app) {
	const struct astrix_theme *t = sh->theme;
	int size = r.h;
	astrix_fill_rect_rounded(c, (struct astrix_rect){ r.x, r.y, size, size }, size / 4,
	                         app_color(app));
	char initial[2] = { app->name[0] ? app->name[0] : '?', 0 };
	int iw = astrix_text_width(initial);
	astrix_draw_text(c, r.x + (size - iw) / 2, r.y + (size - ASTRIX_FONT_H) / 2, initial,
	                 t->on_primary);

	char label[ASTRIX_APP_NAME_LEN];
	astrix_text_ellipsize(app->name, r.w - size - 10, label, sizeof(label));
	astrix_draw_text(c, r.x + size + 10, r.y + (size - ASTRIX_FONT_H) / 2, label, t->text);
}

/*
 * App icons. With no icon theme installed in the QEMU milestone, an icon is
 * drawn as a rounded tile with the app's initial(s) - a real, deterministic
 * rendering, and clearly a placeholder rather than a fake icon asset.
 */
static void draw_app_icon(struct astrix_canvas *c, const struct astrix_shell *sh,
                          struct astrix_rect r, const struct astrix_app *app) {
	const struct astrix_theme *t = sh->theme;
	struct astrix_color base = app_color(app);

	int icon = r.w < r.h ? r.w : r.h;
	struct astrix_rect ir = { r.x + (r.w - icon) / 2, r.y + (icon > 56 ? 6 : 0), icon, icon };
	astrix_fill_rect_rounded(c, ir, icon / 4, base);

	/* Initial letter, centred. */
	char initial[2] = { (app->name[0] ? app->name[0] : '?'), 0 };
	int tw = astrix_text_width(initial);
	astrix_draw_text(c, ir.x + (icon - tw) / 2, ir.y + (icon - ASTRIX_FONT_H) / 2, initial,
	                 t->on_primary);

	/* Label under the icon. */
	char label[ASTRIX_APP_NAME_LEN];
	astrix_text_ellipsize(app->name, g_cell_label_width(r.w), label, sizeof(label));
	int lw = astrix_text_width(label);
	astrix_draw_text(c, r.x + (r.w - lw) / 2, ir.y + icon + 6, label, t->text);
}

/* --- status bar ---------------------------------------------------------- */

static void draw_status_bar(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;
	int h = t->status_h;
	struct astrix_rect r = { 0, 0, sh->width, h };

	/* The bar sits over whatever is behind it, so a subtle scrim keeps the
	 * clock legible on a light wallpaper. */
	astrix_fill_rect(c, r, astrix_color_with_alpha(t->background, 0xE0));

	int pad = 12;
	int y = (h - ASTRIX_FONT_H) / 2;

	/* Left: time. A phone leads with the clock. */
	astrix_draw_text(c, pad, y, sh->status_time, t->text);

	/* The keyboard button, immediately right of the clock. Its geometry is
	 * shared with the hit-test in input.c via astrix_kbd_toggle_rect(), so
	 * the thing drawn and the thing tappable cannot drift apart. */
	struct astrix_rect kbtn = astrix_kbd_toggle_rect(sh);
	struct astrix_color kb_col = sh->kbd_visible ? t->primary : t->text_dim;
	astrix_stroke_rect_rounded(c, kbtn, 4, 1, kb_col);
	/* A little keyboard glyph: three ticks over a bar. */
	int kx = kbtn.x + 5, ky = kbtn.y + 5;
	for (int i = 0; i < 3; i++) {
		astrix_fill_rect(c, (struct astrix_rect){ kx + i * 5, ky, 3, 3 }, kb_col);
	}
	astrix_fill_rect(c,
	                 (struct astrix_rect){ kx, kbtn.y + kbtn.h - 7,
	                                       kbtn.w - 10, 2 },
	                 kb_col);

	/* Right: status glyphs, right-aligned and packed from the edge. */
	int x = sh->width - pad;
	const struct astrix_theme *th = t;

	/* Battery. Unknown battery is shown as "--", never as a fake 0 or 100. */
	if (sh->battery_percent >= 0) {
		char pct[8];
		snprintf(pct, sizeof(pct), "%d%%", sh->battery_percent);
		int pw = astrix_text_width(pct);
		astrix_draw_text(c, x - pw, y, pct, th->text);
		x -= pw + 6;
		/* Battery body. */
		struct astrix_rect bat = { x - 16, y + 2, 14, 7 };
		astrix_stroke_rect(c, bat, 1, th->text_dim);
		astrix_fill_rect(c, (struct astrix_rect){ bat.x + bat.w, bat.y + 2, 2, 3 }, th->text_dim);			if (sh->battery_percent > 0) {
				int fill = (bat.w - 3) * sh->battery_percent / 100;
				if (fill > bat.w - 3) {
					fill = bat.w - 3;
				}
			struct astrix_color bc = th->success;
			if (sh->battery_percent <= 15 && !sh->charging) {
				bc = th->danger;
			}
			astrix_fill_rect(c, (struct astrix_rect){ bat.x + 2, bat.y + 2, fill, bat.h - 3 }, bc);
		}
		if (sh->charging) {
			/* A bolt is drawn as a filled triangle. */
			for (int i = 0; i < 6; i++) {
				astrix_fill_rect(c,
				                 (struct astrix_rect){ x - 4 - i / 2, y + 1 + i, 3, 1 },
				                 th->success);
			}
		}
		x -= 24;
	} else {
		const char *unk = "n/a";
		int uw = astrix_text_width(unk);
		astrix_draw_text(c, x - uw, y, unk, th->text_dim);
		x -= uw + 8;
	}
	(void)x;

	/* Wi-Fi: drawn only when the hardware actually reports it. */
	if (sh->wifi_available) {
		if (sh->wifi_on) {
			/* Three arcs approximated by stacked bars. */
			for (int i = 0; i < 3; i++) {
				int bw = 2 + i * 3;
				astrix_fill_rect(c, (struct astrix_rect){ x - bw, y + 8 - i * 3, bw, 2 + i * 3 },
				                 th->text);
			}
		} else {
			astrix_stroke_rect(c, (struct astrix_rect){ x - 8, y + 4, 8, 5 }, 1, th->text_dim);
		}
		x -= 16;
	}
	/* Bluetooth, same rule: only if the controller exists. */
	if (sh->bt_available && sh->bt_on) {
		astrix_draw_text(c, x - 6, y, "B", th->primary);
		x -= 14;
	}

	/* Unread notification dot, right of the glyphs. */
	int unread = astrix_shell_unread_count(sh);
	if (unread > 0) {
		astrix_fill_rect_rounded(c, (struct astrix_rect){ x - 14, y + 1, 13, 10 }, 5,
		                         th->primary);
		char n[4];
		snprintf(n, sizeof(n), "%d", unread > 9 ? 9 : unread);
		int nw = astrix_text_width(n);
		astrix_draw_text(c, x - 14 + (13 - nw) / 2, y + 1, n, th->on_primary);
		x -= 20;
	}

	/* Bottom hairline. */
	astrix_fill_rect(c, (struct astrix_rect){ 0, h - 1, sh->width, 1 }, t->divider);
}

/* --- navigation bar ------------------------------------------------------ */

static void draw_nav_bar(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;
	int h = t->navbar_h;
	int y = sh->height - h;
	astrix_fill_rect(c, (struct astrix_rect){ 0, y, sh->width, h },
	                 astrix_color_with_alpha(t->background, 0xE0));

	/* Gesture pill: the phone's home affordance. */
	int pill_w = 108, pill_h = 4;
	astrix_fill_rect_rounded(c,
	                         (struct astrix_rect){ (sh->width - pill_w) / 2,
	                                               y + h - 14, pill_w, pill_h },
	                         pill_h / 2, t->text_dim);
}

/* --- home screen --------------------------------------------------------- */

static void draw_wallpaper(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;
	/*
	 * A deterministic vertical gradient derived from the theme. Real OSes
	 * ship wallpaper images; until then this is a plain, honest fill rather
	 * than a fake photograph.
	 */
	struct astrix_color top = sh->dark_mode ? astrix_rgba(0x14, 0x1B, 0x3A, 0xFF)
	                                        : astrix_rgba(0xDD, 0xE6, 0xFF, 0xFF);
	struct astrix_color bottom = sh->dark_mode ? astrix_rgba(0x08, 0x0B, 0x14, 0xFF)
	                                           : astrix_rgba(0xF6, 0xF2, 0xEA, 0xFF);
	astrix_fill_rect_gradient_v(c, (struct astrix_rect){ 0, 0, sh->width, sh->height }, top,
	                            bottom);
}

static void draw_home(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;
	draw_wallpaper(c, sh);

	int top = t->status_h + 16;
	int nav_top = sh->height - t->navbar_h;
	/*
	 * The dock lives in its own band pinned above the navigation bar; the
	 * app grid occupies the space above it. On the dock page the grid area
	 * is empty and only the dock is drawn, which is how a phone home
	 * screen behaves.
	 */
	int dock_h = g_cell_width_for(sh) + 24;
	int bottom = nav_top - (sh->home_page == 0 ? 0 : dock_h + 8);

	/* Clock widget: a phone's home screen leads with time. */
	char big[16];
	snprintf(big, sizeof(big), "%s", sh->status_time);
	int bw = astrix_text_width(big);
	astrix_draw_text(c, (sh->width - bw) / 2, top + 8, big, t->text);

	const char *pname = sh->home_page == 0 ? "Dock" : (sh->home_page == 1 ? "Apps" : "More");
	int pw = astrix_text_width(pname);
	astrix_draw_text(c, (sh->width - pw) / 2, top + 28, pname, t->text_dim);

	/* Page indicator. */
	int pages = 3;
	int dot_y = bottom - 6;
	int total_w = pages * 14 - 4;
	int dx = (sh->width - total_w) / 2;
	for (int i = 0; i < pages; i++) {
		struct astrix_color dc = (i == sh->home_page) ? t->primary : t->divider;
		astrix_fill_rect_rounded(c, (struct astrix_rect){ dx + i * 14, dot_y, 10, 4 }, 2, dc);
	}

	/* Apps on the current page, in registry order. */
	int on_page = 0;
	for (int i = 0; i < sh->app_count; i++) {
		const struct astrix_app *a = &sh->apps[i];
		int page = a->pinned ? 0 : a->page;
		if (page != sh->home_page) {
			continue;
		}		if (page == 0) {
			/* Dock: a single row pinned to the bottom. ASTRIX_DOCK_COLS
			 * is the shared constant; input.c hit-tests the same row. */
			int cell = g_cell_width_for(sh);
			struct astrix_rect dr = {
				(sh->width - ASTRIX_DOCK_COLS * (cell + 10)) / 2 + on_page * (cell + 10),
				nav_top - dock_h, cell, dock_h
			};
			draw_app_icon(c, sh, dr, a);
		} else {
			struct grid g = compute_grid(sh, top + 48, bottom - 14);
			struct astrix_rect cr = cell_rect(&g, on_page);
			draw_app_icon(c, sh, cr, a);
		}
		on_page++;
	}
	if (on_page == 0) {
		const char *msg = "No apps on this page";
		int mw = astrix_text_width(msg);
		astrix_draw_text(c, (sh->width - mw) / 2, sh->height / 2, msg, t->text_dim);
	}
}

/* --- app drawer (all apps) ----------------------------------------------- */

static void draw_launcher(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;
	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, sh->width, sh->height }, t->background);

	int top = t->status_h + 12;
	/* Title */
	const char *title = "All apps";
	astrix_draw_text(c, 20, top, title, t->text);
	astrix_draw_text(c, sh->width - 20 - astrix_text_width("system"),
	                 top, "system", t->text_dim);

	int bottom = sh->height - t->navbar_h - 8;
	struct grid g = compute_grid(sh, top + 32, bottom);

	/* Scroll: the drawer is a list, so a scroll offset is applied. */
	int row_h = g.cell_h;
	g.origin_y -= sh->launcher_scroll;

	for (int i = 0; i < sh->app_count; i++) {
		struct astrix_rect cr = cell_rect(&g, i);
		/* Skip rows fully scrolled out of view. */
		if (cr.y + cr.h < top + 32 || cr.y > bottom) {
			continue;
		}
		draw_app_icon(c, sh, cr, &sh->apps[i]);
	}
	(void)row_h;

	if (sh->app_count == 0) {
		const char *msg = "No applications installed";
		int mw = astrix_text_width(msg);
		astrix_draw_text(c, (sh->width - mw) / 2, sh->height / 2, msg, t->text_dim);
	}
}

/* --- app screen placeholder --------------------------------------------- */

static void draw_app(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;
	/*
	 * When an app is foregrounded its own surface is composited underneath
	 * this pass; here we only draw the system chrome. If no app is actually
	 * mapped yet we show its name rather than a blank screen, so the
	 * compositor's behaviour is visible and debuggable.
	 */
	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, sh->width, sh->height }, t->background);
	if (sh->foreground_app >= 0 && sh->foreground_app < sh->app_count) {
		const struct astrix_app *a = &sh->apps[sh->foreground_app];
		char line[128];
		snprintf(line, sizeof(line), "%s", a->name);
		int lw = astrix_text_width(line);
		astrix_draw_text(c, (sh->width - lw) / 2, sh->height / 2 - 40, line, t->text);

		const char *kind = a->kind == ASTRIX_APP_ANDROID ? "Android app"
		                  : a->kind == ASTRIX_APP_SYSTEM ? "System app"
		                                                 : "Linux app";
		int kw = astrix_text_width(kind);
		astrix_draw_text(c, (sh->width - kw) / 2, sh->height / 2 - 20, kind, t->text_dim);
	}
}

/* --- lock screen --------------------------------------------------------- */

static void draw_lock(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;
	draw_wallpaper(c, sh);

	/* Scrim so the clock reads over the wallpaper. */
	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, sh->width, sh->height },
	                 astrix_rgba(0, 0, 0, sh->dark_mode ? 0x60 : 0x30));

	char time[16];
	snprintf(time, sizeof(time), "%s", sh->status_time);
	int tw = astrix_text_width(time);
	astrix_draw_text(c, (sh->width - tw) / 2, sh->height / 5, time, t->text);

	const char *hint = "Swipe up to unlock";
	int hw = astrix_text_width(hint);
	astrix_draw_text(c, (sh->width - hw) / 2, sh->height / 5 + 24, hint,
	                 astrix_color_with_alpha(t->text, 0xCC));

	/* Battery summary. */
	if (sh->battery_percent >= 0) {
		char b[24];
		snprintf(b, sizeof(b), "%d%%%s", sh->battery_percent,
		         sh->charging ? " charging" : "");
		int bw = astrix_text_width(b);
		astrix_draw_text(c, (sh->width - bw) / 2, sh->height / 5 + 42, b, t->text_dim);
	}
}

/* --- notification centre + quick settings -------------------------------- */

struct toggle_desc {
	const char *label;
	bool enabled;
	bool supported;   /* false = hardware absent: must not be drawn */
	struct astrix_color on_color;
};

static void draw_shade(struct astrix_canvas *c, const struct astrix_shell *sh, float progress) {
	const struct astrix_theme *t = sh->theme;
	int h = (int)(sh->height * 0.82f * progress);
	if (h <= 0) {
		return;
	}
	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, sh->width, h }, t->surface);

	/* Header: date/time. */
	char hdr[32];
	snprintf(hdr, sizeof(hdr), "%s", sh->status_time);
	astrix_draw_text(c, 20, 16, hdr, t->text);

	int y = 48;

	/* --- notifications ------------------------------------------------ */
	int unread = astrix_shell_unread_count(sh);
	if (unread > 0) {
		char label[32];
		snprintf(label, sizeof(label), "Notifications (%d)", unread);
		astrix_draw_text(c, 20, y, label, t->text_dim);
		y += 20;
	}
	if (sh->notification_count == 0) {
		const char *empty = "No notifications";
		astrix_draw_text(c, 20, y + 4, empty, t->text_dim);
		y += 28;
	} else {
		y -= sh->notification_scroll;
		for (int i = 0; i < sh->notification_count && y < h - 200; i++) {
			const struct astrix_notification *n = &sh->notifications[i];
			if (y + 64 < 0) {
				continue;
			}
			struct astrix_rect card = { 14, y, sh->width - 28, 58 };
			astrix_fill_rect_rounded(c, card, t->radius, t->surface_alt);
			/* Priority stripe: critical notifications are unmistakable. */
			if (n->priority >= ASTRIX_NOTIF_HIGH) {
				struct astrix_color pc =
				    n->priority == ASTRIX_NOTIF_CRITICAL ? t->danger : t->warning;
				astrix_fill_rect_rounded(c,
				                         (struct astrix_rect){ card.x, card.y, 4, card.h }, 2,
				                         pc);
			}
			char title[128];
			snprintf(title, sizeof(title), "%s", n->title);
			astrix_draw_text(c, card.x + 14, card.y + 8, title, t->text);
			char body[256];
			char elided[256];
			astrix_text_ellipsize(n->body, sh->width - 70, elided, sizeof(elided));
			snprintf(body, sizeof(body), "%s", elided);
			astrix_draw_text(c, card.x + 14, card.y + 26, body, t->text_dim);
			astrix_draw_text(c, card.x + 14, card.y + 42, n->app_name, t->text_dim);
			y += 66;
		}
	}

	/* --- quick settings tiles ---------------------------------------- */
	int tile_h = 74, tile_w = (sh->width - 42) / 2;
	int ty = h - 210;
	if (ty < y + 12) {
		ty = y + 12;
	}

	/*
	 * Only tiles whose hardware is actually present are drawn. A toggle
	 * that does nothing is worse than no toggle, so unavailable features
	 * are omitted entirely rather than greyed out as decoration.
	 */
	struct toggle_desc tiles[] = {
		{ "Wi-Fi", sh->wifi_on, sh->wifi_available, { 0 } },
		{ "Bluetooth", sh->bt_on, sh->bt_available, { 0 } },
		{ "Do Not Disturb", sh->do_not_disturb, true, { 0 } },
		{ "Airplane Mode", sh->airplane_mode, sh->wifi_available || sh->bt_available, { 0 } },
		{ "Sound", sh->sound_on, true, { 0 } },
		{ "Rotation", !sh->rotation_locked, true, { 0 } },
		{ "Location", sh->location_on, sh->location_available, { 0 } },
		{ "Battery Saver", sh->battery_saver, true, { 0 } },
		{ "Mobile Data", sh->mobile_on, sh->mobile_available, { 0 } },
	};
	int ntiles = (int)(sizeof(tiles) / sizeof(tiles[0]));
	int drawn = 0;
	for (int i = 0; i < ntiles; i++) {
		if (!tiles[i].supported) {
			continue;
		}
		int col = drawn % 2, row = drawn / 2;
		struct astrix_rect tr = { 14 + col * (tile_w + 14), ty + row * (tile_h + 12), tile_w,
		                          tile_h };
		astrix_fill_rect_rounded(c, tr, t->radius,
		                         tiles[i].enabled ? astrix_color_with_alpha(t->primary, 0x33)
		                                          : t->surface_alt);
		astrix_draw_text(c, tr.x + 12, tr.y + (tr.h - ASTRIX_FONT_H) / 2, tiles[i].label,
		                 tiles[i].enabled ? t->primary : t->text_dim);
		/* State pip */
		astrix_fill_rect_rounded(
		    c, (struct astrix_rect){ tr.x + tr.w - 26, tr.y + tr.h / 2 - 8, 16, 16 }, 8,
		    tiles[i].enabled ? t->primary : t->divider);
		drawn++;
	}

	/* --- brightness slider ------------------------------------------- */
	int sy = h - 96;
	if (sy > ty + drawn / 2 * (tile_h + 12) + tile_h) {
		astrix_draw_text(c, 20, sy - 22, "Brightness", t->text_dim);
		struct astrix_rect track = { 20, sy, sh->width - 40, 8 };
		astrix_fill_rect_rounded(c, track, 4, t->divider);
		int fill_w = (track.w * (sh->brightness < 0 ? 0 : sh->brightness)) / 100;
		astrix_fill_rect_rounded(c, (struct astrix_rect){ track.x, track.y, fill_w, track.h },
		                         4, t->primary);
		astrix_fill_rect_rounded(c,
		                         (struct astrix_rect){ track.x + fill_w - 10, track.y - 6, 20,
		                                               20 },
		                         10, t->primary);
	}
}

/* --- app switcher -------------------------------------------------------- */

/*
 * The app switcher shows the running apps as cards, most recent first, so the
 * user can tap to switch or swipe a card away to close it.
 */
static void draw_app_switcher(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;

	int running = 0;
	for (int i = 0; i < sh->app_count; i++) {
		if (sh->apps[i].running) {
			running++;
		}
	}

	if (running == 0) {
		const char *msg = "No recent apps";
		int mw = astrix_text_width(msg);
		astrix_draw_text(c, (sh->width - mw) / 2, sh->height / 2, msg, t->text);
		return;
	}

	/*
	 * Cards are laid out horizontally, the way a phone presents recents.
	 *
	 * The width is a third of the panel plus a margin, and each card is
	 * offset by half its width, so three of them span 2*card_w and stay
	 * entirely on screen. The previous width (width/2 + width/12) with the
	 * same half-width offset put the group at 2.2 widths: the second and
	 * third cards were drawn past the right edge and could never be
	 * reached, so a running app could not be brought forward or closed.
	 */
	int card_w = sh->width / 3 + 40;
	int card_h = (int)(sh->height * 0.52f);
	int step = card_w / 2;
	int x = (sh->width - 2 * card_w) / 2;
	int y = sh->height / 2 - card_h / 2 + 20;

	int idx = 0;
	for (int i = 0; i < sh->app_count; i++) {
		if (!sh->apps[i].running) {
			continue;
		}
		/* Only the first few cards fit on screen; the rest scroll. */
		if (idx >= 3) {
			break;
		}
		struct astrix_rect card = { x + idx * step, y, card_w, card_h };
		astrix_fill_rect_rounded(c, card, t->radius, t->surface);
		astrix_stroke_rect_rounded(c, card, t->radius, 1, t->divider);

		/* Card header: the app's icon tile and name. */
		struct astrix_rect hdr = { card.x + 14, card.y + 14, card.w - 28, 44 };
		draw_app_header(c, sh, hdr, &sh->apps[i]);

		/* Body placeholder: the app's own surface is composited under this
		 * pass, so the card body stays empty by design. */
		astrix_fill_rect_rounded(
		    c, astrix_rect_inset(card, 8, 70), t->radius - 4,
		    astrix_color_with_alpha(t->background, 0x40));
		idx++;
	}
}

/* --- power menu ---------------------------------------------------------- */

static void draw_power_menu(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;
	/* Scrim over the current screen. */
	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, sh->width, sh->height }, t->overlay);

	int w = sh->width - 60;
	struct astrix_rect card = { 30, sh->height / 2 - 130, w, 260 };
	astrix_fill_rect_rounded(c, card, t->radius + 4, t->surface);

	const char *title = "Power";
	int tw = astrix_text_width(title);
	astrix_draw_text(c, card.x + (card.w - tw) / 2, card.y + 20, title, t->text);

	const char *items[] = { "Sleep", "Restart", "Power off", "Cancel" };
	for (int i = 0; i < 4; i++) {
		struct astrix_rect ir = { card.x + 16, card.y + 56 + i * 48, card.w - 32, 40 };
		astrix_draw_text(c, ir.x + (ir.w - astrix_text_width(items[i])) / 2, ir.y + 14, items[i],
		                 i == 3 ? t->text_dim : t->text);
	}
}

/* --- entry point --------------------------------------------------------- */

/*
 * The on-screen keyboard.
 *
 * Drawn over whatever is behind it, with a scrim, because it is modal: while
 * it is up the user is typing into something and the content underneath is
 * context, not something to interact with. The hit-test lives in keyboard.c
 * and reads the same layout function this draws from.
 */
static void draw_keyboard(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;
	struct astrix_rect area = astrix_kbd_area(sh->width, sh->height);

	astrix_fill_rect(c, area, astrix_color_with_alpha(t->background, 0xF2));

	struct astrix_kbd_key keys[ASTRIX_KBD_MAX_KEYS];
	int n = astrix_kbd_layout(sh, sh->width, sh->height, keys, ASTRIX_KBD_MAX_KEYS);
	for (int i = 0; i < n; i++) {
		bool held = sh->kbd_press_x >= 0 &&
		            astrix_rect_contains(keys[i].rect, sh->kbd_press_x,
		                                 sh->kbd_press_y);
		bool accent = keys[i].action == ASTRIX_KBD_SHIFT ||
		              keys[i].action == ASTRIX_KBD_SYMBOLS ||
		              keys[i].action == ASTRIX_KBD_HIDE ||
		              keys[i].action == ASTRIX_KBD_ENTER;
		struct astrix_color bg = held ? t->primary
		                              : (accent ? t->surface_alt : t->surface);
		astrix_fill_rect_rounded(c, keys[i].rect, 6, bg);
		int tw = astrix_text_width(keys[i].label);
		astrix_draw_text(c,
		                 keys[i].rect.x + (keys[i].rect.w - tw) / 2,
		                 keys[i].rect.y + (keys[i].rect.h - ASTRIX_FONT_H) / 2,
		                 keys[i].label, t->text);
	}

	/* Which layer and which modifier are active, stated rather than implied:
	 * a symbol layer that looks identical to letters is a bug report. */
	char state[48];
	snprintf(state, sizeof(state), "%s%s%s",
	         sh->kbd_symbols ? "123" : "abc",
	         sh->kbd_caps ? " CAPS" : (sh->kbd_shift ? " SHIFT" : ""),
	         "");
	astrix_draw_text(c, area.x + 10, area.y - ASTRIX_FONT_H - 4, state, t->text_dim);
}

void astrix_shell_draw(struct astrix_shell *sh) {
	if (!sh || !sh->pixels) {
		return;
	}
	struct astrix_canvas *c = &sh->canvas;
	const struct astrix_theme *t = sh->theme;
	sh->frame_count++;
	sh->needs_redraw = false;

	astrix_clear(c, t->background);

	switch (sh->screen) {
	case ASTRIX_SCREEN_LOCK:
		draw_lock(c, sh);
		break;
	case ASTRIX_SCREEN_HOME:
		draw_home(c, sh);
		break;
	case ASTRIX_SCREEN_LAUNCHER:
		draw_launcher(c, sh);
		break;
	case ASTRIX_SCREEN_APP:
		draw_app(c, sh);
		break;
	case ASTRIX_SCREEN_NOTIFICATION:
	case ASTRIX_SCREEN_QUICK_SETTINGS:
		draw_home(c, sh);
		draw_shade(c, sh, 1.0f);
		break;
	case ASTRIX_SCREEN_APP_SWITCHER:
		draw_home(c, sh);
		/* Scrim, then the recents cards on top. */
		astrix_fill_rect(c, (struct astrix_rect){ 0, 0, sh->width, sh->height },
		                 astrix_color_with_alpha(t->overlay, 0xCC));
		draw_app_switcher(c, sh);
		break;
	case ASTRIX_SCREEN_POWER_MENU:
		draw_home(c, sh);
		draw_power_menu(c, sh);
		break;
	}

	/* Chrome is drawn on top of every screen except the shade, which is
	 * itself full-height. */
	if (sh->screen != ASTRIX_SCREEN_NOTIFICATION &&
	    sh->screen != ASTRIX_SCREEN_QUICK_SETTINGS) {
		draw_status_bar(c, sh);
		draw_nav_bar(c, sh);
	}

	/* The keyboard is chrome too: it covers the nav bar when it is up,
	 * which is correct - a keyboard you can scroll the dock behind is a
	 * keyboard with phantom taps. */
	if (sh->kbd_visible && sh->screen != ASTRIX_SCREEN_APP_SWITCHER &&
	    sh->screen != ASTRIX_SCREEN_POWER_MENU) {
		draw_keyboard(c, sh);
	}
}
