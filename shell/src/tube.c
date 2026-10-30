#include "tube.h"
#include "json.h"
#include "proc.h"
#include "vx.h"

#include "../third_party/stb/stb_image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* ---- files ------------------------------------------------------------ */

static void path_in(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s/%s", dir, name);
}

/* "id<TAB>title<TAB>author<TAB>authorId<TAB>secs<TAB>views<TAB>published" */
static int parse_video(char *line, struct tube_video *v)
{
    char *f[7];
    int n = 0;
    line[strcspn(line, "\r\n")] = '\0';
    for (char *p = line; n < 7; n++) {
        f[n] = p;
        p = strchr(p, '\t');
        if (!p) {
            n++;
            break;
        }
        *p++ = '\0';
    }
    if (n < 4 || !f[0][0])
        return -1;
    memset(v, 0, sizeof(*v));
    vx_strlcpy(v->id, f[0], sizeof(v->id));
    vx_strlcpy(v->title, f[1], sizeof(v->title));
    vx_strlcpy(v->author, f[2], sizeof(v->author));
    vx_strlcpy(v->author_id, f[3], sizeof(v->author_id));
    if (n > 4) v->secs = atoi(f[4]);
    if (n > 5) v->views = atol(f[5]);
    if (n > 6) vx_strlcpy(v->published, f[6], sizeof(v->published));
    return 0;
}

static void video_line(const struct tube_video *v, char *out, size_t n)
{
    snprintf(out, n, "%s\t%s\t%s\t%s\t%d\t%ld\t%s\n", v->id, v->title, v->author, v->author_id,
             v->secs, v->views, v->published);
}

/* Read a video file into v[]; newest_first reverses it (history and saved
 * are appended to). */
static int load_videos(const char *path, struct tube_video *v, int max, int newest_first)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int n = 0, total = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        struct tube_video tmp;
        if (parse_video(line, &tmp) != 0)
            continue;
        if (newest_first) {
            /* keep the last `max`: shift down when full */
            if (n == max) {
                memmove(v, v + 1, sizeof(*v) * (size_t)(max - 1));
                n--;
            }
            v[n++] = tmp;
        } else if (n < max) {
            v[n++] = tmp;
        }
        total++;
    }
    fclose(f);
    if (newest_first)
        for (int i = 0; i < n / 2; i++) {
            struct tube_video tmp = v[i];
            v[i] = v[n - 1 - i];
            v[n - 1 - i] = tmp;
        }
    return n;
}

/* Rewrite `path` without the lines whose first field is `key`.  Returns how
 * many were dropped. */
static int drop_lines(const char *path, const char *key)
{
    char tmp[300], line[512];
    size_t kl = strlen(key);
    int dropped = 0;
    FILE *in = fopen(path, "r"), *out;
    if (!in)
        return 0;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        return 0;
    }
    while (fgets(line, sizeof(line), in)) {
        if (!strncmp(line, key, kl) && line[kl] == '\t') {
            dropped++;
            continue;
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    rename(tmp, path);
    return dropped;
}

static int has_line(const char *path, const char *key)
{
    char line[512];
    size_t kl = strlen(key);
    FILE *f = fopen(path, "r");
    int found = 0;
    if (!f || !key[0]) {
        if (f)
            fclose(f);
        return 0;
    }
    while (!found && fgets(line, sizeof(line), f))
        found = !strncmp(line, key, kl) && line[kl] == '\t';
    fclose(f);
    return found;
}

static void append_line(const char *path, const char *line)
{
    FILE *f = fopen(path, "a");
    if (f) {
        fputs(line, f);
        fclose(f);
    }
}

/* \t \n \\ escapes of the description */
static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 't' ? ' ' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

static void load_settings(struct tube *t)
{
    char path[300], line[160];
    FILE *f;
    vx_strlcpy(t->quality, "240", sizeof(t->quality));
    t->instance[0] = '\0';
    path_in(path, sizeof(path), t->dir, "settings");
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "quality=", 8) && line[8])
            vx_strlcpy(t->quality, line + 8, sizeof(t->quality));
        else if (!strncmp(line, "instance=", 9))
            vx_strlcpy(t->instance, line + 9, sizeof(t->instance));
    }
    fclose(f);
}

void tube_set(struct tube *t, const char *key, const char *value)
{
    char path[300];
    FILE *f;
    if (!strcmp(key, "quality"))
        vx_strlcpy(t->quality, value, sizeof(t->quality));
    else if (!strcmp(key, "instance"))
        vx_strlcpy(t->instance, value, sizeof(t->instance));
    path_in(path, sizeof(path), t->dir, "settings");
    if (!(f = fopen(path, "w")))
        return;
    fprintf(f, "instance=%s\nquality=%s\n", t->instance, t->quality);
    fclose(f);
}

static void load_searches(struct tube *t)
{
    char path[300], line[80];
    FILE *f;
    t->n_searches = 0;
    path_in(path, sizeof(path), t->dir, "searches.txt");
    if (!(f = fopen(path, "r")))
        return;
    while (t->n_searches < TUBE_SEARCHES && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0])
            vx_strlcpy(t->searches[t->n_searches++], line, sizeof(t->searches[0]));
    }
    fclose(f);
}

void tube_add_search(struct tube *t, const char *q)
{
    char path[300];
    FILE *f;
    int i, j;
    if (!q[0])
        return;
    /* move to the front, without duplicates */
    for (i = 0; i < t->n_searches && strcmp(t->searches[i], q); i++)
        ;
    if (i == t->n_searches && t->n_searches < TUBE_SEARCHES)
        t->n_searches++;
    if (i >= t->n_searches)
        i = t->n_searches - 1;
    for (j = i; j > 0; j--)
        memcpy(t->searches[j], t->searches[j - 1], sizeof(t->searches[0]));
    vx_strlcpy(t->searches[0], q, sizeof(t->searches[0]));
    path_in(path, sizeof(path), t->dir, "searches.txt");
    if (!(f = fopen(path, "w")))
        return;
    for (i = 0; i < t->n_searches; i++)
        fprintf(f, "%s\n", t->searches[i]);
    fclose(f);
}

void tube_clear_searches(struct tube *t)
{
    char path[300];
    path_in(path, sizeof(path), t->dir, "searches.txt");
    unlink(path);
    t->n_searches = 0;
}

void tube_clear_history(struct tube *t)
{
    char path[300];
    path_in(path, sizeof(path), t->dir, "history.tsv");
    unlink(path);
}

static void history_add(struct tube *t, const struct tube_video *v)
{
    char path[300], line[512];
    struct stat st;
    path_in(path, sizeof(path), t->dir, "history.tsv");
    drop_lines(path, v->id);
    video_line(v, line, sizeof(line));
    append_line(path, line);
    /* keep it from growing without end: the newest 200 */
    if (stat(path, &st) == 0 && st.st_size > 120 * 1024) {
        static struct tube_video keep[200];
        int n = load_videos(path, keep, 200, 1);
        FILE *f = fopen(path, "w");
        for (int i = n - 1; f && i >= 0; i--) {
            video_line(&keep[i], line, sizeof(line));
            fputs(line, f);
        }
        if (f)
            fclose(f);
    }
}

int tube_toggle_saved(struct tube *t, const struct tube_video *v)
{
    char path[300], line[512];
    path_in(path, sizeof(path), t->dir, "saved.tsv");
    if (drop_lines(path, v->id))
        return 0;
    video_line(v, line, sizeof(line));
    append_line(path, line);
    return 1;
}

int tube_toggle_sub(struct tube *t, const char *author_id, const char *author)
{
    char path[300], line[160];
    path_in(path, sizeof(path), t->dir, "subs.tsv");
    if (drop_lines(path, author_id))
        return 0;
    snprintf(line, sizeof(line), "%s\t%s\n", author_id, author);
    append_line(path, line);
    return 1;
}

int tube_count_subs(struct tube *t)
{
    char path[300], line[160];
    FILE *f;
    int n = 0;
    path_in(path, sizeof(path), t->dir, "subs.tsv");
    if (!(f = fopen(path, "r")))
        return 0;
    while (fgets(line, sizeof(line), f))
        n += line[0] != '\n';
    fclose(f);
    return n;
}

/* ---- set-up ----------------------------------------------------------- */

void tube_init(struct tube *t, const char *cmd, const char *dir, const char *cache)
{
    memset(t, 0, sizeof(*t));
    vx_strlcpy(t->cmd, cmd, sizeof(t->cmd));
    vx_strlcpy(t->dir, dir, sizeof(t->dir));
    vx_strlcpy(t->cache, cache, sizeof(t->cache));
    mkdir(dir, 0755);
    mkdir(cache, 0755);
    path_in(t->p.sock, sizeof(t->p.sock), cache, "mpv.sock");
    t->p.fd = -1;
    load_settings(t);
    load_searches(t);
}

/* ---- the helper --------------------------------------------------------- */

static void stop_fetch(struct tube *t)
{
    if (t->fetch_pid > 0)
        proc_kill_group(t->fetch_pid, 300);
    if (t->fetch == TF_VIDEO && t->ndet)
        t->det[t->ndet - 1].loading = 0;   /* keep what the list said */
    if ((t->fetch == TF_LIST || t->fetch == TF_MORE) && t->nlists)
        t->lists[t->nlists - 1].loading = 0;
    if (t->fetch == TF_SERVERS)
        t->servers_loading = 0;
    t->fetch_pid = 0;
    t->fetch = TF_NONE;
}

/* Stop what runs and name the output file; the caller puts t->out into
 * the helper's arguments and calls run_fetch(). */
static char *begin_fetch(struct tube *t, enum tube_fetch what)
{
    stop_fetch(t);
    path_in(t->out, sizeof(t->out), t->cache, what == TF_VIDEO ? "video.tsv" :
            what == TF_SERVERS ? "servers.tsv" : "list.tsv");
    unlink(t->out);
    return t->out;
}

static int run_fetch(struct tube *t, enum tube_fetch what, char *const argv[])
{
    t->fetch_pid = proc_spawn(argv);
    if (t->fetch_pid < 0) {
        t->fetch_pid = 0;
        vx_strlcpy(t->error, "Could not start vortex-tube", sizeof(t->error));
        if (what == TF_VIDEO && t->ndet)
            t->det[t->ndet - 1].loading = 0;
        if ((what == TF_LIST || what == TF_MORE) && t->nlists)
            t->lists[t->nlists - 1].loading = 0;
        return -1;
    }
    t->fetch = what;
    return 0;
}

static void fetch_thumbs(struct tube *t, const struct tube_video *v, int n)
{
    char *argv[48];
    int k = 0;
    if (t->thumbs_pid > 0)
        proc_kill_group(t->thumbs_pid, 200);
    t->thumbs_pid = 0;
    argv[k++] = t->cmd;
    argv[k++] = "thumbs";
    argv[k++] = "default";
    for (int i = 0; i < n && k < (int)VX_ARRAY_LEN(argv) - 1; i++)
        argv[k++] = (char *)v[i].id;
    argv[k] = NULL;
    if (k > 3) {
        t->thumbs_pid = proc_spawn(argv);
        if (t->thumbs_pid < 0)
            t->thumbs_pid = 0;
    }
}

int tube_busy(const struct tube *t)
{
    return t->fetch_pid > 0 || t->thumbs_pid > 0 || t->p.pid > 0;
}

/* ---- lists ------------------------------------------------------------ */

int tube_open_list(struct tube *t, enum tube_kind kind, const char *arg, const char *title)
{
    struct tube_list *l;
    char path[300];
    if (t->nlists >= TUBE_DEPTH)
        return -1;
    l = &t->lists[t->nlists++];
    memset(l, 0, sizeof(*l));
    l->kind = kind;
    vx_strlcpy(l->title, title, sizeof(l->title));
    vx_strlcpy(l->arg, arg ? arg : "", sizeof(l->arg));
    switch (kind) {
    case TK_HISTORY:
    case TK_SAVED:
        path_in(path, sizeof(path), t->dir, kind == TK_HISTORY ? "history.tsv" : "saved.tsv");
        l->n = load_videos(path, l->v, TUBE_MAX, 1);
        fetch_thumbs(t, l->v, l->n);
        return 0;
    case TK_RELATED: {
        const struct tube_detail *d = tube_detail(t);
        if (d) {
            l->n = d->n_rel;
            memcpy(l->v, d->rel, sizeof(d->rel[0]) * (size_t)d->n_rel);
        }
        fetch_thumbs(t, l->v, l->n);
        return 0;
    }
    case TK_SEARCH: {
        char *out = begin_fetch(t, TF_LIST);
        char *argv[] = { t->cmd, "search", l->arg, out, "1", NULL };
        l->page = 1;
        l->loading = 1;
        run_fetch(t, TF_LIST, argv);
        return 0;
    }
    case TK_CHANNEL: {
        char *out = begin_fetch(t, TF_LIST);
        char *argv[] = { t->cmd, "channel", l->arg, out, NULL };
        l->loading = 1;
        run_fetch(t, TF_LIST, argv);
        return 0;
    }
    default: {
        char *out = begin_fetch(t, TF_LIST);
        char *argv[] = { t->cmd, kind == TK_POPULAR ? "popular" : kind == TK_TRENDING ? "trending" : "feed",
                         out, NULL };
        l->loading = 1;
        run_fetch(t, TF_LIST, argv);
        return 0;
    }
    }
}

void tube_pop_list(struct tube *t)
{
    if (!t->nlists)
        return;
    if (t->fetch == TF_LIST || t->fetch == TF_MORE)
        stop_fetch(t);
    t->nlists--;
}

int tube_load_more(struct tube *t)
{
    struct tube_list *l = tube_list(t);
    char page[12], *out;
    if (!l || l->kind != TK_SEARCH || !l->more || t->fetch != TF_NONE || l->n >= TUBE_MAX)
        return -1;
    snprintf(page, sizeof(page), "%d", l->page + 1);
    out = begin_fetch(t, TF_MORE);
    {
        char *argv[] = { t->cmd, "search", l->arg, out, page, NULL };
        return run_fetch(t, TF_MORE, argv);
    }
}

/* ---- video pages ------------------------------------------------------- */

int tube_open_video(struct tube *t, const struct tube_video *v)
{
    struct tube_detail *d;
    char path[300], *out;
    if (t->ndet >= TUBE_DEPTH)
        return -1;
    d = &t->det[t->ndet++];
    memset(d, 0, sizeof(*d));
    d->v = *v;
    path_in(path, sizeof(path), t->dir, "saved.tsv");
    d->saved = has_line(path, v->id);
    path_in(path, sizeof(path), t->dir, "subs.tsv");
    d->subbed = has_line(path, v->author_id);
    d->loading = 1;
    out = begin_fetch(t, TF_VIDEO);
    {
        char *argv[] = { t->cmd, "video", d->v.id, out, NULL };
        run_fetch(t, TF_VIDEO, argv);
    }
    if (t->thumbs_pid <= 0)
        fetch_thumbs(t, v, 1);
    return 0;
}

void tube_pop_video(struct tube *t)
{
    struct tube_detail *d = tube_detail(t);
    if (!d)
        return;
    if (t->fetch == TF_VIDEO)
        stop_fetch(t);
    free(d->desc);
    d->desc = NULL;
    t->ndet--;
}

void tube_reset(struct tube *t)
{
    stop_fetch(t);
    while (t->ndet)
        tube_pop_video(t);
    t->nlists = 0;
}

/* ---- results ------------------------------------------------------------- */

/* The helper's file, or NULL with t->error set. */
static FILE *open_result(struct tube *t, int code)
{
    FILE *f = fopen(t->out, "r");
    char first[128];
    if (f && fgets(first, sizeof(first), f)) {
        if (first[0] == '!') {
            first[strcspn(first, "\r\n")] = '\0';
            vx_strlcpy(t->error, first + 1, sizeof(t->error));
            fclose(f);
            return NULL;
        }
        rewind(f);
        return f;
    }
    if (f) {
        if (code == 0) /* an empty, but good, answer */
            return f;
        fclose(f);
    }
    snprintf(t->error, sizeof(t->error), code == 0 ? "No answer" : "Download failed (%d)", code);
    return NULL;
}

static void fetch_done(struct tube *t, enum tube_fetch what, int code)
{
    FILE *f = open_result(t, code);
    char line[4096];
    struct tube_list *l = tube_list(t);
    struct tube_detail *d = tube_detail(t);

    if (what == TF_LIST || what == TF_MORE) {
        int first;
        if (!l)
            goto out;
        l->loading = 0;
        if (what == TF_LIST)
            l->n = 0;
        first = l->n;
        while (f && l->n < TUBE_MAX && fgets(line, sizeof(line), f)) {
            struct tube_video v;
            int dup = 0;
            if (parse_video(line, &v) != 0)
                continue;
            for (int i = 0; i < l->n && !dup; i++)  /* pages overlap a little */
                dup = !strcmp(l->v[i].id, v.id);
            if (!dup)
                l->v[l->n++] = v;
        }
        if (f && l->kind == TK_SEARCH) {
            l->page = what == TF_MORE ? l->page + 1 : 1;
            l->more = l->n > first && l->n < TUBE_MAX;
        }
        if (l->n > first)
            fetch_thumbs(t, l->v + first, l->n - first);
    } else if (what == TF_VIDEO) {
        if (!d)
            goto out;
        d->loading = 0;
        d->failed = !f;
        d->n_rel = 0;
        while (f && fgets(line, sizeof(line), f)) {
            if (line[0] == 'V' && line[1] == '\t') {
                /* V id title author authorId secs views likes published desc */
                char *fld[10], *p = line;
                int n = 0;
                line[strcspn(line, "\r\n")] = '\0';
                while (n < 10) {
                    fld[n++] = p;
                    if (!(p = strchr(p, '\t')))
                        break;
                    *p++ = '\0';
                }
                if (n < 10)
                    continue;
                if (fld[2][0]) vx_strlcpy(d->v.title, fld[2], sizeof(d->v.title));
                if (fld[3][0]) vx_strlcpy(d->v.author, fld[3], sizeof(d->v.author));
                if (fld[4][0]) vx_strlcpy(d->v.author_id, fld[4], sizeof(d->v.author_id));
                if (atoi(fld[5])) d->v.secs = atoi(fld[5]);
                if (atol(fld[6])) d->v.views = atol(fld[6]);
                d->likes = atol(fld[7]);
                if (fld[8][0]) vx_strlcpy(d->v.published, fld[8], sizeof(d->v.published));
                unescape(fld[9]);
                free(d->desc);
                d->desc = strdup(fld[9]);
            } else if (d->n_rel < TUBE_RELATED && parse_video(line, &d->rel[d->n_rel]) == 0) {
                d->n_rel++;
            }
        }
        if (d->v.author_id[0]) {
            char path[300];
            path_in(path, sizeof(path), t->dir, "subs.tsv");
            d->subbed = has_line(path, d->v.author_id);
        }
    } else if (what == TF_SERVERS) {
        t->servers_loading = 0;
        t->n_servers = 0;
        while (f && t->n_servers < TUBE_SERVERS && fgets(line, sizeof(line), f)) {
            struct tube_server *s = &t->servers[t->n_servers];
            char *tab = strchr(line, '\t');
            if (!tab)
                continue;
            *tab = '\0';
            vx_strlcpy(s->url, line, sizeof(s->url));
            s->ok = atoi(tab + 1);
            tab = strchr(tab + 1, '\t');
            s->ms = tab ? atoi(tab + 1) : 0;
            t->n_servers++;
        }
    }
out:
    if (f)
        fclose(f);
}

int tube_fetch_servers(struct tube *t)
{
    char *out = begin_fetch(t, TF_SERVERS);
    char *argv[] = { t->cmd, "instances", out, NULL };
    t->servers_loading = 1;
    if (run_fetch(t, TF_SERVERS, argv) < 0) {
        t->servers_loading = 0;
        return -1;
    }
    return 0;
}

int tube_poll(struct tube *t)
{
    int code, changed = 0;
    if (t->fetch_pid > 0 && proc_poll(t->fetch_pid, &code) != 0) {
        enum tube_fetch what = t->fetch;
        t->fetch_pid = 0;
        t->fetch = TF_NONE;
        fetch_done(t, what, code);
        changed = 1;
    }
    if (t->thumbs_pid > 0) {
        if (proc_poll(t->thumbs_pid, &code) != 0)
            t->thumbs_pid = 0;
        changed = 1;   /* thumbnails appear as they arrive */
    }
    return changed;
}

/* ---- thumbnails ------------------------------------------------------------ */

/* Crop the centre of the picture to the w:h shape (YouTube's 4:3 pictures
 * have black bars), then box-filter it down. */
static struct vx_image *thumb_scale(const unsigned char *rgb, int sw, int sh, int w, int h)
{
    struct vx_image *im = gfx_image_new(w, h);
    int cw = sw, ch = sh, cx, cy;
    if (!im)
        return NULL;
    if ((long)sw * h > (long)sh * w)
        cw = sh * w / h;
    else
        ch = sw * h / w;
    cx = (sw - cw) / 2;
    cy = (sh - ch) / 2;
    for (int y = 0; y < h; y++) {
        int y0 = cy + y * ch / h, y1 = cy + (y + 1) * ch / h;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < w; x++) {
            int x0 = cx + x * cw / w, x1 = cx + (x + 1) * cw / w;
            unsigned r = 0, g = 0, b = 0, n = 0;
            if (x1 <= x0) x1 = x0 + 1;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const unsigned char *p = rgb + ((size_t)yy * sw + xx) * 3;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    n++;
                }
            im->px[y * w + x] = 0xff000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    return im;
}

struct vx_image *tube_thumb(struct tube *t, const char *id, int big, int w, int h)
{
    static unsigned stamp;
    char path[400];
    size_t len;
    char *data;
    unsigned char *rgb;
    int sw, sh, ch, slot = -1;

    for (int i = 0; i < TUBE_THUMBS; i++) {
        if (t->thumbs[i].id[0] && !strcmp(t->thumbs[i].id, id) && t->thumbs[i].big == big &&
            t->thumbs[i].w == w && t->thumbs[i].h == h) {
            t->thumbs[i].used = ++stamp;
            return t->thumbs[i].img;
        }
        if (slot < 0 || !t->thumbs[i].id[0] ||
            (t->thumbs[slot].id[0] && t->thumbs[i].used < t->thumbs[slot].used))
            slot = i;
    }
    snprintf(path, sizeof(path), "%s/thumbs/%s/%s.jpg", t->cache, big ? "mqdefault" : "default", id);
    data = vx_read_file(path, &len, 256 * 1024);
    if (!data)
        return NULL;   /* not downloaded (yet): ask again later */
    rgb = stbi_load_from_memory((const unsigned char *)data, (int)len, &sw, &sh, &ch, 3);
    free(data);
    free(t->thumbs[slot].img);
    vx_strlcpy(t->thumbs[slot].id, id, sizeof(t->thumbs[slot].id));
    t->thumbs[slot].big = big;
    t->thumbs[slot].w = w;
    t->thumbs[slot].h = h;
    t->thumbs[slot].used = ++stamp;
    t->thumbs[slot].img = rgb ? thumb_scale(rgb, sw, sh, w, h) : NULL;
    stbi_image_free(rgb);
    return t->thumbs[slot].img;
}

void tube_drop_thumbs(struct tube *t)
{
    for (int i = 0; i < TUBE_THUMBS; i++) {
        free(t->thumbs[i].img);
        t->thumbs[i].img = NULL;
        t->thumbs[i].id[0] = '\0';
    }
}

/* ---- playback --------------------------------------------------------------- */

static void ipc_close(struct tube_player *p)
{
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
    p->len = 0;
}

static void ipc_send(struct tube_player *p, const char *s)
{
    size_t n = strlen(s);
    if (p->fd >= 0 && send(p->fd, s, n, MSG_NOSIGNAL) != (ssize_t)n)
        ipc_close(p);
}

static void ipc_connect(struct tube_player *p)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    int fd;
    vx_strlcpy(a.sun_path, p->sock, sizeof(a.sun_path));
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    p->fd = fd;
    ipc_send(p, "{\"command\":[\"observe_property\",1,\"time-pos\"]}\n"
                "{\"command\":[\"observe_property\",2,\"duration\"]}\n"
                "{\"command\":[\"observe_property\",3,\"pause\"]}\n"
                "{\"command\":[\"observe_property\",4,\"volume\"]}\n"
                "{\"command\":[\"observe_property\",5,\"paused-for-cache\"]}\n");
}

int tube_play(struct tube *t, const struct tube_video *v, int video, int start, long now)
{
    char from[16];
    char *argv[] = { t->cmd, "play", (char *)v->id, video ? "video" : "audio", t->p.sock, from, NULL };
    struct tube_player *p = &t->p;
    if (p->pid > 0)
        tube_player_stop(t);
    snprintf(from, sizeof(from), "%d", start > 0 ? start : 0);
    unlink(p->sock);
    p->pid = proc_spawn(argv);
    if (p->pid < 0) {
        p->pid = 0;
        return -1;
    }
    p->video = video;
    p->v = *v;
    p->pos = start;
    p->dur = v->secs;
    p->paused = p->buffering = 0;
    p->volume = 80;
    p->started = now;
    ipc_close(p);
    history_add(t, v);
    return 0;
}

void tube_player_cmd(struct tube *t, const char *cmd)
{
    char line[160];
    snprintf(line, sizeof(line), "%s\n", cmd);
    ipc_send(&t->p, line);
}

void tube_player_stop(struct tube *t)
{
    struct tube_player *p = &t->p;
    if (p->pid <= 0)
        return;
    tube_player_cmd(t, "quit");
    proc_kill_group(p->pid, 1200);
    p->pid = 0;
    ipc_close(p);
    unlink(p->sock);
}

static int ipc_event(struct tube_player *p, const char *line, size_t len)
{
    struct json *j = json_parse(line, len);
    const struct json *data;
    const char *name;
    int changed = 0;
    if (!j)
        return 0;
    name = json_get_str(j, "name", "");
    data = json_get(j, "data");
    if (!strcmp(json_get_str(j, "event", ""), "property-change") && data) {
        double v = data->type == JSON_NUM || data->type == JSON_BOOL ? data->num : 0;
        if (!strcmp(name, "time-pos")) {
            changed = (long)v != (long)p->pos;
            p->pos = v;
        } else if (!strcmp(name, "duration") && v > 0) {
            changed = (long)v != (long)p->dur;
            p->dur = v;
        } else if (!strcmp(name, "pause")) {
            changed = p->paused != (v != 0);
            p->paused = v != 0;
        } else if (!strcmp(name, "volume")) {
            changed = p->volume != (int)(v + 0.5);
            p->volume = (int)(v + 0.5);
        } else if (!strcmp(name, "paused-for-cache")) {
            changed = p->buffering != (v != 0);
            p->buffering = v != 0;
        }
    }
    json_free(j);
    return changed;
}

int tube_player_poll(struct tube *t, int *code)
{
    struct tube_player *p = &t->p;
    int changed = 0;
    if (p->pid <= 0)
        return 0;
    if (proc_poll(p->pid, code) != 0) {
        p->pid = 0;
        ipc_close(p);
        unlink(p->sock);
        return 2;
    }
    if (p->fd < 0)
        ipc_connect(p);
    while (p->fd >= 0) {
        ssize_t r = read(p->fd, p->buf + p->len, sizeof(p->buf) - 1 - (size_t)p->len);
        char *nl, *s;
        if (r <= 0) {
            if (r == 0 || (errno != EAGAIN && errno != EINTR))
                ipc_close(p);
            break;
        }
        p->len += (int)r;
        p->buf[p->len] = '\0';
        s = p->buf;
        while ((nl = strchr(s, '\n'))) {
            changed |= ipc_event(p, s, (size_t)(nl - s));
            s = nl + 1;
        }
        p->len -= (int)(s - p->buf);
        memmove(p->buf, s, (size_t)p->len);
        if (p->len >= (int)sizeof(p->buf) - 1)  /* one absurdly long line */
            p->len = 0;
    }
    return changed;
}

/* ---- formatting --------------------------------------------------------------- */

void tube_format_count(long n, char *out, size_t size)
{
    if (n >= 1000000000L)
        snprintf(out, size, "%.1fB", n / 1e9);
    else if (n >= 1000000L)
        snprintf(out, size, "%.1fM", n / 1e6);
    else if (n >= 1000L)
        snprintf(out, size, "%.1fK", n / 1e3);
    else
        snprintf(out, size, "%ld", n);
    /* 1.0M -> 1M */
    {
        char *dot = strstr(out, ".0");
        if (dot)
            memmove(dot, dot + 2, strlen(dot + 2) + 1);
    }
}

void tube_format_time(long s, char *out, size_t size)
{
    if (s < 0)
        s = 0;
    if (s >= 3600)
        snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
}
