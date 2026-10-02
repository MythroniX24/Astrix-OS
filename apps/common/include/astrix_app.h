/*
 * Astrix OS - native application host.
 *
 * Every Astrix app is a plain Wayland client. This header is the small amount
 * of plumbing they all need: connect to astrix-compositor, own a fullscreen
 * xdg-shell surface, keep two wl_shm buffers alive, receive touch/pointer/key
 * input from the seat, and run a frame loop.
 *
 * Why apps do not link against a library or a toolkit runtime
 * ------------------------------------------------------------
 * A phone shell has to launch an app in a few tens of milliseconds. A runtime
 * that has to initialise itself first (Java, Electron, a large toolkit) blows
 * that budget. Every Astrix app is therefore a single static-ish C binary that
 * is ready to draw in its first frame. The cost is that this plumbing is
 * duplicated; the mitigation is that it lives here, once, instead of in each
 * app.
 *
 * Apps talk to the host only through the functions below, so an app has no
 * knowledge of Wayland at all. That is what makes the apps testable with the
 * same render-to-PPM path the shell uses.
 */

#ifndef ASTRIX_APP_H
#define ASTRIX_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "astrix_ui.h"

/* Forward declaration: the host is opaque to apps. */
struct astrix_app_host;

/* --- application description --------------------------------------------- */

/*
 * An app supplies callbacks and a descriptor. The host calls on_start once
 * the surface is configured, then on_frame whenever a redraw is needed, and
 * on_input for every input event.
 *
 * Returning true from on_input means "I consumed this", which stops the host
 * from doing anything else with it.
 */
struct astrix_app_desc {
	const char *app_id;   /* reverse-DNS, used as the Wayland app_id   */
	const char *title;    /* window title, used in the app switcher   */
	const char *version;

	/* Ask the compositor for keyboard focus as soon as the surface is
	 * mapped. Apps that display a text field or a keyboard (terminal,
	 * settings search) set this; pure display apps leave it false. */
	bool wants_keyboard;

	void (*on_start)(struct astrix_app_host *host);
	void (*on_frame)(struct astrix_app_host *host);
	bool (*on_input)(struct astrix_app_host *host, const struct astrix_input_event *ev);
	void (*on_exit)(struct astrix_app_host *host);

	/* Optional: called ~16 times a second so an app can poll a clock or a
	 * file without spawning a timer thread. */
	void (*on_tick)(struct astrix_app_host *host, double dt);
};

/* --- host services -------------------------------------------------------- */

struct astrix_canvas *astrix_app_canvas(struct astrix_app_host *host);
const struct astrix_theme *astrix_app_theme(struct astrix_app_host *host);
void astrix_app_size(struct astrix_app_host *host, int *w, int *h);

/* Mark the app as needing a redraw. Cheap; coalesced to the next frame. */
void astrix_app_invalidate(struct astrix_app_host *host);

/* Set the theme. Returns true if it changed, so callers can redraw. */
bool astrix_app_set_dark(struct astrix_app_host *host, bool dark);
bool astrix_app_dark(struct astrix_app_host *host);

/* Ask the compositor to close this app's surface. */
void astrix_app_request_close(struct astrix_app_host *host);
/* Terminate the app's event loop after the current frame. */
void astrix_app_quit(struct astrix_app_host *host);

/* Show a short toast at the bottom of the screen. */
void astrix_app_toast(struct astrix_app_host *host, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* Host-owned state an app may use freely. Zeroed on creation. */
void *astrix_app_user(struct astrix_app_host *host);
void astrix_app_set_user(struct astrix_app_host *host, void *user);

/* Monotonic milliseconds, for timing. */
int64_t astrix_app_now_ms(struct astrix_app_host *host);

/* --- standard widgets ----------------------------------------------------- */

/*
 * These are the only UI pieces the apps share. They are deliberately plain
 * functions over the canvas rather than a retained widget tree: the shell
 * redraws its whole UI every frame and apps do the same, so a retained tree
 * would cost more than it saves on a phone-class CPU.
 */

/*
 * Draw a top app bar. Returns the content rect below it. `subtitle` may be
 * NULL. A back chevron is drawn on the left when `show_back` is set.
 */
struct astrix_rect astrix_widget_appbar(struct astrix_canvas *c,
                                        const struct astrix_theme *th, const char *title,
                                        const char *subtitle, bool show_back);

/* Rect of the back chevron hit area, for hit-testing. */
struct astrix_rect astrix_widget_back_hit(int width);

/* A tappable filled button. `variant`: 0 neutral, 1 primary, 2 danger. */
struct astrix_rect astrix_widget_button(struct astrix_canvas *c,
                                        const struct astrix_theme *th, struct astrix_rect r,
                                        const char *label, int variant, bool pressed);

/*
 * A list row: title, optional subtitle, optional trailing text, and a leading
 * square swatch whose colour is derived from the title so each app has a
 * stable colour without shipping an icon.
 */
void astrix_widget_row(struct astrix_canvas *c, const struct astrix_theme *th,
                       struct astrix_rect r, const char *title, const char *subtitle,
                       const char *trailing, bool selected, bool pressed);

/* A section header. */
void astrix_widget_section(struct astrix_canvas *c, const struct astrix_theme *th,
                           struct astrix_rect r, const char *text);

/* A switch. Returns the rect, which is also the hit target. */
struct astrix_rect astrix_widget_switch(struct astrix_canvas *c,
                                        const struct astrix_theme *th, struct astrix_rect r,
                                        bool on, bool pressed);

/* A slider. Returns the thumb rect. */
struct astrix_rect astrix_widget_slider(struct astrix_canvas *c,
                                        const struct astrix_theme *th, struct astrix_rect r,
                                        float value, bool pressed);

/* A card (rounded surface) that a widget can be drawn into. */
struct astrix_rect astrix_widget_card(struct astrix_canvas *c,
                                      const struct astrix_theme *th, struct astrix_rect r);

/*
 * The content rect a card would have, without drawing it. Hit-testing needs
 * the same geometry the drawing code uses, and a hit-test must not write to
 * the framebuffer, so the inset is exposed separately.
 */
struct astrix_rect astrix_widget_card_inner(const struct astrix_theme *th, struct astrix_rect r);

/* An empty-state message, centred in `r`. */
void astrix_widget_empty(struct astrix_canvas *c, const struct astrix_theme *th,
                         struct astrix_rect r, const char *title, const char *body);

/* A horizontal progress bar. */
void astrix_widget_progress(struct astrix_canvas *c, const struct astrix_theme *th,
                            struct astrix_rect r, float fraction);

/* --- scrolling ------------------------------------------------------------ */

/*
 * A minimal fling/inertia scroller. Apps call astrix_scroll_handle from their
 * on_input; it consumes touch motion and up events while the user is
 * scrolling. Everything outside that window is left for the app, so a row tap
 * still works while a long list is on screen.
 */
struct astrix_scroll {
	int offset;          /* pixels scrolled from the top      */
	int content_h;       /* total content height in pixels    */
	int view_h;          /* viewport height in pixels         */
	int view_w;          /* viewport width, for touch slop     */
	bool dragging;
	bool tracking;       /* a finger is down, may become a drag */
	int touch_x, touch_y;
	int start_offset;
	/* Fling velocity tracking: the last motion sample and the time it
	 * arrived, so a flick's speed is measured rather than assumed. */
	int last_y;
	int64_t last_ms;
	int64_t down_ms;
	int velocity;        /* px per frame, for inertia          */
	bool inertial;
};

void astrix_scroll_reset(struct astrix_scroll *s, int content_h, int view_h, int view_w);
void astrix_scroll_clamp(struct astrix_scroll *s);
/*
 * Feed an event. Returns true if the scroller consumed it. The event's
 * timestamp (seconds) is used for fling velocity.
 */
bool astrix_scroll_handle(struct astrix_scroll *s, const struct astrix_input_event *ev);
/* Call once per frame while inertia is running. Returns true if still moving. */
bool astrix_scroll_step(struct astrix_scroll *s);

/* --- entry point ---------------------------------------------------------- */

/*
 * The conventional main(): build a descriptor, hand it here, let the host own
 * the Wayland connection and the loop. Returns the process exit code.
 */
int astrix_app_main(const struct astrix_app_desc *desc, int argc, char **argv);

/*
 * As above, but seeds astrix_app_user() with `user` before on_start runs.
 *
 * Apps need this because their state has to exist before the host does: a
 * terminal has to open its PTY, a file manager has to read a directory, and
 * neither can wait for a callback that the host would only deliver after it
 * had already allocated the user pointer.
 */
int astrix_app_main_with_user(const struct astrix_app_desc *desc, int argc, char **argv,
                              void *user);

#endif /* ASTRIX_APP_H */
