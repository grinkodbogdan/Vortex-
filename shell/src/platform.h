/* Display + input backend.
 *
 * The shell renders into a 32-bit XRGB8888 back buffer owned by the
 * platform; plat_present() converts/copies it to the real display.
 *
 * Backends:
 *   platform_linux.c    - /dev/fb0 + evdev (the device build)
 *   platform_headless.c - in-memory, driven by a key script; used by tests
 */
#ifndef VX_PLATFORM_H
#define VX_PLATFORM_H

#include "vx.h"

struct vx_surface {
    uint32_t *pixels;  /* XRGB8888, row-major */
    int w, h;
    int stride;        /* in pixels */
};

struct vx_plat_opts {
    const char *keymap_path;
    const char *script;       /* headless only */
    int width, height;        /* headless only */
};

int  plat_init(const struct vx_plat_opts *opts, struct vx_surface *out);
void plat_present(void);
/* Wait up to timeout_ms (-1 = forever) for one event.  Returns 1 if *ev was
 * filled, 0 on timeout. */
int  plat_wait_event(struct vx_event *ev, int timeout_ms);
/* Blank/unblank the panel (screen timeout). */
void plat_set_blank(int blank);
/* Another process is about to own the display (a web app); stop touching
 * it until plat_resume_display(). Input keeps flowing so the shell can
 * still catch the End key. */
void plat_release_display(void);
void plat_resume_display(void);
void plat_shutdown(void);

/* Keymap: evdev keycode -> vx_key. Shared by the Linux backend and tests. */
#define VX_KEYMAP_SIZE 0x300
void keymap_defaults(enum vx_key map[VX_KEYMAP_SIZE]);
int  keymap_load(enum vx_key map[VX_KEYMAP_SIZE], const char *path);

#endif
