/* Vortex shell - shared types.
 *
 * The shell is a single-threaded, event-driven program.  It only redraws
 * when something changed (a key press, the minute rolling over, a child
 * process finishing), and it never allocates in the steady state: all UI
 * memory comes from fixed buffers set up at start-up.
 */
#ifndef VX_H
#define VX_H

#include <stddef.h>
#include <stdint.h>

#define VX_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Logical keys of a KaiOS-style feature phone keypad. */
enum vx_key {
    VK_NONE = 0,
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_OK,
    VK_SOFT_LEFT, VK_SOFT_RIGHT,
    VK_BACK,             /* "C"/clear key, Backspace on KaiOS */
    VK_CALL, VK_END,     /* green / red keys; red doubles as power */
    VK_0, VK_1, VK_2, VK_3, VK_4, VK_5, VK_6, VK_7, VK_8, VK_9,
    VK_STAR, VK_HASH,
    VK_VOL_UP, VK_VOL_DOWN,
    VK_COUNT
};

enum vx_event_type { VX_EV_NONE = 0, VX_EV_KEY, VX_EV_QUIT };

struct vx_event {
    enum vx_event_type type;
    enum vx_key key;
    int pressed;  /* 1 = down, 0 = up */
    int repeat;   /* auto-repeat of a held key */
};

/* Runtime configuration, read from /etc/vortex/vortex.conf. */
struct vx_config {
    char font[256];
    char apps_dir[256];
    char cache_dir[256];
    char store_url[256];
    char run_cmd[256];
    char install_cmd[256];
    char remove_cmd[256];
    char call_cmd[256];
    char keymap[256];
    char backlight[256];   /* sysfs backlight dir, or "auto" */
    char tube_cmd[256];    /* Invidious helper (runtime/vortex-tube) */
    char tube_dir[256];    /* its settings, history, saved videos */
    int font_size;
    int screen_timeout;    /* seconds, 0 = never */
};

int vx_config_load(struct vx_config *cfg, const char *path);

const char *vx_key_name(enum vx_key k);
enum vx_key vx_key_from_name(const char *name);

/* Small helpers shared across modules. */
size_t vx_strlcpy(char *dst, const char *src, size_t size);
char *vx_read_file(const char *path, size_t *len_out, size_t max_len);
int vx_read_int(const char *path, long *out);
int vx_write_str(const char *path, const char *s);

#endif
