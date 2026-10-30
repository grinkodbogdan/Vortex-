/* Installed apps: built-in shell screens plus KaiOS apps found under
 * apps_dir/<id>/manifest.webapp. */
#ifndef VX_APPS_H
#define VX_APPS_H

#include "gfx.h"

#define APPS_MAX 128

enum app_kind { APP_KAIOS = 0, APP_BUILTIN_STORE, APP_BUILTIN_SETTINGS, APP_BUILTIN_SYSINFO,
                APP_BUILTIN_DIALER, APP_BUILTIN_CALC, APP_BUILTIN_MESSAGES, APP_BUILTIN_TORCH };

/* Built-in apps come first in the list, in this order. */
#define APPS_BUILTIN 7

struct vx_app {
    enum app_kind kind;
    char id[64];
    char name[48];
    char icon_path[192];
    uint32_t color;           /* placeholder tile colour */
    struct vx_image *icon;    /* decoded lazily, may be evicted */
    unsigned icon_used;       /* LRU stamp */
    int icon_failed;
};

struct vx_apps {
    struct vx_app list[APPS_MAX];
    int count;
};

void apps_scan(struct vx_apps *a, const char *apps_dir);
/* Returns the icon, decoding it on first use.  At most ICON_CACHE icons are
 * kept in memory; the least recently used is dropped. */
struct vx_image *apps_icon(struct vx_apps *a, int idx, int size);
void apps_drop_icons(struct vx_apps *a);
void apps_free(struct vx_apps *a);

#endif
