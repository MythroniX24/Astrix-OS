/*
 * Astrix OS - Astrix Shell.
 *
 * The shell is the mobile user interface. It is a single Wayland client that
 * draws the entire system UI (status bar, home screen, launcher, notification
 * centre, quick settings, lock screen, app switcher, power menu) into one
 * shm buffer using libastrix-ui, and it interprets touch gestures to move
 * between them.
 *
 * Why one process draws all of it
 * -------------------------------
 * A phone shell that composes each surface separately pays for IPC, extra
 * buffers and extra compositing passes on every frame. Astrix draws the whole
 * UI in one pass and only blends in a native app's surface when an app is in
 * the foreground. That keeps the frame cost proportional to what is on screen
 * and avoids a "compositor talks to shell" protocol entirely.
 *
 * The tradeoff is that the shell cannot be replaced by an independent process
 * without reimplementing this draw pass. docs/ARCHITECTURE.md describes the
 * seams that keep that possible.
 */

#ifndef ASTRIX_SHELL_H
#define ASTRIX_SHELL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "astrix_ui.h"

/* --- identity ------------------------------------------------------------ */

#define ASTRIX_SHELL_APP_ID "org.astrix.Shell"
#define ASTRIX_MAX_APPS 64
#define ASTRIX_MAX_NOTIFICATIONS 32
#define ASTRIX_APP_ID_LEN 64
#define ASTRIX_APP_NAME_LEN 48
#define ASTRIX_APP_EXEC_LEN 192

enum astrix_app_kind {
	ASTRIX_APP_NATIVE,   /* a Linux Wayland client                     */
	ASTRIX_APP_ANDROID,  /* an Android app hosted by the compatibility layer */
	ASTRIX_APP_SYSTEM,   /* built into the shell (terminal, files, ...) */
};

struct astrix_app {
	char id[ASTRIX_APP_ID_LEN];
	char name[ASTRIX_APP_NAME_LEN];
	char exec[ASTRIX_APP_EXEC_LEN];
	char icon_path[256];
	enum astrix_app_kind kind;
	/* For Android apps, the activity to start. Native apps use exec. */
	char android_activity[256];
	/* Page the icon lives on in the home screen grid. */
	int page;
	/* 0..3: 0 = dock, 1..3 = home pages. */
	int slot;
	bool pinned;
	bool running;
	bool hidden;
	/*
	 * The pid of the process behind this app, or 0 when none is running.
	 *
	 * The shell model never forks and never calls kill(), so this is a
	 * plain record: whoever owns the display connection writes the pid
	 * here after a successful launch, and the shell clears it when the
	 * process is gone. Keeping it in the model (rather than in the
	 * client's private state) is what lets a host test assert the
	 * bookkeeping without a compositor.
	 */
	int pid;
};

/* --- notifications ------------------------------------------------------- */

enum astrix_notification_priority {
	ASTRIX_NOTIF_LOW = 0,
	ASTRIX_NOTIF_NORMAL,
	ASTRIX_NOTIF_HIGH,
	ASTRIX_NOTIF_CRITICAL,
};

struct astrix_notification {
	char id[64];
	char app_id[ASTRIX_APP_ID_LEN];
	char app_name[ASTRIX_APP_NAME_LEN];
	char title[128];
	char body[256];
	enum astrix_notification_priority priority;
	int64_t timestamp_ms;
	bool dismissed;
};

/* --- gesture recogniser -------------------------------------------------- */

enum astrix_gesture {
	ASTRIX_GESTURE_NONE = 0,
	ASTRIX_GESTURE_TAP,
	ASTRIX_GESTURE_DOUBLE_TAP,
	ASTRIX_GESTURE_LONG_PRESS,
	ASTRIX_GESTURE_SWIPE_UP,
	ASTRIX_GESTURE_SWIPE_DOWN,
	ASTRIX_GESTURE_SWIPE_LEFT,
	ASTRIX_GESTURE_SWIPE_RIGHT,
	ASTRIX_GESTURE_SWIPE_UP_HOLD,   /* app switcher */
	ASTRIX_GESTURE_EDGE_SWIPE,      /* back */
	ASTRIX_GESTURE_PINCH,
	ASTRIX_GESTURE_TWO_FINGER_SWIPE,
};

/* Tunables, exposed so settings can adjust them (gesture sensitivity). */
struct astrix_gesture_config {
	int swipe_threshold_px;      /* distance that counts as a swipe      */
	int long_press_ms;           /* hold duration for long press        */
	int double_tap_ms;           /* max gap between taps                */
	int edge_width_px;           /* width of the back-gesture edge zone */
	int touch_slop_px;           /* movement that cancels a tap          */
	int long_press_slop_px;      /* movement allowed during a long press */
	int vertical_dominance_pct;  /* ady must exceed adx by this percent    */
	float pinch_scale_trigger;
};

void astrix_gesture_config_defaults(struct astrix_gesture_config *cfg);

/*
 * Stateful gesture recogniser. Feed it pointer/touch events; it returns the
 * recognised gesture. Keeping it pure (no Wayland, no drawing) means it can be
 * unit tested on the host, which is where gesture bugs are found.
 */
struct astrix_gesture_state {
	struct astrix_gesture_config cfg;

	bool down;
	int start_x, start_y;
	int last_x, last_y;
	int64_t start_ms;
	int64_t last_ms;

	/* Multi-touch */
	int active_points;
	int pinch_start_distance;
	int pinch_last_distance;

	/* Double-tap tracking */
	int64_t last_tap_ms;
	int last_tap_x, last_tap_y;

	/* True once movement exceeded slop: the gesture is a drag/swipe. */
	bool moving;
	/* Set once an upward swipe has been recognised, so a subsequent hold
	 * becomes "swipe up and hold" (the app switcher) rather than a plain
	 * long press. */
	bool swiped_up;
	/*
	 * When the finger last actually moved. "Hold" means the finger has
	 * stopped, so both the long press and the swipe-up-and-hold are timed
	 * from this rather than from the start of the touch: timing from the
	 * start means any swipe slow enough to outlive long_press_ms
	 * satisfies the hold, which on a loaded machine is every swipe.
	 */
	int64_t moved_ms;
	/* Where the finger was when it last moved, to measure "has it stopped". */
	int hold_anchor_x, hold_anchor_y;
	/* Set once a long press has been reported and then revoked by travel;
	 * stops it firing again later in the same contact. */
	bool long_press_cancelled;
	/* Set while a long press has already fired so it fires only once. */
	bool long_press_fired;
	/*
	 * Latches a gesture recognised mid-touch (currently swipe-up-hold).
	 * Without this, the release would re-classify from the end position and
	 * downgrade a completed hold back to a plain swipe.
	 */
	enum astrix_gesture latched;
};

void astrix_gesture_init(struct astrix_gesture_state *st,
                         const struct astrix_gesture_config *cfg);

/* Feed an event. Returns the gesture recognised by this event, if any. */
enum astrix_gesture astrix_gesture_handle(struct astrix_gesture_state *st,
                                          const struct astrix_input_event *ev);

/* Human-readable name, for logs and the settings UI. */
const char *astrix_gesture_name(enum astrix_gesture g);

/* --- shell screen state -------------------------------------------------- */

/*
 * The on-screen keyboard lives in the shell, not in one app. It has to: a
 * touch OS that cannot type into Settings or a PIN prompt is not a phone OS,
 * and the terminal already carried a private keyboard, which meant two ways to
 * type and no way to type into anything else.
 *
 * Declared here rather than in keyboard.h because the shell struct below
 * queues the keys, and the queue has to hold the action kinds.
 */
enum astrix_kbd_action {
	ASTRIX_KBD_CHAR = 0,   /* emit a codepoint              */
	ASTRIX_KBD_SHIFT,      /* one-shot shift                */
	ASTRIX_KBD_SYMBOLS,    /* toggle the symbol layer       */
	ASTRIX_KBD_BACKSPACE,  /* '\b'                          */
	ASTRIX_KBD_ENTER,      /* '\n'                          */
	ASTRIX_KBD_SPACE,      /* ' '                           */
	ASTRIX_KBD_HIDE,       /* close the keyboard            */
};

#define ASTRIX_KBD_QUEUE_LEN 64

/* One queued key press: either a special action or a codepoint to type. */
struct astrix_kbd_press {
	enum astrix_kbd_action action;
	uint32_t codepoint;
};

enum astrix_screen {
	ASTRIX_SCREEN_LOCK,
	ASTRIX_SCREEN_HOME,
	ASTRIX_SCREEN_APP,
	ASTRIX_SCREEN_NOTIFICATION,  /* shade, drag-revealed */
	ASTRIX_SCREEN_QUICK_SETTINGS,
	ASTRIX_SCREEN_APP_SWITCHER,
	ASTRIX_SCREEN_POWER_MENU,
	ASTRIX_SCREEN_LAUNCHER,      /* all-apps list */
};

struct astrix_shell {
	/* Presentation */
	int width, height;
	struct astrix_canvas canvas;
	/* The framebuffer the canvas draws into. ARGB8888, uploaded to the
	 * compositor through wl_shm. */
	uint32_t *pixels;
	size_t buffer_size;
	int stride;
	/*
	 * Who owns `pixels`.
	 *
	 * Normally the shell allocates its own framebuffer (owned = true) and
	 * frees it on resize and destroy. A Wayland host instead aliases
	 * `pixels` onto a wl_shm mapping it owns, so the shell must neither
	 * free nor reallocate it - doing so is free() on an mmap'd pointer,
	 * which is a SIGSEGV. Every allocation path sets this, and every
	 * free path checks it.
	 */
	bool pixels_owned;

	bool dark_mode;
	const struct astrix_theme *theme;

	/* Navigation */
	enum astrix_screen screen;
	enum astrix_screen screen_from;   /* what to return to on dismiss */
	int home_page;
	int launcher_scroll;
	bool locked;
	bool status_shade_open;
	float shade_progress;              /* 0..1 drag state */
	int foreground_app;                /* index into apps, -1 = none */
	/*
	 * Index of an app the user asked to open, waiting for whoever owns the
	 * display to actually start it, or -1.
	 *
	 * The shell decides *what* to launch; the Wayland client that holds the
	 * connection to the compositor is what can start a process. Setting a
	 * flag is the seam between them, and it keeps the decision testable
	 * without a compositor: the shell never forks, and a host test can
	 * assert that opening an app requests exactly one launch.
	 */
	int launch_pending;                /* index into apps, -1 = none */
	/*
	 * Index of an app the user asked to terminate, waiting for whoever
	 * owns the display to send the signal, or -1. Same seam as
	 * launch_pending, in the other direction: closing an app has to
	 * actually stop its process, not just clear a boolean.
	 */
	int kill_pending;                  /* index into apps, -1 = none */
	/*
	 * The pid to signal, captured when the close was requested.
	 *
	 * This cannot be looked up from apps[kill_pending] at send time: the
	 * close clears that entry's pid immediately, because the app is no
	 * longer "running" from the UI's point of view. So the pid was read
	 * after being wiped, the signal was never sent, and the app carried
	 * on running with a surface mapped - while the launcher showed it
	 * closed. The model has to carry the value the signal needs.
	 */
	int kill_pending_pid;

	/* Apps */
	struct astrix_app apps[ASTRIX_MAX_APPS];
	int app_count;
	/* Apps that have a live Wayland surface, for the app switcher. */
	int running_apps[ASTRIX_MAX_APPS];
	int running_count;

	/* Notifications */
	struct astrix_notification notifications[ASTRIX_MAX_NOTIFICATIONS];
	int notification_count;
	int notification_scroll;

	/* Input */
	struct astrix_gesture_state gestures;
	struct astrix_input_event last_event;
	bool has_focus;
	int pointer_x, pointer_y;   /* where the finger is, for press feedback */

	/*
	 * On-screen keyboard.
	 *
	 * A tap becomes a queued press; whoever owns the seat (main.c) drains
	 * the queue and delivers it as real key events through a virtual
	 * keyboard. Keeping the queue in the model is what lets the host tests
	 * assert "tapping the key Q produces Q" without a compositor, a
	 * keymap, or a client.
	 */
	bool kbd_visible;
	bool kbd_shift;         /* one-shot: cleared after the next character */
	bool kbd_caps;          /* sticky: until tapped again                  */
	bool kbd_symbols;       /* the symbol layer                            */
	int kbd_press_x, kbd_press_y;   /* -1 when no key is held */
	struct astrix_kbd_press kbd_queue[ASTRIX_KBD_QUEUE_LEN];
	int kbd_queue_len;      /* pending presses, never exceeds the array    */

	/* Status bar content, refreshed from the system daemon. */
	char status_time[16];
	char status_battery[16];
	int battery_percent;
	bool charging;
	bool wifi_on;
	bool wifi_available;
	bool bt_on;
	bool bt_available;
	bool airplane_mode;
	bool do_not_disturb;
	bool dnd_available;
	bool location_on;
	bool location_available;
	bool rotation_locked;
	int brightness;            /* 0..100 */
	bool sound_on;
	bool battery_saver;
	bool mobile_available;
	bool mobile_on;

	/* Clock / uptime for the power menu */
	int64_t uptime_s;
	double load_avg;
	int64_t memory_used_kb;
	int64_t memory_total_kb;

	/* Frame pacing */
	uint64_t frame_count;
	bool needs_redraw;
};

/* --- lifecycle ----------------------------------------------------------- */

struct astrix_shell *astrix_shell_create(int width, int height);
void astrix_shell_destroy(struct astrix_shell *shell);
void astrix_shell_set_size(struct astrix_shell *shell, int width, int height);

/* Register an application so it appears in the launcher. */
/* Number of icons in the dock row. Shared by render.c (which draws it),
 * input.c (which hit-tests it) and apps.c (which decides what to pin). */
#define ASTRIX_DOCK_COLS 4

int astrix_shell_add_app(struct astrix_shell *shell, const struct astrix_app *app);

/*
 * Populate the launcher from the freedesktop.org desktop entries in the XDG
 * data directories. Returns how many apps were added.
 *
 * Without this the launcher's app list is empty on a real device: the home
 * screen renders fine, every icon grid test passes, and there is nothing to
 * tap. Returns the count so a caller can report a zero-app system as a
 * problem rather than as a session that is simply still starting.
 */
int astrix_shell_scan_apps(struct astrix_shell *shell);
void astrix_shell_sort_apps(struct astrix_shell *shell);

/* Notifications */
int astrix_shell_notify(struct astrix_shell *shell, const char *app_id, const char *app_name,
                        const char *title, const char *body, enum astrix_notification_priority prio);
void astrix_shell_dismiss_notification(struct astrix_shell *shell, int index);
void astrix_shell_clear_notifications(struct astrix_shell *shell);
int astrix_shell_unread_count(const struct astrix_shell *shell);

/* Draw one full frame. */
void astrix_shell_draw(struct astrix_shell *shell);

/* Handle an input event. Returns true if the shell consumed it. */
bool astrix_shell_handle_input(struct astrix_shell *shell,
                                const struct astrix_input_event *ev);

/* Navigation helpers, also used by the keyboard/daemon paths. */
void astrix_shell_go_home(struct astrix_shell *shell);
void astrix_shell_open_app(struct astrix_shell *shell, int index);
void astrix_shell_close_app(struct astrix_shell *shell, int index);

/*
 * Record a successful launch. Called by the display owner with the pid it
 * forked; until this runs the app is "requested" but not running.
 */
void astrix_shell_set_app_pid(struct astrix_shell *shell, int index, int pid);

/*
 * Note that a process the shell launched has exited.
 *
 * Returns true if the pid belonged to a tracked app, i.e. if this call
 * changed anything. A pid the shell does not know about (or one already
 * reaped) is ignored rather than treated as an error, so a double reap or
 * an unrelated child cannot corrupt the app list. A foreground app that
 * exits returns the user to the home screen, because leaving a dead app's
 * screen up would be a lie about what is running.
 */
bool astrix_shell_note_app_exit(struct astrix_shell *shell, int pid);
void astrix_shell_toggle_lock(struct astrix_shell *shell);

#endif /* ASTRIX_SHELL_H */
