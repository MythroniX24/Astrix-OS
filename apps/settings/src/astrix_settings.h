/*
 * Astrix OS - Astrix Settings.
 *
 * A settings store plus a list of controls bound to it.
 *
 * Two rules shape everything here:
 *
 *  1. A toggle is only offered if the thing it controls actually exists. A
 *     "Bluetooth" switch that cannot do anything is a lie, so the Bluetooth
 *     section is hidden entirely when /sys/class/bluetooth is absent (which
 *     is the case under QEMU).
 *
 *  2. A toggle that claims to change something must actually change it, or
 *     report that it could not. Brightness writes to the backlight device
 *     and reads the write back; if the read-back fails, the switch snaps off
 *     again and says why. Settings that silently do nothing are the single
 *     most common complaint about a phone OS.
 */

#ifndef ASTRIX_SETTINGS_H
#define ASTRIX_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

enum astrix_setting_type {
	ASTRIX_SETTING_TOGGLE,
	ASTRIX_SETTING_SLIDER,
	ASTRIX_SETTING_CHOICE,
	ASTRIX_SETTING_ACTION,
};

struct astrix_setting {
	const char *key;
	const char *label;
	const char *description;
	enum astrix_setting_type type;
	int min, max, step;   /* slider */
	const char *choices[6]; /* choice */
	/* The hardware this setting needs. Empty = always available. */
	const char *requires_hw;
};

#define ASTRIX_MAX_SETTINGS 64

struct astrix_settings {
	char dir[512];       /* e.g. /var/lib/astrix/settings */
	/* Values are stored as strings, one per key. A settings file that is
	 * human-editable and diffable is worth more than a binary blob when
	 * you are debugging a device. */
	char values[ASTRIX_MAX_SETTINGS][64];
	int value_count;
};

/* Probe which hardware is present. Fills the `hw_*` flags. */
struct astrix_hw {
	bool backlight;
	bool bluetooth;
	bool wifi;
	bool battery;
	bool accelerometer;
	bool rtc;
};

void astrix_hw_probe(struct astrix_hw *hw);

/* Default hardware requirements for the settings the store knows about. */
const struct astrix_setting *astrix_settings_table(int *count);

void astrix_settings_defaults(struct astrix_settings *s);
int astrix_settings_load(struct astrix_settings *s, const char *dir);
int astrix_settings_save(const struct astrix_settings *s);

const char *astrix_settings_get(const struct astrix_settings *s, const char *key,
                                const char *fallback);
bool astrix_settings_get_bool(const struct astrix_settings *s, const char *key, bool fallback);
int astrix_settings_get_int(const struct astrix_settings *s, const char *key, int fallback);
void astrix_settings_set(struct astrix_settings *s, const char *key, const char *value);
void astrix_settings_set_bool(struct astrix_settings *s, const char *key, bool value);
void astrix_settings_set_int(struct astrix_settings *s, const char *key, int value);

/* Whether a setting is usable given the probed hardware. */
bool astrix_setting_available(const struct astrix_setting *set, const struct astrix_hw *hw);

/* True if the process may write to the given sysfs path. */
bool astrix_settings_sysfs_writable(const char *path);

#endif /* ASTRIX_SETTINGS_H */
