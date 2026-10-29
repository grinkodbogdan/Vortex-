/* Headless backend for tests and screenshots.
 *
 * The key script is a list of tokens separated by spaces, commas or
 * newlines (or the path of a file holding them, prefixed with '@'):
 *
 *   up down ok soft_left 5 hash ...   press and release a key
 *   hold:end                          long-press (down, repeats, up)
 *   wait:500                          let 500 ms pass (child processes run)
 *   shot:out.ppm                      write the current frame as PPM
 *   quit                              stop (also implied at end of script)
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
    struct vx_surface surf;
    char *script;
    char *cursor;
    enum vx_key pending_release;
    int hold_left;
    enum vx_key hold_key;
} H;

int plat_init(const struct vx_plat_opts *opts, struct vx_surface *out)
{
    H.surf.w = opts->width > 0 ? opts->width : 240;
    H.surf.h = opts->height > 0 ? opts->height : 320;
    H.surf.stride = H.surf.w;
    H.surf.pixels = calloc((size_t)H.surf.w * H.surf.h, sizeof(uint32_t));
    if (!H.surf.pixels)
        return -1;
    if (opts->script && opts->script[0] == '@') {
        H.script = vx_read_file(opts->script + 1, NULL, 1 << 20);
        if (!H.script) {
            perror(opts->script + 1);
            return -1;
        }
    } else {
        H.script = strdup(opts->script ? opts->script : "");
    }
    H.cursor = H.script;
    *out = H.surf;
    return 0;
}

void plat_present(void) {}

static int write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", H.surf.w, H.surf.h);
    for (int i = 0; i < H.surf.w * H.surf.h; i++) {
        uint32_t p = H.surf.pixels[i];
        unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

static char *next_token(void)
{
    char *t;
    while (*H.cursor && strchr(" ,\t\r\n", *H.cursor))
        H.cursor++;
    if (!*H.cursor)
        return NULL;
    t = H.cursor;
    while (*H.cursor && !strchr(" ,\t\r\n", *H.cursor))
        H.cursor++;
    if (*H.cursor)
        *H.cursor++ = '\0';
    return t;
}

int plat_wait_event(struct vx_event *ev, int timeout_ms)
{
    char *t;
    (void)timeout_ms;

    memset(ev, 0, sizeof(*ev));
    if (H.hold_left > 0) {
        ev->type = VX_EV_KEY;
        ev->key = H.hold_key;
        ev->pressed = 1;
        ev->repeat = 1;
        if (--H.hold_left == 0)
            H.pending_release = H.hold_key;
        return 1;
    }
    if (H.pending_release) {
        ev->type = VX_EV_KEY;
        ev->key = H.pending_release;
        ev->pressed = 0;
        H.pending_release = VK_NONE;
        return 1;
    }
    for (;;) {
        t = next_token();
        if (!t || strcmp(t, "quit") == 0) {
            ev->type = VX_EV_QUIT;
            return 1;
        }
        if (strncmp(t, "shot:", 5) == 0) {
            write_ppm(t + 5);
            continue;
        }
        if (strncmp(t, "wait:", 5) == 0) {
            usleep((useconds_t)atoi(t + 5) * 1000);
            return 0; /* a timeout tick */
        }
        if (strncmp(t, "hold:", 5) == 0) {
            enum vx_key k = vx_key_from_name(t + 5);
            if (k == VK_NONE)
                continue;
            ev->type = VX_EV_KEY;
            ev->key = k;
            ev->pressed = 1;
            H.hold_key = k;
            H.hold_left = 30; /* ~1.5 s of auto-repeat at 50 ms */
            return 1;
        }
        ev->key = vx_key_from_name(t);
        if (ev->key == VK_NONE) {
            fprintf(stderr, "headless: unknown token '%s'\n", t);
            continue;
        }
        ev->type = VX_EV_KEY;
        ev->pressed = 1;
        H.pending_release = ev->key;
        return 1;
    }
}

void plat_set_blank(int blank) { (void)blank; }
void plat_release_display(void) {}
void plat_resume_display(void) {}

void plat_shutdown(void)
{
    free(H.surf.pixels);
    free(H.script);
}
