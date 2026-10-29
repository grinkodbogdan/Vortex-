/* Native client for the BananaHackers store (store-db data.json, v3).
 *
 * The database is downloaded with curl into cache_dir/store.json and parsed
 * into compact records; the JSON text and DOM are freed straight away.
 * Everything here is dropped with store_unload() while a web app runs. */
#ifndef VX_STORE_H
#define VX_STORE_H

#include <sys/types.h>

#define STORE_MAX_CATS 24

struct store_app {
    char *slug, *name, *description, *author, *type, *license, *url, *git;
    unsigned cats;          /* bitmask into store.cats */
    unsigned char has_ads, has_tracking;
};

struct store_cat {
    char *key, *name;
};

enum store_state { STORE_EMPTY, STORE_FETCHING, STORE_READY, STORE_ERROR };

struct store {
    enum store_state state;
    char error[96];
    struct store_app *apps;
    int n_apps;
    struct store_cat cats[STORE_MAX_CATS];
    int n_cats;
    long generated_at;      /* seconds since epoch */
    pid_t fetch_pid;
    char cache_path[256], tmp_path[264];
    char url[256];
};

void store_init(struct store *s, const char *cache_dir, const char *url);
/* Load the cached copy if present (no network). */
int  store_load_cache(struct store *s);
/* Start a background refresh. */
int  store_refresh(struct store *s);
/* Call periodically while fetching; returns 1 when the state changed. */
int  store_poll(struct store *s);
void store_unload(struct store *s);

/* Index list of apps in category `cat` (-1 = all) whose name contains
 * `query` (case-insensitive, may be NULL). Returns the count. */
int  store_filter(const struct store *s, int cat, const char *query, int *out, int max);
int  store_parse(struct store *s, const char *text, size_t len);

#endif
