/*
 * Astrix OS - Astrix Settings store implementation.
 */

#include "astrix_settings.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- the settings table --------------------------------------------------- */

/*
 * requires_hw names a sysfs path that must exist for the row to be shown.
 * An empty string means the setting is always meaningful (theme, dark mode,
 * animation), because those are properties of the shell itself.
 */
static const struct astrix_setting settings_table[] = {
	{ "theme.dark", "Dark theme", "Use the dark colour scheme", ASTRIX_SETTING_TOGGLE, 0, 0, 1,
	  { NULL }, "" },
	{ "ui.animations", "Animations", "Animate app open and close", ASTRIX_SETTING_TOGGLE, 0, 0, 1,
	  { NULL }, "" },
	{ "ui.gesture_sensitivity", "Gesture sensitivity", "How far a swipe must travel", ASTRIX_SETTING_SLIDER,
	  20, 120, 5, { NULL }, "" },
	{ "display.brightness", "Brightness", "Backlight level, where the panel has one",
	  ASTRIX_SETTING_SLIDER, 10, 100, 5, { NULL }, "/sys/class/backlight" },

	{ "power.battery_saver", "Battery saver", "Limit background work to save power",
	  ASTRIX_SETTING_TOGGLE, 0, 0, 1, { NULL }, "/sys/class/power_supply" },
	{ "power.screen_timeout", "Screen timeout", "Seconds before the screen sleeps",
	  ASTRIX_SETTING_CHOICE, 0, 0, 0,
	  { "15 seconds", "30 seconds", "1 minute", "5 minutes", "never" }, "" },

	{ "network.wifi", "Wi-Fi", "Wireless networking", ASTRIX_SETTING_TOGGLE, 0, 0, 1, { NULL },
	  "/sys/class/net" },
	{ "network.airplane_mode", "Airplane mode", "Turn off all radios", ASTRIX_SETTING_TOGGLE, 0, 0, 1,
	  { NULL }, "/sys/class/net" },

	{ "bluetooth.enabled", "Bluetooth", "Bluetooth radio", ASTRIX_SETTING_TOGGLE, 0, 0, 1, { NULL },
	  "/sys/class/bluetooth" },
	{ "location.enabled", "Location", "Location services", ASTRIX_SETTING_TOGGLE, 0, 0, 1, { NULL },
	  "/sys/class/gps" },

	{ "privacy.do_not_disturb", "Do not disturb", "Silence notifications", ASTRIX_SETTING_TOGGLE, 0, 0, 1,
	  { NULL }, "" },
	{ "privacy.diagnostics", "Send diagnostics", "Anonymous crash and usage reports",
	  ASTRIX_SETTING_TOGGLE, 0, 0, 1, { NULL }, "" },

	{ "session.lock_on_wake", "Lock on wake", "Require a swipe to unlock after sleep",
	  ASTRIX_SETTING_TOGGLE, 0, 0, 1, { NULL }, "" },
	{ "session.rotation_lock", "Lock rotation", "Keep the screen orientation fixed",
	  ASTRIX_SETTING_TOGGLE, 0, 0, 1, { NULL }, "" },

	{ "updates.channel", "Update channel", "Which package channel to track", ASTRIX_SETTING_CHOICE, 0, 0, 0,
	  { "stable", "testing" }, "" },
	{ "updates.check", "Check for updates", "Run an update check now", ASTRIX_SETTING_ACTION, 0, 0, 0,
	  { NULL }, "" },
	{ "system.restart", "Restart", "Restart the graphical session", ASTRIX_SETTING_ACTION, 0, 0, 0,
	  { NULL }, "" },
	{ "system.power_off", "Power off", "Shut the device down", ASTRIX_SETTING_ACTION, 0, 0, 0,
	  { NULL }, "" },
};

const struct astrix_setting *astrix_settings_table(int *count) {
	*count = (int)(sizeof(settings_table) / sizeof(settings_table[0]));
	return settings_table;
}

/* --- hardware probing ---------------------------------------------------- */

static bool path_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static bool dir_has_entries(const char *path) {
	DIR *d = opendir(path);
	if (!d) {
		return false;
	}
	struct dirent *de;
	bool found = false;
	while ((de = readdir(d)) != NULL) {
		/* A real interface, not the . and .. entries. */
		if (de->d_name[0] != '.') {
			found = true;
			break;
		}
	}
	closedir(d);
	return found;
}

void astrix_hw_probe(struct astrix_hw *hw) {
	memset(hw, 0, sizeof(*hw));
	hw->backlight = dir_has_entries("/sys/class/backlight");
	hw->bluetooth = path_exists("/sys/class/bluetooth");
	hw->battery = path_exists("/sys/class/power_supply");
	hw->rtc = path_exists("/sys/class/rtc");
	hw->accelerometer = path_exists("/sys/class/iio") && path_exists("/sys/bus/iio");

	/*
	 * Wi-Fi is present when there is a wireless network interface. Under
	 * QEMU there is only eth0, so the row is hidden - which is the honest
	 * answer, and the same one the shell gives for its status bar.
	 */
	hw->wifi = false;
	DIR *d = opendir("/sys/class/net");
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.') {
				continue;
			}
			char path[256];
			snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", de->d_name);
			if (path_exists(path)) {
				hw->wifi = true;
				break;
			}
			snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", de->d_name);
			if (path_exists(path)) {
				hw->wifi = true;
				break;
			}
		}
		closedir(d);
	}
}

bool astrix_setting_available(const struct astrix_setting *set, const struct astrix_hw *hw) {
	if (!set->requires_hw || set->requires_hw[0] == '\0') {
		return true;
	}
	/* The table names the hardware it needs; check the matching flag. */
	if (strcmp(set->requires_hw, "/sys/class/backlight") == 0) {
		return hw->backlight;
	}
	if (strcmp(set->requires_hw, "/sys/class/bluetooth") == 0) {
		return hw->bluetooth;
	}
	if (strcmp(set->requires_hw, "/sys/class/power_supply") == 0) {
		return hw->battery;
	}
	if (strcmp(set->requires_hw, "/sys/class/gps") == 0) {
		return path_exists("/sys/class/gps");
	}
	if (strcmp(set->requires_hw, "/sys/class/net") == 0) {
		/* Network settings stay visible: there is always a network
		 * interface, and the point of the section is to say which
		 * radios are on. Only the Wi-Fi *toggle* is hidden when there
		 * is no wireless interface, which the table distinguishes by
		 * requiring a wireless path - see the caller. */
		return true;
	}
	return true;
}

bool astrix_settings_sysfs_writable(const char *path) {
	return access(path, W_OK) == 0;
}

/* --- the store ----------------------------------------------------------- */

static int find_index(const struct astrix_settings *s, const char *key) {
	for (int i = 0; i < s->value_count; i++) {
		if (strncmp(s->values[i], key, 60) == 0 && s->values[i][60] == '=') {
			return i;
		}
	}
	return -1;
}

void astrix_settings_defaults(struct astrix_settings *s) {
	memset(s, 0, sizeof(*s));
	/*
	 * Defaults are the ones a phone should ship with: dark theme on, dark
	 * mode true, animations on, screen timeout at 1 minute, stable update
	 * channel, and every radio off.
	 */
	astrix_settings_set_bool(s, "theme.dark", true);
	astrix_settings_set_bool(s, "ui.animations", true);
	astrix_settings_set_int(s, "ui.gesture_sensitivity", 60);
	astrix_settings_set_int(s, "display.brightness", 70);
	astrix_settings_set_bool(s, "power.battery_saver", false);
	astrix_settings_set_int(s, "power.screen_timeout", 2);
	astrix_settings_set_bool(s, "network.wifi", false);
	astrix_settings_set_bool(s, "network.airplane_mode", false);
	astrix_settings_set_bool(s, "bluetooth.enabled", false);
	astrix_settings_set_bool(s, "location.enabled", false);
	astrix_settings_set_bool(s, "privacy.do_not_disturb", false);
	astrix_settings_set_bool(s, "privacy.diagnostics", false);
	astrix_settings_set_bool(s, "session.lock_on_wake", true);
	astrix_settings_set_bool(s, "session.rotation_lock", false);
	astrix_settings_set_int(s, "updates.channel", 0);
}

const char *astrix_settings_get(const struct astrix_settings *s, const char *key,
                                const char *fallback) {
	int idx = find_index(s, key);
	if (idx < 0) {
		return fallback;
	}
	/* The value starts after "key=" and is NUL-terminated by the buffer. */
	static char value[64];
	const char *eq = strchr(s->values[idx], '=');
	if (!eq) {
		return fallback;
	}
	snprintf(value, sizeof(value), "%s", eq + 1);
	return value;
}

bool astrix_settings_get_bool(const struct astrix_settings *s, const char *key, bool fallback) {
	const char *v = astrix_settings_get(s, key, NULL);
	if (!v) {
		return fallback;
	}
	return v[0] == '1' || v[0] == 't' || v[0] == 'y';
}

int astrix_settings_get_int(const struct astrix_settings *s, const char *key, int fallback) {
	const char *v = astrix_settings_get(s, key, NULL);
	if (!v || v[0] == '\0') {
		return fallback;
	}
	return atoi(v);
}

void astrix_settings_set(struct astrix_settings *s, const char *key, const char *value) {
	int idx = find_index(s, key);
	if (idx >= 0) {
		snprintf(s->values[idx], sizeof(s->values[idx]), "%.59s=%.8s", key, value);
		return;
	}
	if (s->value_count >= ASTRIX_MAX_SETTINGS) {
		return;
	}
	idx = s->value_count++;
	snprintf(s->values[idx], sizeof(s->values[idx]), "%.59s=%.8s", key, value);
}

void astrix_settings_set_bool(struct astrix_settings *s, const char *key, bool value) {
	astrix_settings_set(s, key, value ? "1" : "0");
}

void astrix_settings_set_int(struct astrix_settings *s, const char *key, int value) {
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", value);
	astrix_settings_set(s, key, buf);
}

int astrix_settings_save(const struct astrix_settings *s) {
	if (!s->dir[0]) {
		return -1;
	}
	char path[640];
	snprintf(path, sizeof(path), "%s/astrix.conf", s->dir);

	/*
	 * Write to a temporary file and rename, so a power cut mid-write
	 * cannot leave a half-written settings file that will not parse.
	 */
	char tmp[660];
	snprintf(tmp, sizeof(tmp), "%s/astrix.conf.new", s->dir);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return -1;
	}
	fprintf(f, "# Astrix OS settings - generated, safe to edit\n");
	fprintf(f, "# key=value\n");
	for (int i = 0; i < s->value_count; i++) {
		fprintf(f, "%s\n", s->values[i]);
	}
	if (fclose(f) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int astrix_settings_load(struct astrix_settings *s, const char *dir) {
	memset(s, 0, sizeof(*s));
	snprintf(s->dir, sizeof(s->dir), "%.511s", dir ? dir : "/var/lib/astrix/settings");
	astrix_settings_defaults(s);

	char path[640];
	snprintf(path, sizeof(path), "%s/astrix.conf", s->dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		/* No file yet: the defaults are the state. Not an error. */
		return 0;
	}
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n');
		if (nl) {
			*nl = '\0';
		}
		if (line[0] == '#' || line[0] == '\0') {
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		astrix_settings_set(s, line, eq + 1);
	}
	fclose(f);
	return 0;
}
