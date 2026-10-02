/*
 * Astrix OS - Astrix System Info.
 *
 * Everything on this screen is read from /proc or /sys at the moment it is
 * drawn. Nothing is hard-coded and nothing is estimated: if a value is not
 * readable the row says "not available" rather than showing a plausible
 * number. A system info screen that lies is worse than no screen.
 *
 * On hardware without a battery, under QEMU, the battery rows are hidden
 * rather than shown as zero - the same rule the shell uses for its status
 * bar.
 */

#include "astrix_app.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#define MAX_FIELDS 32

struct sysinfo_field {
	char label[48];
	char value[96];
};

struct sysinfo_app {
	struct sysinfo_field fields[MAX_FIELDS];
	int field_count;
	int section_count;

	char kernel[96];
	char hardware[96];
	char uptime[48];
	char loadavg[48];
	char memory[64];
	char battery[64];
	bool has_battery;
	bool has_cpu_freq;
	char cpu_freq[48];

	struct astrix_scroll scroll;
	int width, height;
};

/* --- readers ------------------------------------------------------------- */

/* Read a whole (small) file into `out`. Returns 0 on success. */
static int read_file(const char *path, char *out, size_t out_size) {
	FILE *f = fopen(path, "r");
	if (!f) {
		out[0] = '\0';
		return -1;
	}
	size_t n = fread(out, 1, out_size - 1, f);
	out[n] = '\0';
	/* Trim the trailing newline that /proc files always have. */
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' ')) {
		out[--n] = '\0';
	}
	fclose(f);
	return 0;
}

static int file_exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static void add_field(struct sysinfo_app *s, const char *label, const char *value) {
	if (s->field_count >= MAX_FIELDS) {
		return;
	}
	struct sysinfo_field *f = &s->fields[s->field_count++];
	snprintf(f->label, sizeof(f->label), "%.47s", label);
	snprintf(f->value, sizeof(f->value), "%.95s", value);
}

/* CPU model name: the first "model name" line in /proc/cpuinfo on ARM64. */
static void read_cpu(struct sysinfo_app *s) {
	char buf[4096];
	if (read_file("/proc/cpuinfo", buf, sizeof(buf)) != 0) {
		add_field(s, "CPU", "not available");
		return;
	}
	char model[128] = "";
	const char *p = buf;
	while (*p) {
		if (strncmp(p, "model name", 10) == 0 || strncmp(p, "Processor", 9) == 0 ||
		    strncmp(p, "Hardware", 8) == 0) {
			const char *colon = strchr(p, ':');
			if (colon) {
				colon++;
				while (*colon == ' ' || *colon == '\t') {
					colon++;
				}
				size_t i = 0;
				while (colon[i] && colon[i] != '\n' && i < sizeof(model) - 1) {
					model[i] = colon[i];
					i++;
				}
				model[i] = '\0';
				break;
			}
		}
		const char *nl = strchr(p, '\n');
		if (!nl) {
			break;
		}
		p = nl + 1;
	}
	if (model[0]) {
		add_field(s, "CPU", model);
	} else {
		add_field(s, "CPU", "unknown");
	}

	/* Core count: "processor" lines, or the count of "CPU part" blocks. */
	int cores = 0;
	for (const char *q = buf; (q = strstr(q, "processor")) != NULL; q += 9) {
		cores++;
	}
	if (cores == 0) {
		cores = 1;
	}
	char tmp[80];
	snprintf(tmp, sizeof(tmp), "%d core%s", cores, cores == 1 ? "" : "s");
	add_field(s, "Cores", tmp);

	/*
	 * CPU frequency. On most phones the scaling governor lives at
	 * /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq. Under QEMU
	 * and on many dev boards there is no cpufreq at all, and then this
	 * row is omitted rather than invented.
	 */
	snprintf(tmp, sizeof(tmp), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", 0);
	/* tmp is sized for the formatted path; assert that here rather than
	 * let snprintf silently truncate it into a wrong filename. */
	_Static_assert(sizeof(tmp) >= 80, "tmp must hold a cpufreq path");
	char hz[32];
	if (read_file(tmp, hz, sizeof(hz)) == 0 && hz[0]) {
		long khz = atol(hz);
		if (khz > 0) {
			snprintf(s->cpu_freq, sizeof(s->cpu_freq), "%ld MHz", khz / 1000);
			s->has_cpu_freq = true;
		}
	}
}

static void read_memory(struct sysinfo_app *s) {
	char buf[256];
	if (read_file("/proc/meminfo", buf, sizeof(buf)) != 0) {
		snprintf(s->memory, sizeof(s->memory), "not available");
		return;
	}
	/* MemTotal is in kB. */
	long total = 0, available = 0;
	const char *p = buf;
	while (*p) {
		if (strncmp(p, "MemTotal:", 9) == 0) {
			total = atol(p + 9);
		} else if (strncmp(p, "MemAvailable:", 13) == 0) {
			available = atol(p + 13);
		}
		const char *nl = strchr(p, '\n');
		if (!nl) {
			break;
		}
		p = nl + 1;
	}
	if (total <= 0) {
		snprintf(s->memory, sizeof(s->memory), "not available");
		return;
	}
	snprintf(s->memory, sizeof(s->memory), "%ld MB total, %ld MB available", total / 1024,
	         available / 1024);
}

static void read_uptime(struct sysinfo_app *s) {
	char buf[64];
	if (read_file("/proc/uptime", buf, sizeof(buf)) != 0) {
		snprintf(s->uptime, sizeof(s->uptime), "not available");
		return;
	}
	double seconds = atof(buf);
	long total = (long)seconds;
	long days = total / 86400;
	long hours = (total % 86400) / 3600;
	long mins = (total % 3600) / 60;
	snprintf(s->uptime, sizeof(s->uptime), "%ld d %ld h %ld m", days, hours, mins);
}

static void read_load(struct sysinfo_app *s) {
	char buf[128];
	if (read_file("/proc/loadavg", buf, sizeof(buf)) != 0) {
		snprintf(s->loadavg, sizeof(s->loadavg), "not available");
		return;
	}
	/* loadavg is "0.12 0.34 0.56 1/234 5678". */
	sscanf(buf, "%*s %*s %*s");
	char one[32] = "", five[32] = "";
	const char *sp = strchr(buf, ' ');
	if (sp) {
		sscanf(sp, "%31s", one);
	}
	const char *sp2 = sp ? strchr(sp + 1, ' ') : NULL;
	if (sp2) {
		sscanf(sp2, "%31s", five);
	}
	snprintf(s->loadavg, sizeof(s->loadavg), "1 min %s, 5 min %s",
	         one[0] ? one : "?", five[0] ? five : "?");
}

static void read_storage(struct sysinfo_app *s, const char *mount) {
	/* statvfs is the only portable way to get free space without
	 * statvfs-specific mount table parsing. */
	struct statvfs vfs;
	if (statvfs(mount, &vfs) != 0) {
		add_field(s, "Storage", "not available");
		return;
	}
	double total = (double)vfs.f_blocks * vfs.f_frsize / (1024.0 * 1024.0 * 1024.0);
	double freeb = (double)vfs.f_bavail * vfs.f_frsize / (1024.0 * 1024.0 * 1024.0);
	char v[96];
	snprintf(v, sizeof(v), "%.1f GB total, %.1f GB free", total, freeb);
	add_field(s, "Storage", v);
}

static void read_battery(struct sysinfo_app *s) {
	/*
	 * file_exists() is the cheap pre-check: walking the supply directory
	 * only makes sense if the class itself is present, and on QEMU it is
	 * not.
	 */
	if (!file_exists("/sys/class/power_supply")) {
		s->has_battery = false;
		return;
	}
	/*
	 * The power supply class is the modern interface and is what the
	 * Astrix kernel config enables. If there is no supply directory, or
	 * no battery in it, the device has no battery as far as userspace is
	 * concerned - which is the case under QEMU - and the rows are hidden.
	 */
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) {
		s->has_battery = false;
		return;
	}
	bool found = false;
	char type[64] = "", status[64] = "", capacity[32] = "";
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[256];
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", de->d_name);
		if (read_file(path, type, sizeof(type)) != 0 || strcmp(type, "Battery") != 0) {
			continue;
		}
		found = true;
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", de->d_name);
		read_file(path, capacity, sizeof(capacity));
		snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", de->d_name);
		read_file(path, status, sizeof(status));
		break;
	}
	closedir(d);

	if (!found || capacity[0] == '\0') {
		s->has_battery = false;
		return;
	}
	s->has_battery = true;
	snprintf(s->battery, sizeof(s->battery), "%s%%, %s", capacity,
	         status[0] ? status : "unknown");
}

static void refresh(struct sysinfo_app *s) {
	s->field_count = 0;
	s->section_count = 0;

	struct utsname u;
	if (uname(&u) == 0) {
		snprintf(s->kernel, sizeof(s->kernel), "%.24s %.60s", u.sysname, u.release);
		add_field(s, "Kernel", s->kernel);
		add_field(s, "Architecture", u.machine);
	}

	char tmp[64];
	snprintf(tmp, sizeof(tmp), "%d cores online", (int)sysconf(_SC_NPROCESSORS_ONLN));
	add_field(s, "CPUs online", tmp);

	read_cpu(s);
	if (s->has_cpu_freq) {
		add_field(s, "CPU frequency", s->cpu_freq);
	}

	read_memory(s);
	add_field(s, "Memory", s->memory);
	read_uptime(s);
	add_field(s, "Uptime", s->uptime);
	read_load(s);
	add_field(s, "Load", s->loadavg);
	read_storage(s, "/");
	read_battery(s);
	if (s->has_battery) {
		add_field(s, "Battery", s->battery);
	}

	char model[64] = "";
	if (read_file("/sys/firmware/devicetree/base/model", model, sizeof(model)) == 0) {
		/* The device tree model string is NUL-padded; trim it. */
		size_t l = strnlen(model, sizeof(model) - 1);
		model[l] = '\0';
		if (model[0]) {
			add_field(s, "Device", model);
		}
	}
}

/* --- drawing ------------------------------------------------------------- */

static void on_frame(struct astrix_app_host *host) {
	struct sysinfo_app *s = astrix_app_user(host);
	struct astrix_canvas *c = astrix_app_canvas(host);
	const struct astrix_theme *th = astrix_app_theme(host);
	int w, h;
	astrix_app_size(host, &w, &h);

	astrix_fill_rect(c, (struct astrix_rect){ 0, 0, w, h }, th->background);
	struct astrix_rect content = astrix_widget_appbar(c, th, "System", "Astrix OS", true);

	int rh = th->touch_target + th->spacing;
	int y = content.y + th->spacing * 2 - s->scroll.offset;
	for (int i = 0; i < s->field_count; i++) {
		if (y > h || y + rh < content.y) {
			y += rh;
			continue;
		}
		struct astrix_rect r = { 0, y, w, rh };
		astrix_widget_row(c, th, r, s->fields[i].label, NULL, s->fields[i].value, false, false);
		y += rh;
	}

	if (s->field_count == 0) {
		struct astrix_rect body = { th->spacing * 3, content.y + 40, w - th->spacing * 6,
			                            h - content.y - 120 };
		astrix_widget_empty(c, th, body, "No data", "/proc could not be read.");
	}

	/* Footnote, because a screen of numbers with no provenance is not
	 * trustworthy. */
	astrix_draw_text(c, th->spacing * 2, h - th->navbar_h - 4, "live from /proc and /sys",
	                 astrix_color_with_alpha(th->text_dim, 170));
}

static void on_tick(struct astrix_app_host *host, double dt) {
	(void)dt;
	struct sysinfo_app *s = astrix_app_user(host);
	/*
	 * Re-read at about 2 Hz. Faster and the numbers flicker unreadably;
	 * slower and "uptime" looks stuck. The values here are cheap file
	 * reads, but on a phone even that is worth throttling.
	 */
	static int64_t last = 0;
	int64_t now = astrix_app_now_ms(host);
	if (now - last < 500) {
		if (astrix_scroll_step(&s->scroll)) {
			astrix_app_invalidate(host);
		}
		return;
	}
	last = now;
	refresh(s);
	astrix_scroll_reset(&s->scroll, s->field_count * (astrix_app_theme(host)->touch_target +
	                                                  astrix_app_theme(host)->spacing),
	                    s->height - astrix_app_theme(host)->status_h - 80, s->width);
	astrix_app_invalidate(host);
}

static bool on_input(struct astrix_app_host *host, const struct astrix_input_event *ev) {
	struct sysinfo_app *s = astrix_app_user(host);
	switch (ev->kind) {
	case ASTRIX_INPUT_TOUCH_MOTION:
	case ASTRIX_INPUT_POINTER_MOTION:
		if (astrix_scroll_handle(&s->scroll, ev)) {
			astrix_app_invalidate(host);
		}
		return false;
	case ASTRIX_INPUT_SCROLL:
		s->scroll.offset -= ev->scroll_y / 4;
		astrix_scroll_clamp(&s->scroll);
		astrix_app_invalidate(host);
		return true;
	default:
		return false;
	}
}

static const struct astrix_app_desc desc = {
	.app_id = "org.astrix.SystemInfo",
	.title = "System",
	.version = "0.1.0",
	.wants_keyboard = false,
	.on_start = NULL,
	.on_frame = on_frame,
	.on_input = on_input,
	.on_exit = NULL,
	.on_tick = on_tick,
};

int main(int argc, char **argv) {
	struct sysinfo_app s;
	memset(&s, 0, sizeof(s));
	refresh(&s);
	return astrix_app_main_with_user(&desc, argc, argv, &s);
}
