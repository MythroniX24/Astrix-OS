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
}/* Colour derived from an app id, so each app is visually stable and distinct. */
static struct astrix_color app_color(const struct astrix_app *app) {
	unsigned h = 5381;
	for (const char *p = app->id; *p; p++) {
		h = h * 33 + (unsigned char)*p;
	}
	return (struct astrix_color){ (uint8_t)(80 + h % 120), (uint8_t)(80 + (h >> 8) % 120),
	                          (uint8_t)(110 + (h >> 16) % 110), 0xFF };
}

/*
 * Which icon an app gets.
 *
 * A letter in a coloured square is the universal "this build is unfinished"
 * signal, and it is what every launcher looks like before anyone draws
 * anything. The set below is small and deliberate: an icon you recognise at a
 * glance beats a letter you have to read. Unknown apps fall back to the grid,
 * which is honest - it means "we have no icon for this", not "this is a
 * picture of a folder".
 */
static enum astrix_glyph app_glyph(const struct astrix_app *app) {
	const char *id = app->id ? app->id : "";
	const char *name = app->name ? app->name : "";
	if (strstr(id, "files") || strstr(name, "Files")) {
		return ASTRIX_GLYPH_FOLDER;
	}
	if (strstr(id, "terminal") || strstr(name, "Terminal")) {
		return ASTRIX_GLYPH_TERMINAL;
	}
	if (strstr(id, "settings") || strstr(name, "Settings")) {
		return ASTRIX_GLYPH_SETTINGS;
	}
	if (strstr(id, "package-manager") || strstr(id, "package_manager") ||
	    strstr(name, "App Store")) {
		return ASTRIX_GLYPH_STORE;
	}
	if (strstr(id, "apk") || strstr(name, "Android")) {
		return ASTRIX_GLYPH_ANDROID;
	}
	if (strstr(id, "sysinfo") || strstr(name, "System Info")) {
		return ASTRIX_GLYPH_INFO;
	}
	return ASTRIX_GLYPH_GRID;
}

/* Two-stop accent per app, so a tile has a light source rather than a fill. */
static void app_gradient(const struct astrix_app *app, struct astrix_color *top,
                         struct astrix_color *bottom) {
	struct astrix_color base = app_color(app);
	/*
	 * The gradient is deliberately kept in the middle of the range. The
	 * original 1.35x/0.72x spread produced tiles light enough that a white
	 * glyph on them was measurably lower contrast than the label underneath,
	 * which is the one thing an icon must never be.
	 */
	*top = astrix_color_scale(base, 1.18f);
	*bottom = astrix_color_scale(base, 0.62f);
}

/*
 * Glyph colour that adapts to the tile it sits on.
 *
 * Icons are derived from a hash of the app id, so some are unavoidably light.
 * A fixed white glyph on a light tile disappears; the fix is not to hand-pick
 * nicer colours but to pick the ink from the tile's own luminance.
 */
static struct astrix_color icon_ink(const struct astrix_app *app) {
	struct astrix_color top, bottom;
	app_gradient(app, &top, &bottom);
	int lum = (top.r * 299 + top.g * 587 + top.b * 114) / 1000;
	if (lum > 150) {
		return astrix_rgba(0x14, 0x18, 0x24, 0xFF);
	}
	return astrix_rgba(0xFF, 0xFF, 0xFF, 0xFA);
}

/* Soft shadow colour for the current theme. */
static struct astrix_color shadow_color(const struct astrix_shell *sh) {
	return sh->dark_mode ? astrix_rgba(0x00, 0x00, 0x00, 0x8C)
	                     : astrix_rgba(0x24, 0x2C, 0x40, 0x3C);
}

/* Draws a small icon tile plus the app's name. Used by the switcher cards. */
static void draw_app_header(struct astrix_canvas *c, const struct astrix_shell *sh,
                            struct astrix_rect r, const struct astrix_app *app) {
	int size = r.h;
	struct astrix_rect ir = { r.x, r.y, size, size };
	struct astrix_color top, bottom;
	app_gradient(app, &top, &bottom);
	astrix_draw_card(c, ir, size / 4, top, bottom, shadow_color(sh));
	astrix_draw_glyph(c, (struct astrix_rect){ ir.x, ir.y, ir.w, ir.h }, app_glyph(app),
	                  icon_ink(app));

	char label[ASTRIX_APP_NAME_LEN];
	astrix_text_ellipsize(app->name, r.w - size - 10, label, sizeof(label));
	astrix_draw_text(c, r.x + size + 10, r.y + (size - ASTRIX_FONT_H) / 2, label,
	                 sh->theme->text);
}

/*
 * App icons: a gradient tile lifted off the wallpaper by a soft shadow, a
 * vector glyph, and a label with a shadow of its own so it stays readable
 * over any wallpaper. The tile's geometry is unchanged from the flat version
 * - only its painting changed - so every hit-test in input.c still lines up.
 */
static void draw_app_icon(struct astrix_canvas *c, const struct astrix_shell *sh,
                          struct astrix_rect r, const struct astrix_app *app) {
	const struct astrix_theme *t = sh->theme;

	int icon = r.w < r.h ? r.w : r.h;
	struct astrix_rect ir = { r.x + (r.w - icon) / 2, r.y + (icon > 56 ? 6 : 0), icon, icon };

	/* Squircle-ish tile: a generous radius reads as modern, a small one as 2012. */
	struct astrix_color top, bottom;
	app_gradient(app, &top, &bottom);
	astrix_draw_card(c, ir, icon / 3, top, bottom, shadow_color(sh));

	/* A specular highlight along the top third, which is what makes a flat
	 * gradient look like a physical object. */
	astrix_fill_rect(c, (struct astrix_rect){ ir.x + icon / 8, ir.y + 2, icon * 3 / 4, 1 },
	                 astrix_color_with_alpha(astrix_rgba(0xFF, 0xFF, 0xFF, 0xFF), 0x50));

	int glyph = icon * 3 / 5;
	struct astrix_color ink = icon_ink(app);
	astrix_draw_glyph(c, (struct astrix_rect){ ir.x + (icon - glyph) / 2,
	                                            ir.y + (icon - glyph) / 2, glyph, glyph },
	                  app_glyph(app), ink);

	/* Label under the icon, with its own drop shadow: text over a photo is
	 * the classic legibility failure and the fix is not "pick a nicer colour". */
	char label[ASTRIX_APP_NAME_LEN];
	astrix_text_ellipsize(app->name, g_cell_label_width(r.w), label, sizeof(label));
	int lw = astrix_text_width(label);
	int lx = r.x + (r.w - lw) / 2;
	int ly = ir.y + icon + 6;
	astrix_draw_text(c, lx, ly + 1, label, sh->dark_mode ? astrix_rgba(0x00, 0x00, 0x00, 0x99)
	                                                    : astrix_rgba(0xFF, 0xFF, 0xFF, 0xCC));
	astrix_draw_text(c, lx, ly, label, t->text);
}

/* --- status bar ---------------------------------------------------------- */

static void draw_status_bar(struct astrix_canvas *c, const struct astrix_shell *sh) {
	const struct astrix_theme *t = sh->theme;
	int h = t->status_h;
	struct astrix_rect r = { 0, 0, sh->width, h };

	/* The bar sits over whatever is behind it, so a subtle scrim keeps the
	 * clock legible on a light wallpaper. */
	/* A translucent bar over the wallpaper rather than a flat slab: the
	 * status bar is glass on every phone OS, and it keeps the wallpaper
	 * visible where it matters. */
	astrix_fill_glass(c, r, 0, sh->dark_mode ? astrix_rgba(0x0A, 0x0D, 0x16, 0xFF)
	                                          : astrix_rgba(0xF7, 0xF9, 0xFF, 0xFF),
	                  sh->dark_mode ? 0xC4 : 0xE6);

	int pad = 12;
	int y = (h - ASTRIX_FONT_H) / 2;

	/* Left: time. A phone leads with the clock, and it is the one label that
	 * earns weight - bold at this size, regular everywhere else. */
	astrix_draw_text_bold(c, pad, y, sh->status_time, t->text);

	/* The keyboard button, immediately right of the clock. Its geometry is
	 * shared with the hit-test in input.c via astrix_kbd_toggle_rect(), so
	 * the thing drawn and the thing tappable cannot drift apart. */
	struct astrix_rect kbtn = astrix_kbd_toggle_rect(sh);
	struct astrix_color kb_col = sh->kbd_visible ? t->on_primary : t->text_dim;
	if (sh->kbd_visible) {
		/* Active: a filled accent chip, so "the keyboard is open" is
		 * visible at a glance instead of having to read a border. */
		astrix_fill_rect_rounded(c, kbtn, kbtn.h / 2, t->primary);
	} else {
		astrix_fill_rect_rounded(c, kbtn, kbtn.h / 2,
		                         astrix_color_with_alpha(t->surface_alt, 0x90));
	}
	int kg = kbtn.h - 10;
	astrix_draw_glyph(c, (struct astrix_rect){ kbtn.x + 5, kbtn.y + 5, kg, kg },
	                  ASTRIX_GLYPH_KEYBOARD, kb_col);

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
	/*
	 * A floating pill rather than a full-width slab. This is the single
	 * cheapest upgrade to a phone UI: the bar stops being a band of flat
	 * colour across the bottom of the screen and becomes an object sitting
	 * on top of the wallpaper.
	 */
	int inset = sh->width / 14;
	if (inset < 10) {
		inset = 10;
	}
	struct astrix_rect bar = { inset, y + h / 5, sh->width - 2 * inset, h - h / 5 - 4 };
	struct astrix_color tint = sh->dark_mode ? astrix_rgba(0x14, 0x18, 0x24, 0xFF)
	                                          : astrix_rgba(0xFF, 0xFF, 0xFF, 0xFF);
	astrix_shadow_rounded(c, bar, bar.h / 2, bar.h / 3, 5, shadow_color(sh));
	astrix_fill_glass(c, bar, bar.h / 2, tint, sh->dark_mode ? 0xB4 : 0xD8);

	/* Gesture pill: the phone's home affordance. */
	int pill_w = bar.w / 3;
	int pill_h = 4;
	astrix_fill_rect_rounded(c,
	                         (struct astrix_rect){ (sh->width - pill_w) / 2,
	                                               bar.y + bar.h - 14, pill_w, pill_h },
	                         pill_h / 2,
	                         astrix_color_with_alpha(t->text, sh->dark_mode ? 0xB0 : 0x99));
}

/* --- home screen --------------------------------------------------------- */

/*
 * One soft radial bloom: a filled ellipse whose alpha falls off from the
 * centre. One pass over its own rows, with each row's span taken from the
 * ellipse equation, so the cost is the ellipse's area rather than area times
 * the number of steps - which is the difference between a wallpaper that
 * costs 40ms on a software rasteriser and one that costs 4ms.
 */
static void astrix_bloom(struct astrix_canvas *c, struct astrix_rect bounds, int cx, int cy,
                         int rx, int ry, struct astrix_color col, uint8_t max_alpha) {
	if (!c || !c->pixels || max_alpha == 0 || rx <= 0 || ry <= 0) {
		return;
	}
	for (int y = 0; y < bounds.h; y++) {
		int dy = (bounds.y + y) - cy;
		float ny = (float)dy / (float)ry;
		if (ny <= -1.0f || ny >= 1.0f) {
			continue;
		}
		float k = 1.0f - ny * ny;
		int hw = (int)((float)rx * sqrtf(k));
		int x0 = cx - hw, x1 = cx + hw;
		if (x0 < bounds.x) {
			x0 = bounds.x;
		}
		if (x1 > bounds.x + bounds.w) {
			x1 = bounds.x + bounds.w;
		}
		if (x1 <= x0) {
			continue;
		}
		/* Ease the falloff so the centre is bright and the rim is invisible;
		 * a linear ramp reads as a visible disc edge. */
		float f = 1.0f - fabsf(ny);
		f = f * f;
		struct astrix_color cc = astrix_color_with_alpha(col, (uint8_t)((float)max_alpha * f));
		astrix_fill_rect(c, (struct astrix_rect){ x0, bounds.y + y, x1 - x0, 1 }, cc);
	}
}

static void draw_wallpaper(struct astrix_canvas *c, const struct astrix_shell *sh) {
	/*
	 * A deterministic vertical gradient derived from the theme. Real OSes
	 * ship wallpaper images; until then this is a plain, honest fill rather
	 * than a fake photograph.
	 */
	struct astrix_color top = sh->dark_mode ? astrix_rgba(0x1A, 0x22, 0x4A, 0xFF)
	                                        : astrix_rgba(0xD8, 0xE2, 0xFF, 0xFF);
	struct astrix_color bottom = sh->dark_mode ? astrix_rgba(0x05, 0x07, 0x0E, 0xFF)
	                                           : astrix_rgba(0xF7, 0xF1, 0xE8, 0xFF);
	struct astrix_rect full = { 0, 0, sh->width, sh->height };
	astrix_fill_rect_gradient_v(c, full, top, bottom);

	/*
	 * Two soft accent blooms over the base ramp. A single linear gradient
	 * reads as "a gradient"; a gradient with light falling into it from two
	 * corners reads as a photograph, and that difference is most of why a
	 * modern wallpaper looks modern. Drawn as huge low-alpha ellipses
	 * rather than a bitmap, so it costs nothing to ship and nothing to load.
	 */
	struct astrix_color glow_a = sh->dark_mode ? astrix_rgba(0x5C, 0x7C, 0xFF, 0xFF)
	                                           : astrix_rgba(0xFF, 0xA8, 0x54, 0xFF);
	struct astrix_color glow_b = sh->dark_mode ? astrix_rgba(0x9B, 0x4D, 0xD8, 0xFF)
	                                           : astrix_rgba(0x4A, 0xB4, 0xFF, 0xFF);

	/*
	 * Two accent blooms over the base ramp. A single linear gradient reads as
	 * "a gradient"; light falling into it from two corners reads as depth.
	 *
	 * Cost matters here: this is a software rasteriser on a phone GPU we do
	 * not yet have. Each bloom is one pass over its own rows, with the span
	 * for a row taken straight from the ellipse equation - so the price is
	 * the area of the ellipse (a few hundred thousand blended pixels at
	 * 1024x768), not area times steps.
	 */
	struct astrix_rect bloom = { 0, 0, sh->width, sh->height };
	astrix_bloom(c, bloom, sh->width / 3, sh->height / 5, sh->width / 3, sh->height / 4,
	             glow_a, sh->dark_mode ? 0x66 : 0x70);
	astrix_bloom(c, bloom, (sh->width * 5) / 6, (sh->height * 4) / 5, sh->width / 3,
	             sh->height / 4, glow_b, sh->dark_mode ? 0x54 : 0x62);
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
