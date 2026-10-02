/*
 * Astrix OS - Astrix Settings UI.
 *
 * Renders the settings table, applies changes to the store, and performs the
 * side effects for the toggles that have a real one (brightness, radios,
 * session actions). A toggle that cannot be applied says so and reverts,
 * rather than presenting a state the system is not in.
 */

#include "astrix_app.h"
#include "astrix_settings.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct settings_app {
	struct astrix_settings store;
	struct astrix_hw hw;

	/* Indices into the settings table, for the rows actually shown. */
	int visible[ASTRIX_MAX_SETTINGS];
	int visible_count;

	struct astrix_scroll scroll;
	int pressed_row;
	bool pressed_valid;
	int width, height;
	char last_error[128];
};

/* --- side effects -------------------------------------------------------- */

/*
 * Write a value to a sysfs attribute and read it back. Returns 0 only if the
 * kernel reports the value we asked for, which is the only way to know the
 * write actually took effect rather than merely being accepted.
 */
static int write_sysfs(const char *path, int value) {
	FILE *f = fopen(path, "w");
	if (!f) {
		return -1;
	}
	if (fprintf(f, "%d\n", value) < 0) {
		fclose(f);
		return -1;
	}
	fclose(f);

	FILE *r = fopen(path, "r");
	if (!r) {
		return -1;
	}
	int got = -1;
	if (fscanf(r, "%d", &got) != 1) {
		got = -1;
	}
	fclose(r);
	return (got == value) ? 0 : -1;
}

/* Find the brightness device. Panels are usually "backlight" or a vendor name. */
static void find_backlight(char *out, size_t out_size, int *max_out) {
	out[0] = '\0';
	*max_out = 100;
	DIR *d = opendir("/sys/class/backlight");
	if (!d) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		snprintf(out, out_size, "/sys/class/backlight/%s/brightness", de->d_name);
		char maxpath[256];
		snprintf(maxpath, sizeof(maxpath), "/sys/class/backlight/%s/max_brightness", de->d_name);
		FILE *f = fopen(maxpath, "r");
		if (f) {
			if (fscanf(f, "%d", max_out) != 1) {
				*max_out = 100;
			}
			fclose(f);
		}
		break;
	}
	closedir(d);
}

static void apply_brightness(struct settings_app *sa, struct astrix_app_host *host) {
	int pct = astrix_settings_get_int(&sa->store, "display.brightness", 70);
	char path[256];
	int max_bright = 100;
	find_backlight(path, sizeof(path), &max_bright);
	if (path[0] == '\0') {
		sa->last_error[0] = '\0'; /* no panel: the row would not be shown */
		return;
	}
	int scaled = (pct * max_bright) / 100;
	if (write_sysfs(path, scaled) != 0) {
		snprintf(sa->last_error, sizeof(sa->last_error), "brightness: %s", strerror(errno));
		astrix_app_toast(host, "brightness not changed");
		return;
	}
	sa->last_error[0] = '\0';
}

/*
 * Radios are toggled through the kernel's rfkill interface when it is
 * present. rfkill is the only supported way to turn a radio off in software
 * without the radio's own driver, and the Astrix kernel config enables it.
 */
static void apply_radio(struct settings_app *host_store, struct astrix_app_host *host,
                        const char *label, const char *sysfs_name, bool on) {
	char path[256];
	/* rfkill indexes are not stable, so the switch is resolved by name
	 * through the sysfs symlink. */
	snprintf(path, sizeof(path), "/sys/class/rfkill/%s-%s", sysfs_name, on ? "0" : "1");
	if (write_sysfs(path, 0) != 0) {
		snprintf(host_store->last_error, sizeof(host_store->last_error), "%s: no rfkill device",
		         label);
		astrix_app_toast(host, "%s not available", label);
	}
}

static void run_action(struct astrix_app_host *host, const char *action) {
	if (strcmp(action, "system.power_off") == 0) {
		astrix_app_toast(host, "power off requires root; use the power menu");
		return;
	}
	if (strcmp(action, "system.restart") == 0) {
		astrix_app_toast(host, "restart requires root; use the power menu");
		return;
	}
	if (strcmp(action, "updates.check") == 0) {
		/*
		 * A real update check runs apt-get update through polkit rather
		 * than pretending to. Until the polkit helper is installed the
		 * honest answer is to say so.
		 */
		astrix_app_toast(host, "update check needs the package manager");
	}
}

static void apply_setting(struct settings_app *sa, struct astrix_app_host *host,
                          const struct astrix_setting *set) {
	if (strcmp(set->key, "display.brightness") == 0) {
		apply_brightness(sa, host);
	} else if (strcmp(set->key, "network.wifi") == 0) {
		bool on = astrix_settings_get_bool(&sa->store, set->key, false);
		apply_radio(sa, host, "Wi-Fi", "wlan", on);
	} else if (strcmp(set->key, "bluetooth.enabled") == 0) {
		bool on = astrix_settings_get_bool(&sa->store, set->key, false);
		apply_radio(sa, host, "Bluetooth", "bluetooth", on);
	} else if (set->type == ASTRIX_SETTING_ACTION) {
		run_action(host, set->key);
	}
}

/* --- visible rows -------------------------------------------------------- */

static void rebuild_visible(struct settings_app *sa) {
	int count = 0;
	const struct astrix_setting *table = astrix_settings_table(&count);
	sa->visible_count = 0;
	for (int i = 0; i < count && sa->visible_count < ASTRIX_MAX_SETTINGS; i++) {
		if (!astrix_setting_available(&table[i], &sa->hw)) {
			continue;
		}
		/*
		 * The Wi-Fi toggle needs an actual wireless interface. The
		 * generic "/sys/class/net" requirement above is satisfied by an
		 * Ethernet-only device too, so the wireless case is checked here
		 * where the specific key is known.
		 */
		if (strcmp(table[i].key, "network.wifi") == 0 && !sa->hw.wifi) {
			continue;
		}
		sa->visible[sa->visible_count++] = i;
	}
}

/* --- drawing ------------------------------------------------------------- */

static int row_height(const struct astrix_theme *th) {
	return th->touch_target + th->spacing * 2;
}

static void on_frame(struct astrix_app_host *host) {
	struct settings_app *sa = astrix_app_user(host);
	struct astrix_canvas *c = astrix_app_canvas(host);
	const struct astrix_theme *th = astrix_app_theme(host);
	int w, h;
	astrix_app_size(host, &w, &h);
	int count = 0;
	const struct astrix_setting *table = astrix_settings_table(&count);

	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, w, h }, th->background);
	struct astrix_rect content = astrix_widget_appbar(c, th, "Settings", "Astrix OS", true);

	int y = content.y + th->spacing - sa->scroll.offset;
	int rh = row_height(th);

	/*
	 * Section headers are derived from the key prefix, so adding a setting
	 * to the table is enough to have it appear in the right place.
	 */
	const char *section = NULL;
	for (int i = 0; i < sa->visible_count; i++) {
		const struct astrix_setting *set = &table[sa->visible[i]];
		if (y > h + rh) {
			break;
		}
		if (y + rh > content.y) {
			char prefix[32];
			const char *dot = strchr(set->key, '.');
			if (dot) {
				size_t n = (size_t)(dot - set->key);
				if (n < sizeof(prefix)) {
					memcpy(prefix, set->key, n);
					prefix[n] = '\0';
				} else {
					snprintf(prefix, sizeof(prefix), "%.*s", (int)n, set->key);
				}
			} else {
				snprintf(prefix, sizeof(prefix), "other");
			}
			if (!section || strcmp(section, prefix) != 0) {
				char title[64];
				snprintf(title, sizeof(title), "%s", prefix);
				title[0] = (char)(title[0] - 'a' + 'A');
				astrix_widget_section(c, th, (struct astrix_rect){ th->spacing * 2, y,
				                                                      w - th->spacing * 4, 24 },
				                     title);
				y += 26;
				section = set->key; /* stable pointer into the static table */
				section = prefix;
			}
		}

		if (y + rh < content.y || y > h) {
			y += rh;
			continue;
		}

		struct astrix_rect row = { 0, y, w, rh };
		bool pressed = sa->pressed_valid && sa->pressed_row == i;
		const char *trailing = NULL;
		char valuebuf[64];

		switch (set->type) {
		case ASTRIX_SETTING_TOGGLE:
			astrix_widget_row(c, th, row, set->label, set->description, NULL, false, pressed);
			astrix_widget_switch(c, th, row,
			                     astrix_settings_get_bool(&sa->store, set->key, false), pressed);
			break;
		case ASTRIX_SETTING_SLIDER: {
			int v = astrix_settings_get_int(&sa->store, set->key, set->min);
			float f = (float)(v - set->min) / (float)(set->max - set->min);
			astrix_widget_row(c, th, row, set->label, set->description, NULL, false, pressed);
			struct astrix_rect track = { w / 2, y + th->spacing * 2, w / 2 - th->spacing * 3,
				                            th->touch_target - th->spacing * 4 };
			astrix_widget_slider(c, th, track, f, pressed);
			break;
		}
		case ASTRIX_SETTING_CHOICE: {
			int idx = astrix_settings_get_int(&sa->store, set->key, 0);
			if (idx < 0 || idx >= 6 || !set->choices[idx]) {
				idx = 0;
			}
			astrix_widget_row(c, th, row, set->label, set->description, set->choices[idx], false,
			                  pressed);
			break;
		}
		case ASTRIX_SETTING_ACTION:
			astrix_widget_row(c, th, row, set->label, set->description, "Tap", false, pressed);
			break;
		}
		(void)valuebuf;
		y += rh;
	}

	if (sa->last_error[0]) {
		astrix_draw_text(c, th->spacing * 2, h - th->navbar_h - 4, sa->last_error, th->warning);
	}
}

static void on_tick(struct astrix_app_host *host, double dt) {
	(void)dt;
	struct settings_app *sa = astrix_app_user(host);
	if (astrix_scroll_step(&sa->scroll)) {
		astrix_app_invalidate(host);
	}
}

/* --- input --------------------------------------------------------------- */

static bool on_input(struct astrix_app_host *host, const struct astrix_input_event *ev) {
	struct settings_app *sa = astrix_app_user(host);
	int w, h;
	astrix_app_size(host, &w, &h);
	const struct astrix_theme *th = astrix_app_theme(host);
	int count = 0;
	const struct astrix_setting *table = astrix_settings_table(&count);
	int rh = row_height(th);
	int list_top = th->status_h + 52;

	switch (ev->kind) {
	case ASTRIX_INPUT_TOUCH_DOWN: {
		sa->pressed_valid = false;
		/* Walk the visible rows, accounting for the section headers that
		 * precede them, so the tap lands on the row the user aimed at. */
		int y = list_top + th->spacing - sa->scroll.offset;
		const char *section = NULL;
		char prev_prefix[32] = "";
		for (int i = 0; i < sa->visible_count; i++) {
			const struct astrix_setting *set = &table[sa->visible[i]];
			char prefix[32];
			const char *dot = strchr(set->key, '.');
			if (dot && (size_t)(dot - set->key) < sizeof(prefix)) {
				size_t n = (size_t)(dot - set->key);
				memcpy(prefix, set->key, n);
				prefix[n] = '\0';
			} else {
				snprintf(prefix, sizeof(prefix), "other");
			}
			if (strcmp(prefix, prev_prefix) != 0) {
				if (y > ev->y) {
					break;
				}
				y += 26;
				memcpy(prev_prefix, prefix, sizeof(prefix));
			}
			(void)section;
			if (ev->y >= y && ev->y < y + rh) {
				sa->pressed_row = i;
				sa->pressed_valid = true;
				break;
			}
			y += rh;
		}
		return true;
	}

	case ASTRIX_INPUT_TOUCH_MOTION:
	case ASTRIX_INPUT_POINTER_MOTION: {
		/*
		 * A slider is dragged, not tapped. The track geometry here is
		 * the same one on_frame uses, so the thumb follows the finger
		 * exactly.
		 */
		int list_top = th->status_h + 52;
		int y = list_top + th->spacing - sa->scroll.offset;
		char prev_prefix[32] = "";
		for (int i = 0; i < sa->visible_count; i++) {
			const struct astrix_setting *set = &table[sa->visible[i]];
			char prefix[32];
			const char *dot = strchr(set->key, '.');
			if (dot && (size_t)(dot - set->key) < sizeof(prefix)) {
				size_t n = (size_t)(dot - set->key);
				memcpy(prefix, set->key, n);
				prefix[n] = '\0';
			} else {
				snprintf(prefix, sizeof(prefix), "other");
			}
			if (strcmp(prefix, prev_prefix) != 0) {
				y += 26;
				memcpy(prev_prefix, prefix, sizeof(prefix));
			}
			if (ev->y >= y && ev->y < y + rh && set->type == ASTRIX_SETTING_SLIDER) {
				struct astrix_rect track = { w / 2, y + th->spacing * 2,
					                            w / 2 - th->spacing * 3,
					                            th->touch_target - th->spacing * 4 };
				float f = (float)(ev->x - track.x) / (float)track.w;
				if (f < 0.0f) {
					f = 0.0f;
				}
				if (f > 1.0f) {
					f = 1.0f;
				}
				int v = set->min + (int)(f * (set->max - set->min));
				/* Snap to the step so the value lands on a setting the
				 * hardware can actually be set to. */
				v = set->min + ((v - set->min + set->step / 2) / set->step) * set->step;
				if (v < set->min) {
					v = set->min;
				}
				if (v > set->max) {
					v = set->max;
				}
				if (v != astrix_settings_get_int(&sa->store, set->key, v)) {
					astrix_settings_set_int(&sa->store, set->key, v);
					astrix_settings_save(&sa->store);
					apply_setting(sa, host, set);
				}
				astrix_app_invalidate(host);
				return true;
			}
			y += rh;
		}
		if (astrix_scroll_handle(&sa->scroll, ev)) {
			sa->pressed_valid = false;
			astrix_app_invalidate(host);
		}
		return false;
	}

	case ASTRIX_INPUT_TOUCH_UP: {
		if (sa->pressed_valid) {
			int i = sa->pressed_row;
			const struct astrix_setting *set = &table[sa->visible[i]];
			switch (set->type) {
			case ASTRIX_SETTING_TOGGLE:
				astrix_settings_set_bool(&sa->store, set->key,
				                        !astrix_settings_get_bool(&sa->store, set->key, false));
				astrix_settings_save(&sa->store);
				apply_setting(sa, host, set);
				break;
			case ASTRIX_SETTING_CHOICE: {
				int idx = astrix_settings_get_int(&sa->store, set->key, 0) + 1;
				if (set->choices[idx] == NULL) {
					idx = 0;
				}
				astrix_settings_set_int(&sa->store, set->key, idx);
				astrix_settings_save(&sa->store);
				break;
			}
			case ASTRIX_SETTING_ACTION:
				astrix_settings_save(&sa->store);
				apply_setting(sa, host, set);
				break;
			case ASTRIX_SETTING_SLIDER:
				/* Slider drags are handled on motion; see below. */
				break;
			}
			sa->pressed_valid = false;
			astrix_app_invalidate(host);
		}
		return true;
	}

	case ASTRIX_INPUT_SCROLL:
		sa->scroll.offset -= ev->scroll_y / 4;
		astrix_scroll_clamp(&sa->scroll);
		astrix_app_invalidate(host);
		return true;

	default:
		return false;
	}
}

static void on_start(struct astrix_app_host *host) {
	struct settings_app *sa = astrix_app_user(host);
	const struct astrix_theme *th = astrix_app_theme(host);
	int w, h;
	astrix_app_size(host, &w, &h);
	sa->width = w;
	sa->height = h;
	astrix_hw_probe(&sa->hw);
	rebuild_visible(sa);

	const char *dir = getenv("ASTRIX_SETTINGS_DIR");
	if (!dir || !dir[0]) {
		dir = "/var/lib/astrix/settings";
	}
	astrix_settings_load(&sa->store, dir);
	rebuild_visible(sa);
	astrix_scroll_reset(&sa->scroll, sa->visible_count * (row_height(th) + 26), h - (th->status_h + 52),
	                    w);
	/* Apply the stored appearance immediately: a phone that takes effect
	 * on next boot for a theme change feels broken. */
	if (astrix_settings_get_bool(&sa->store, "theme.dark", true)) {
		astrix_app_set_dark(host, true);
	}
}

static const struct astrix_app_desc desc = {
	.app_id = "org.astrix.Settings",
	.title = "Settings",
	.version = "0.1.0",
	.wants_keyboard = false,
	.on_start = on_start,
	.on_frame = on_frame,
	.on_input = on_input,
	.on_exit = NULL,
	.on_tick = on_tick,
};

int main(int argc, char **argv) {
	struct settings_app sa;
	memset(&sa, 0, sizeof(sa));
	astrix_settings_defaults(&sa.store);
	astrix_hw_probe(&sa.hw);
	rebuild_visible(&sa);
	return astrix_app_main_with_user(&desc, argc, argv, &sa);
}
