#include "store.h"
#include "json.h"
#include "proc.h"
#include "vx.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

void store_init(struct store *s, const char *cache_dir, const char *url)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->cache_path, sizeof(s->cache_path), "%s/store.json", cache_dir);
    snprintf(s->tmp_path, sizeof(s->tmp_path), "%s.part", s->cache_path);
    vx_strlcpy(s->url, url, sizeof(s->url));
    mkdir(cache_dir, 0755);
}

static char *dup_or_empty(const char *v)
{
    return strdup(v ? v : "");
}

static void free_records(struct store *s)
{
    for (int i = 0; i < s->n_apps; i++) {
        struct store_app *a = &s->apps[i];
        free(a->slug); free(a->name); free(a->description); free(a->author);
        free(a->type); free(a->license); free(a->url); free(a->git);
    }
    free(s->apps);
    s->apps = NULL;
    s->n_apps = 0;
    for (int i = 0; i < s->n_cats; i++) {
        free(s->cats[i].key);
        free(s->cats[i].name);
    }
    s->n_cats = 0;
}

static int cat_index(const struct store *s, const char *key)
{
    for (int i = 0; i < s->n_cats; i++)
        if (strcmp(s->cats[i].key, key) == 0)
            return i;
    return -1;
}

/* Strip "<email>" from "Name <email>" author strings. */
static char *author_name(const struct json *authors)
{
    const char *a = NULL;
    char *out, *lt;
    if (authors && authors->type == JSON_ARR && authors->child)
        a = json_str(authors->child, NULL);
    else
        a = json_str(authors, NULL);
    out = dup_or_empty(a);
    if (out && (lt = strchr(out, '<'))) {
        *lt = '\0';
        for (char *e = lt; e > out && isspace((unsigned char)e[-1]); )
            *--e = '\0';
    }
    return out;
}

int store_parse(struct store *s, const char *text, size_t len)
{
    struct json *root = json_parse(text, len);
    const struct json *apps, *cats, *gen;
    int n = 0;

    if (!root) {
        vx_strlcpy(s->error, "Store data is not valid JSON", sizeof(s->error));
        return -1;
    }
    apps = json_get(root, "apps");
    if (!apps || apps->type != JSON_ARR) {
        json_free(root);
        vx_strlcpy(s->error, "Store data has no app list", sizeof(s->error));
        return -1;
    }
    free_records(s);

    cats = json_get(root, "categories");
    if (cats && cats->type == JSON_OBJ)
        for (const struct json *c = cats->child; c && s->n_cats < STORE_MAX_CATS; c = c->next) {
            s->cats[s->n_cats].key = strdup(c->key);
            s->cats[s->n_cats].name = dup_or_empty(json_get_str(c, "name", c->key));
            s->n_cats++;
        }
    gen = json_get(root, "generated_at");
    /* store-db writes milliseconds */
    s->generated_at = gen && gen->type == JSON_NUM ? (long)(gen->num / 1000) : 0;

    for (const struct json *c = apps->child; c; c = c->next)
        n++;
    s->apps = calloc((size_t)(n ? n : 1), sizeof(*s->apps));
    if (!s->apps) {
        json_free(root);
        return -1;
    }
    for (const struct json *c = apps->child; c; c = c->next) {
        struct store_app *a = &s->apps[s->n_apps];
        const struct json *dl = json_get(c, "download");
        const struct json *meta = json_get(c, "meta");
        const struct json *ac = json_get(meta, "categories");
        const char *url = json_get_str(dl, "url", NULL);
        if (!url || !json_get_str(c, "slug", NULL))
            continue; /* not installable */
        a->slug = dup_or_empty(json_get_str(c, "slug", NULL));
        a->name = dup_or_empty(json_get_str(c, "name", a->slug));
        a->description = dup_or_empty(json_get_str(c, "description", ""));
        a->author = author_name(json_get(c, "author"));
        a->type = dup_or_empty(json_get_str(c, "type", "packaged"));
        a->license = dup_or_empty(json_get_str(c, "license", "?"));
        a->url = dup_or_empty(url);
        a->git = dup_or_empty(json_get_str(c, "git_repo", ""));
        a->has_ads = (unsigned char)json_truthy(json_get(c, "has_ads"));
        a->has_tracking = (unsigned char)json_truthy(json_get(c, "has_tracking"));
        if (ac && ac->type == JSON_ARR)
            for (const struct json *k = ac->child; k; k = k->next) {
                int ci = cat_index(s, json_str(k, ""));
                if (ci >= 0 && ci < 32)
                    a->cats |= 1u << ci;
            }
        s->n_apps++;
    }
    json_free(root);
    s->state = STORE_READY;
    s->error[0] = '\0';
    return 0;
}

int store_load_cache(struct store *s)
{
    size_t len;
    char *text = vx_read_file(s->cache_path, &len, 8u << 20);
    int r;
    if (!text)
        return -1;
    r = store_parse(s, text, len);
    free(text);
    return r;
}

int store_refresh(struct store *s)
{
    char *argv[] = { "curl", "-fsSL", "--max-time", "120", "-o", s->tmp_path, s->url, NULL };
    if (s->fetch_pid > 0)
        return 0;
    s->fetch_pid = proc_spawn(argv);
    if (s->fetch_pid < 0) {
        s->fetch_pid = 0;
        vx_strlcpy(s->error, "Could not start curl", sizeof(s->error));
        s->state = s->n_apps ? STORE_READY : STORE_ERROR;
        return -1;
    }
    if (!s->n_apps)
        s->state = STORE_FETCHING;
    return 0;
}

int store_poll(struct store *s)
{
    int code;
    if (s->fetch_pid <= 0 || proc_poll(s->fetch_pid, &code) == 0)
        return 0;
    s->fetch_pid = 0;
    if (code != 0) {
        snprintf(s->error, sizeof(s->error), "Download failed (curl %d). Offline?", code);
        unlink(s->tmp_path);
        if (!s->n_apps)
            s->state = STORE_ERROR;
        return 1;
    }
    if (rename(s->tmp_path, s->cache_path) != 0 || store_load_cache(s) != 0) {
        if (!s->error[0])
            vx_strlcpy(s->error, "Could not read store data", sizeof(s->error));
        if (!s->n_apps)
            s->state = STORE_ERROR;
    }
    return 1;
}

void store_unload(struct store *s)
{
    free_records(s);
    if (s->state == STORE_READY)
        s->state = STORE_EMPTY;
}

static int icontains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return 1;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0)
            return 1;
    return 0;
}

int store_filter(const struct store *s, int cat, const char *query, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < s->n_apps && n < max; i++) {
        const struct store_app *a = &s->apps[i];
        if (cat >= 0 && !(a->cats & (1u << cat)))
            continue;
        if (query && *query && !icontains(a->name, query))
            continue;
        out[n++] = i;
    }
    return n;
}
