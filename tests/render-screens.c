/*
 * Astrix OS - shell screenshot harness.
 *
 * Renders each shell screen to a PPM so the UI can be inspected on the build
 * host, without a compositor, a GPU or a phone. This is how the home screen,
 * shade, lock screen, launcher and switcher get verified during development.
 *
 * Usage: render-screens <output-directory>
 */

#include "astrix_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_ppm(const char *dir, const char *name, const struct astrix_shell *sh) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "cannot write %s\n", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", sh->width, sh->height);
	for (int y = 0; y < sh->height; y++) {
		for (int x = 0; x < sh->width; x++) {
			uint32_t p = sh->pixels[(size_t)y * sh->stride + x];
			fputc((p >> 16) & 0xFF, f);
			fputc((p >> 8) & 0xFF, f);
			fputc(p & 0xFF, f);
		}
	}
	fclose(f);
	printf("wrote %s\n", path);
}

/* Populate a realistic set of apps, mixing runtimes on purpose. */
static void seed_apps(struct astrix_shell *sh) {
	struct astrix_app a;
	memset(&a, 0, sizeof(a));

#define ADD(id_, name_, kind_, exec_, act_)                                              \
	do {                                                                                \
		memset(&a, 0, sizeof(a));                                                       \
		snprintf(a.id, sizeof(a.id), "%s", (id_));                                       \
		snprintf(a.name, sizeof(a.name), "%s", (name_));                                 \
		snprintf(a.exec, sizeof(a.exec), "%s", (const char *)(exec_));                                 \
		if ((const char *)(act_)) {                                                                   \
			snprintf(a.android_activity, sizeof(a.android_activity), "%s", (const char *)(act_));      \
		}                                                                               \
		a.kind = (kind_);                                                                \
		astrix_shell_add_app(sh, &a);                                                    \
	} while (0)

	ADD("org.astrix.Terminal", "Terminal", ASTRIX_APP_SYSTEM, "astrix-terminal", NULL);
	ADD("org.astrix.Files", "Files", ASTRIX_APP_SYSTEM, "astrix-files", NULL);
	ADD("org.astrix.Settings", "Settings", ASTRIX_APP_SYSTEM, "astrix-settings", NULL);
	ADD("org.astrix.SysInfo", "System Info", ASTRIX_APP_SYSTEM, "astrix-sysinfo", NULL);
	ADD("org.astrix.Packages", "Packages", ASTRIX_APP_SYSTEM, "astrix-packages", NULL);
	ADD("org.astrix.APKs", "APK Manager", ASTRIX_APP_SYSTEM, "astrix-apk-manager", NULL);
	ADD("org.astrix.Text", "Text Editor", ASTRIX_APP_NATIVE, "astrix-text", NULL);
	ADD("com.android.chrome", "Chrome", ASTRIX_APP_ANDROID, "waydroid-app", ".Main");
	ADD("org.mozilla.firefox", "Firefox", ASTRIX_APP_ANDROID, "waydroid-app", ".App");
	ADD("com.spotify.music", "Spotify", ASTRIX_APP_ANDROID, "waydroid-app", ".MainActivity");
	ADD("com.termux", "Termux", ASTRIX_APP_ANDROID, "waydroid-app", ".app.TermuxActivity");
	ADD("com.discord", "Discord", ASTRIX_APP_ANDROID, "waydroid-app", ".MainActivity");
#undef ADD

	/*
	 * Page 0 is the dock. Pin the four most-used apps there, put the next
	 * batch on page 1 and the rest on page 2, so every home page has real
	 * content and page navigation is visible.
	 */
	for (int i = 0; i < sh->app_count; i++) {
		if (i < 4) {
			sh->apps[i].pinned = true;
			sh->apps[i].page = 0;
		} else if (i < 12) {
			sh->apps[i].page = 1;
		} else {
			sh->apps[i].page = 2;
		}
	}
	astrix_shell_sort_apps(sh);
}

static void seed_status(struct astrix_shell *sh) {
	snprintf(sh->status_time, sizeof(sh->status_time), "12:45");
	sh->battery_percent = 87;
	sh->charging = false;
	sh->wifi_available = true;
	sh->wifi_on = true;
	sh->bt_available = true;
	sh->bt_on = false;
	sh->location_available = true;
	sh->mobile_available = true;
	sh->brightness = 70;
	sh->sound_on = true;
	sh->uptime_s = 3742;
	sh->memory_used_kb = 612 * 1024;
	sh->memory_total_kb = 3072 * 1024;
	sh->load_avg = 0.31f;
}

int main(int argc, char **argv) {
	/*
	 * render-screens <output-directory> [width height]
	 *
	 * The width/height are optional so that the same harness renders both the
	 * dev environment's geometry and each device profile's real panel. That
	 * matters because the panel sizes are the one thing about a phone that can
	 * be checked without the phone: a shell that lays out at 1024x768 can put
	 * its dock off the bottom of a 2400-row screen. tests/test-device-panels.sh
	 * asserts the geometry; this renders it so a human can look at it.
	 */
	const char *outdir = argc > 1 ? argv[1] : "build/screens";
	int panel_w = argc > 3 ? atoi(argv[2]) : 720;
	int panel_h = argc > 3 ? atoi(argv[3]) : 1600;

	struct astrix_shell *sh = astrix_shell_create(panel_w, panel_h);
	if (!sh) {
		fprintf(stderr, "cannot create a shell at %dx%d\n", panel_w, panel_h);
		return 1;
	}
	seed_apps(sh);
	seed_status(sh);

	/* Home screen. */
	astrix_shell_go_home(sh);
	astrix_shell_draw(sh);
	write_ppm(outdir, "01-home", sh);

	/* Home screen, second page. */
	sh->home_page = 1;
	astrix_shell_draw(sh);
	write_ppm(outdir, "02-home-grid", sh);
	sh->home_page = 0;

	/* App drawer. */
	sh->screen = ASTRIX_SCREEN_LAUNCHER;
	astrix_shell_draw(sh);
	write_ppm(outdir, "03-launcher", sh);

	/* Foreground app. */
	astrix_shell_open_app(sh, 0);
	astrix_shell_draw(sh);
	write_ppm(outdir, "04-app", sh);

	/* Notification shade. */
	astrix_shell_notify(sh, "org.astrix.Terminal", "Terminal", "Build finished",
	                    "astrix-compositor: no errors", ASTRIX_NOTIF_NORMAL);
	astrix_shell_notify(sh, "com.discord", "Discord", "Mika",
	                    "Are we still on for tonight?", ASTRIX_NOTIF_HIGH);
	astrix_shell_notify(sh, "org.astrix.System", "System", "Battery low",
	                    "5% remaining - connect a charger", ASTRIX_NOTIF_CRITICAL);
	sh->screen = ASTRIX_SCREEN_NOTIFICATION;
	astrix_shell_draw(sh);
	write_ppm(outdir, "05-notifications", sh);

	/* Quick settings. */
	sh->do_not_disturb = true;
	sh->bt_on = true;
	sh->battery_saver = true;
	sh->screen = ASTRIX_SCREEN_QUICK_SETTINGS;
	astrix_shell_draw(sh);
	write_ppm(outdir, "06-quick-settings", sh);

	/* App switcher with several running apps. */
	sh->screen = ASTRIX_SCREEN_HOME;
	sh->apps[0].running = true;
	sh->apps[1].running = true;
	sh->apps[7].running = true;
	sh->screen = ASTRIX_SCREEN_APP_SWITCHER;
	astrix_shell_draw(sh);
	write_ppm(outdir, "07-app-switcher", sh);

	/* Power menu. */
	sh->screen_from = ASTRIX_SCREEN_HOME;
	sh->screen = ASTRIX_SCREEN_POWER_MENU;
	astrix_shell_draw(sh);
	write_ppm(outdir, "08-power-menu", sh);

	/* Lock screen. */
	astrix_shell_toggle_lock(sh);
	astrix_shell_draw(sh);
	write_ppm(outdir, "09-lock", sh);
	astrix_shell_toggle_lock(sh);

	/* Light theme, to prove the theme swap works. */
	sh->dark_mode = false;
	sh->theme = astrix_theme_light();
	astrix_shell_go_home(sh);
	astrix_shell_draw(sh);
	write_ppm(outdir, "10-home-light", sh);

	/* A second panel size, to prove the layout is not hardcoded. */
	astrix_shell_set_size(sh, 1080, 2400);
	sh->dark_mode = true;
	sh->theme = astrix_theme_dark();
	astrix_shell_draw(sh);
	write_ppm(outdir, "11-home-1080x2400", sh);

	/*
	 * The size the QEMU guest actually runs at. The VM's virtio-gpu panel
	 * is 1024x768, not a phone's 720x1600, and the shell adopts whatever
	 * wl_output reports - so this is the picture worth looking at when
	 * asking "what does the booted VM look like". It is still rendered on
	 * the host, not captured from the VM: QMP screendump returns the text
	 * console on virtio-gpu, not the DRM plane (bug 16). What this proves
	 * is that the layout adapts to a landscape panel with the dock still
	 * on screen and hit-testable.
	 */
	astrix_shell_set_size(sh, 1024, 768);
	sh->dark_mode = true;
	sh->theme = astrix_theme_dark();
	astrix_shell_draw(sh);
	write_ppm(outdir, "12-home-1024x768-qemu", sh);

	/*
	 * The on-screen keyboard, at both layers and on a real phone panel.
	 * A keyboard is the kind of UI that can pass every geometry test and
	 * still be unreadable, so it gets rendered like anything else - and
	 * the symbol layer is rendered separately because that is where the
	 * letter rows are replaced.
	 */
	astrix_shell_set_size(sh, 1080, 2400);
	sh->screen = ASTRIX_SCREEN_HOME;
	sh->kbd_visible = true;
	astrix_shell_draw(sh);
	write_ppm(outdir, "13-keyboard", sh);

	sh->kbd_symbols = true;
	sh->kbd_caps = true;
	astrix_shell_draw(sh);
	write_ppm(outdir, "14-keyboard-symbols", sh);

	/* And with a key held, which is the state a slow tap produces. */
	sh->kbd_symbols = false;
	sh->kbd_caps = false;
	sh->kbd_press_x = 540;
	sh->kbd_press_y = 2100;
	astrix_shell_draw(sh);
	write_ppm(outdir, "15-keyboard-press", sh);
	sh->kbd_press_x = -1;
	sh->kbd_press_y = -1;
	sh->kbd_visible = false;

	astrix_shell_destroy(sh);
	return 0;
}
