#include "apps.h"
#include "json.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define STBI_ONLY_PNG
#include "../third_party/stb/stb_image.h"

#define ICON_CACHE 24

static uint32_t tile_colour(const char *s)
{
    static const uint32_t pal[] = {
        /* soft tints that sit well on the navy wallpaper */
        0xff8fc4f5, 0xfff2a7a7, 0xff9ddcb6, 0xfff5cf95,
        0xffc4b2f0, 0xff8fdad6, 0xffeeaecb, 0xffaebcf5,
    };
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return pal[h % VX_ARRAY_LEN(pal)];
}

static void add_builtin(struct vx_apps *a, enum app_kind k, const char *id, const char *name,
                        uint32_t colour)
{
    struct vx_app *app = &a->list[a->count++];
    memset(app, 0, sizeof(*app));
    app->kind = k;
    vx_strlcpy(app->id, id, sizeof(app->id));
    vx_strlcpy(app->name, name, sizeof(app->name));
    app->color = colour;
}

/* Choose the icon closest to (but preferably not below) 56 px, the size
 * KaiOS shows in its launcher. */
static void pick_icon(const struct json *icons, const char *dir, char *out, size_t outlen)
{
    const struct json *best = NULL;
    long best_size = 0;
    if (!icons || icons->type != JSON_OBJ)
        return;
    for (const struct json *c = icons->child; c; c = c->next) {
        long sz = strtol(c->key, NULL, 10);
        if (c->type != JSON_STR)
            continue;
        if (!best || (best_size < 56 && sz > best_size) || (sz >= 56 && sz < best_size)) {
            best = c;
            best_size = sz;
        }
    }
    if (best && strncmp(best->str, "http", 4) != 0)
        snprintf(out, outlen, "%s/%s", dir, best->str[0] == '/' ? best->str + 1 : best->str);
}

static int load_manifest(struct vx_app *app, const char *apps_dir, const char *id)
{
    char dir[256], path[300];
    size_t len;
    char *text;
    struct json *m;
    const char *name;

    snprintf(dir, sizeof(dir), "%s/%s", apps_dir, id);
    snprintf(path, sizeof(path), "%s/manifest.webapp", dir);
    text = vx_read_file(path, &len, 256 * 1024);
    if (!text)
        return -1;
    m = json_parse(text, len);
    free(text);
    if (!m)
        return -1;

    memset(app, 0, sizeof(*app));
    app->kind = APP_KAIOS;
    vx_strlcpy(app->id, id, sizeof(app->id));
    name = json_get_str(m, "name", NULL);
    if (!name) {
        /* Some apps only carry localised names. */
        const struct json *loc = json_get(m, "locales");
        const struct json *first = loc && loc->type == JSON_OBJ ? loc->child : NULL;
        const struct json *en = json_get(loc, "en-US");
        name = json_get_str(en ? en : first, "name", id);
    }
    vx_strlcpy(app->name, name, sizeof(app->name));
    pick_icon(json_get(m, "icons"), dir, app->icon_path, sizeof(app->icon_path));
    app->color = tile_colour(app->name);
    json_free(m);
    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    const struct vx_app *x = a, *y = b;
    return strcasecmp(x->name, y->name);
}

void apps_scan(struct vx_apps *a, const char *apps_dir)
{
    DIR *d;
    struct dirent *e;
    int first_kai;

    apps_free(a);
    a->count = 0;
    add_builtin(a, APP_BUILTIN_STORE, "vortex.store", "Banana Store", 0xfff7dc7a);
    add_builtin(a, APP_BUILTIN_DIALER, "vortex.dialer", "Phone", 0xffb4dcb9);
    add_builtin(a, APP_BUILTIN_SETTINGS, "vortex.settings", "Settings", 0xffc9d4e0);
    add_builtin(a, APP_BUILTIN_SYSINFO, "vortex.sysinfo", "System", 0xff9fb3c9);
    first_kai = a->count;

    d = opendir(apps_dir);
    if (!d)
        return;
    while ((e = readdir(d)) && a->count < APPS_MAX) {
        if (e->d_name[0] == '.')
            continue;
        if (load_manifest(&a->list[a->count], apps_dir, e->d_name) == 0)
            a->count++;
    }
    closedir(d);
    qsort(&a->list[first_kai], (size_t)(a->count - first_kai), sizeof(struct vx_app), cmp_name);
}

/* Box-filter downscale of an RGBA buffer into an ARGB vx_image. */
static struct vx_image *scale_icon(const unsigned char *rgba, int w, int h, int size)
{
    struct vx_image *im = gfx_image_new(size, size);
    if (!im)
        return NULL;
    for (int y = 0; y < size; y++) {
        int sy0 = y * h / size, sy1 = (y + 1) * h / size;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < size; x++) {
            int sx0 = x * w / size, sx1 = (x + 1) * w / size;
            unsigned r = 0, g = 0, b = 0, al = 0, n = 0;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (int yy = sy0; yy < sy1; yy++)
                for (int xx = sx0; xx < sx1; xx++) {
                    const unsigned char *p = rgba + ((size_t)yy * w + xx) * 4;
                    /* weight colour by alpha so transparent edges stay clean */
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    al += p[3];
                    n++;
                }
            im->px[y * size + x] = al ? ((al / n) << 24) | ((r / al) << 16) | ((g / al) << 8) | (b / al) : 0;
        }
    }
    return im;
}

struct vx_image *apps_icon(struct vx_apps *a, int idx, int size)
{
    static unsigned stamp;
    struct vx_app *app = &a->list[idx];
    int cached = 0, lru = -1;
    size_t len;
    char *data;
    unsigned char *rgba;
    int w, h, ch;

    if (app->icon) {
        app->icon_used = ++stamp;
        return app->icon;
    }
    if (app->icon_failed || !app->icon_path[0])
        return NULL;

    for (int i = 0; i < a->count; i++) {
        if (!a->list[i].icon)
            continue;
        cached++;
        if (lru < 0 || a->list[i].icon_used < a->list[lru].icon_used)
            lru = i;
    }
    if (cached >= ICON_CACHE && lru >= 0) {
        free(a->list[lru].icon);
        a->list[lru].icon = NULL;
    }

    data = vx_read_file(app->icon_path, &len, 512 * 1024);
    if (!data) {
        app->icon_failed = 1;
        return NULL;
    }
    rgba = stbi_load_from_memory((const stbi_uc *)data, (int)len, &w, &h, &ch, 4);
    free(data);
    if (!rgba) {
        app->icon_failed = 1;
        return NULL;
    }
    app->icon = scale_icon(rgba, w, h, size);
    stbi_image_free(rgba);
    if (!app->icon)
        app->icon_failed = 1;
    app->icon_used = ++stamp;
    return app->icon;
}

void apps_drop_icons(struct vx_apps *a)
{
    for (int i = 0; i < a->count; i++) {
        free(a->list[i].icon);
        a->list[i].icon = NULL;
    }
}

void apps_free(struct vx_apps *a)
{
    apps_drop_icons(a);
}
