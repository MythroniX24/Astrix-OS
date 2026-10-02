/*
 * Astrix OS - host test for the shell's framebuffer ownership rules.
 *
 * Why this exists: on a real panel the shell's framebuffer is not its own
 * allocation. The Wayland host mmaps a wl_shm buffer and points the shell's
 * `pixels` at it, so the shell draws straight into the buffer the compositor
 * uploads (no per-frame copy). The shell therefore has two possible owners
 * for `pixels`, and the resize path has to tell them apart.
 *
 * It did not. shell_resize() munmap'd the old buffers - and the old buffer
 * *is* shell->pixels - then astrix_shell_set_size() saw a size change and
 * called free() on that now-unmapped mmap pointer. Under the host smoke test
 * the compositor reported the default 720x1600, so the size already matched
 * and the resize returned early; the bug only appeared on the real panel
 * (1024x768), where it SIGSEGV'd the shell a few seconds into every boot.
 *
 * This test aliases an mmap'd region the way the Wayland host does, resizes,
 * and checks the mapping is untouched. free() on the mapping would abort the
 * test, so simply reaching the end is the regression signal.
 *
 * Host test: no Wayland, no compositor, no QEMU.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "astrix_shell.h"

static int failures;

static void check(bool ok, const char *what) {
	printf("%s %s\n", ok ? "  +" : "  x", what);
	if (!ok) {
		failures++;
	}
}

int main(void) {
	/* 1. A shell that owns its framebuffer reallocates on resize. */
	struct astrix_shell *owned = astrix_shell_create(720, 1600);
	check(owned != NULL, "shell created");
	check(owned->pixels != NULL, "a fresh shell owns a framebuffer");
	check(owned->pixels_owned, "a fresh shell reports its framebuffer as owned");

	astrix_shell_set_size(owned, 200, 300);
	check(owned->width == 200 && owned->height == 300, "owned resize updates the size");
	check(owned->buffer_size == (size_t)200 * 300 * 4, "owned resize updates buffer_size");
	check(owned->canvas.pixels == owned->pixels, "canvas follows the owned framebuffer");
	check(owned->canvas.width == 200 && owned->canvas.height == 300, "canvas is resized");
	astrix_shell_destroy(owned);

	/* 2. A shell whose framebuffer is aliased onto a host mapping must leave
	 *    that mapping alone on resize and on destroy. */
	struct astrix_shell *sh = astrix_shell_create(720, 1600);
	check(sh != NULL, "shell created for the aliased case");

	size_t sz = (size_t)1024 * 768 * 4;
	void *region = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(region != MAP_FAILED, "test mapping created");
	if (region == MAP_FAILED) {
		return 1;
	}

	/* Exactly what gui/shell/src/main.c does after creating the shm buffers. */
	sh->pixels = region;
	sh->pixels_owned = false;
	sh->buffer_size = sz;

	astrix_shell_set_size(sh, 1024, 768);
	check(sh->pixels == region, "resize leaves an aliased framebuffer alone");
	check(sh->width == 1024 && sh->height == 768, "aliased resize updates the size");
	check(sh->canvas.pixels == region, "canvas follows the aliased framebuffer");
	check(sh->canvas.width == 1024 && sh->canvas.height == 768, "canvas is resized");
	check(sh->buffer_size == sz, "the host mapping's size is preserved");

	/* Destroy must not free the host's mapping either. */
	astrix_shell_destroy(sh);

	/* The mapping is still ours to write and read: proof it was not freed
	 * or unmapped. Writing through a freed pointer would corrupt the heap
	 * and reading a freed pointer is undefined. */
	volatile uint32_t *probe = region;
	probe[0] = 0xdeadbeefu;
	probe[255] = 0xfeedfaceu;
	check(probe[0] == 0xdeadbeefu && probe[255] == 0xfeedfaceu,
	      "the host mapping is still valid after destroy");
	munmap(region, sz);

	printf("%s\n", failures ? "shell size checks FAILED" : "all shell size checks passed");
	return failures ? 1 : 0;
}
