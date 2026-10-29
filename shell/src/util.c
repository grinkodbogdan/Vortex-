#include "vx.h"
#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

size_t vx_strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

/* Read a whole (small) file into a NUL-terminated heap buffer. */
char *vx_read_file(const char *path, size_t *len_out, size_t max_len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t cap = 0, len = 0;
    if (!f)
        return NULL;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            char *nb;
            if (ncap > max_len + 1)
                ncap = max_len + 1;
            if (ncap <= len + 1) { /* too big */
                free(buf);
                fclose(f);
                errno = EFBIG;
                return NULL;
            }
            nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        size_t r = fread(buf + len, 1, cap - len - 1, f);
        len += r;
        if (r == 0)
            break;
    }
    fclose(f);
    buf[len] = '\0';
    if (len_out)
        *len_out = len;
    return buf;
}

int vx_read_int(const char *path, long *out)
{
    char buf[32];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    *out = strtol(buf, NULL, 10);
    return 0;
}

int vx_write_str(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0)
        return -1;
    n = write(fd, s, strlen(s));
    close(fd);
    return n < 0 ? -1 : 0;
}

static const char *const key_names[VK_COUNT] = {
    [VK_NONE] = "none",
    [VK_UP] = "up", [VK_DOWN] = "down", [VK_LEFT] = "left", [VK_RIGHT] = "right",
    [VK_OK] = "ok", [VK_SOFT_LEFT] = "soft_left", [VK_SOFT_RIGHT] = "soft_right",
    [VK_BACK] = "back", [VK_CALL] = "call", [VK_END] = "end",
    [VK_0] = "0", [VK_1] = "1", [VK_2] = "2", [VK_3] = "3", [VK_4] = "4",
    [VK_5] = "5", [VK_6] = "6", [VK_7] = "7", [VK_8] = "8", [VK_9] = "9",
    [VK_STAR] = "star", [VK_HASH] = "hash",
    [VK_VOL_UP] = "vol_up", [VK_VOL_DOWN] = "vol_down",
};

const char *vx_key_name(enum vx_key k)
{
    return (k >= 0 && k < VK_COUNT && key_names[k]) ? key_names[k] : "?";
}

enum vx_key vx_key_from_name(const char *name)
{
    for (int i = 0; i < VK_COUNT; i++)
        if (key_names[i] && strcmp(key_names[i], name) == 0)
            return (enum vx_key)i;
    return VK_NONE;
}

/* Default evdev mapping.  The Nokia 6300 4G keypad matrix is described in
 * the device tree; these cover the usual codes plus a desktop keyboard so
 * the shell is usable on a PC VT. /etc/vortex/keymap.conf overrides them. */
void keymap_defaults(enum vx_key map[VX_KEYMAP_SIZE])
{
    static const struct { int code; enum vx_key key; } defs[] = {
        { KEY_UP, VK_UP }, { KEY_DOWN, VK_DOWN },
        { KEY_LEFT, VK_LEFT }, { KEY_RIGHT, VK_RIGHT },
        { KEY_ENTER, VK_OK }, { KEY_KPENTER, VK_OK }, { KEY_OK, VK_OK },
        { KEY_SELECT, VK_OK },
        { KEY_F1, VK_SOFT_LEFT }, { KEY_MENU, VK_SOFT_LEFT },
        { KEY_F2, VK_SOFT_RIGHT }, { KEY_COMPOSE, VK_SOFT_RIGHT },
        { KEY_BACKSPACE, VK_BACK }, { KEY_BACK, VK_BACK }, { KEY_ESC, VK_BACK },
        { KEY_SEND, VK_CALL }, { KEY_PHONE, VK_CALL }, { KEY_F3, VK_CALL },
        { KEY_END, VK_END }, { KEY_POWER, VK_END }, { KEY_F4, VK_END },
        { KEY_0, VK_0 }, { KEY_1, VK_1 }, { KEY_2, VK_2 }, { KEY_3, VK_3 },
        { KEY_4, VK_4 }, { KEY_5, VK_5 }, { KEY_6, VK_6 }, { KEY_7, VK_7 },
        { KEY_8, VK_8 }, { KEY_9, VK_9 },
        { KEY_KP0, VK_0 }, { KEY_KP1, VK_1 }, { KEY_KP2, VK_2 }, { KEY_KP3, VK_3 },
        { KEY_KP4, VK_4 }, { KEY_KP5, VK_5 }, { KEY_KP6, VK_6 }, { KEY_KP7, VK_7 },
        { KEY_KP8, VK_8 }, { KEY_KP9, VK_9 },
        { KEY_NUMERIC_0, VK_0 }, { KEY_NUMERIC_1, VK_1 }, { KEY_NUMERIC_2, VK_2 },
        { KEY_NUMERIC_3, VK_3 }, { KEY_NUMERIC_4, VK_4 }, { KEY_NUMERIC_5, VK_5 },
        { KEY_NUMERIC_6, VK_6 }, { KEY_NUMERIC_7, VK_7 }, { KEY_NUMERIC_8, VK_8 },
        { KEY_NUMERIC_9, VK_9 },
        { KEY_NUMERIC_STAR, VK_STAR }, { KEY_KPASTERISK, VK_STAR },
        { KEY_NUMERIC_POUND, VK_HASH }, { KEY_KPSLASH, VK_HASH },
        { KEY_VOLUMEUP, VK_VOL_UP }, { KEY_VOLUMEDOWN, VK_VOL_DOWN },
    };
    memset(map, 0, sizeof(enum vx_key) * VX_KEYMAP_SIZE);
    for (size_t i = 0; i < VX_ARRAY_LEN(defs); i++)
        if (defs[i].code < VX_KEYMAP_SIZE)
            map[defs[i].code] = defs[i].key;
}

/* keymap.conf: "<evdev code> <key name>" per line, '#' comments.
 * Codes may be decimal or 0x-hex. A key name of "none" unmaps a code. */
int keymap_load(enum vx_key map[VX_KEYMAP_SIZE], const char *path)
{
    char line[128];
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char name[32];
        char *hash = strchr(line, '#');
        long code;
        char *end;
        if (hash)
            *hash = '\0';
        code = strtol(line, &end, 0);
        if (end == line)
            continue;
        if (sscanf(end, "%31s", name) != 1)
            continue;
        if (code < 0 || code >= VX_KEYMAP_SIZE)
            continue;
        map[code] = vx_key_from_name(name);
    }
    fclose(f);
    return 0;
}

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

int vx_config_load(struct vx_config *cfg, const char *path)
{
    static const struct { const char *key; size_t off; } strs[] = {
#define S(k) { #k, offsetof(struct vx_config, k) }
        S(font), S(apps_dir), S(cache_dir), S(store_url), S(run_cmd),
        S(install_cmd), S(remove_cmd), S(call_cmd), S(keymap), S(backlight),
#undef S
    };
    char line[512];
    FILE *f;

    memset(cfg, 0, sizeof(*cfg));
    vx_strlcpy(cfg->font, "/usr/share/vortex/fonts/Roboto-Regular.ttf", sizeof(cfg->font));
    vx_strlcpy(cfg->apps_dir, "/var/lib/vortex/apps", sizeof(cfg->apps_dir));
    vx_strlcpy(cfg->cache_dir, "/var/cache/vortex", sizeof(cfg->cache_dir));
    vx_strlcpy(cfg->store_url, "https://banana-hackers.gitlab.io/store-db/data.json",
               sizeof(cfg->store_url));
    vx_strlcpy(cfg->run_cmd, "/usr/libexec/vortex/vortex-kai-run", sizeof(cfg->run_cmd));
    vx_strlcpy(cfg->install_cmd, "/usr/libexec/vortex/vortex-kai-install", sizeof(cfg->install_cmd));
    vx_strlcpy(cfg->remove_cmd, "/usr/libexec/vortex/vortex-kai-remove", sizeof(cfg->remove_cmd));
    vx_strlcpy(cfg->call_cmd, "/usr/libexec/vortex/vortex-call", sizeof(cfg->call_cmd));
    vx_strlcpy(cfg->keymap, "/etc/vortex/keymap.conf", sizeof(cfg->keymap));
    vx_strlcpy(cfg->backlight, "auto", sizeof(cfg->backlight));
    cfg->font_size = 15;
    cfg->screen_timeout = 30;

    if (!path || !(f = fopen(path, "r")))
        return path ? -1 : 0;
    while (fgets(line, sizeof(line), f)) {
        char *eq, *k, *v;
        size_t i;
        if (line[0] == '#' || !(eq = strchr(line, '=')))
            continue;
        *eq = '\0';
        k = trim(line);
        v = trim(eq + 1);
        if (strcmp(k, "font_size") == 0) {
            cfg->font_size = atoi(v);
            continue;
        }
        if (strcmp(k, "screen_timeout") == 0) {
            cfg->screen_timeout = atoi(v);
            continue;
        }
        for (i = 0; i < VX_ARRAY_LEN(strs); i++) {
            if (strcmp(k, strs[i].key) == 0) {
                vx_strlcpy((char *)cfg + strs[i].off, v, 256);
                break;
            }
        }
        if (i == VX_ARRAY_LEN(strs))
            fprintf(stderr, "vortex: %s: unknown key '%s'\n", path, k);
    }
    fclose(f);
    if (cfg->font_size < 8 || cfg->font_size > 40)
        cfg->font_size = 15;
    return 0;
}
