/* Tube: the model behind the shell's native Invidious client.
 *
 * The network side is the vortex-tube helper (runtime/vortex-tube): the
 * shell runs it in the background, it writes plain tab-separated files and
 * exits.  Playback is mpv, started by the same helper; the shell steers it
 * through mpv's JSON IPC socket.
 *
 * Screens nest (list -> video -> related list -> video ...), so lists and
 * video pages are small stacks that follow the shell's screen stack.
 * Everything is fixed-size except the description text and the decoded
 * thumbnails. */
#ifndef VX_TUBE_H
#define VX_TUBE_H

#include "gfx.h"

#include <sys/types.h>

#define TUBE_MAX 100       /* videos in one list */
#define TUBE_DEPTH 3       /* nested lists / video pages */
#define TUBE_RELATED 30
#define TUBE_SERVERS 20
#define TUBE_SEARCHES 15
#define TUBE_THUMBS 14     /* decoded thumbnails kept */

struct tube_video {
    char id[16];
    char title[112];
    char author[48];
    char author_id[32];
    int secs;
    long views;
    char published[28];
};

enum tube_kind { TK_SEARCH, TK_POPULAR, TK_TRENDING, TK_FEED, TK_CHANNEL, TK_RELATED,
                 TK_HISTORY, TK_SAVED };

struct tube_list {
    enum tube_kind kind;
    char title[64];
    char arg[96];          /* the query, or the channel id */
    int page;              /* search: the last page loaded */
    int more;              /* search: another page may follow */
    int loading;
    struct tube_video v[TUBE_MAX];
    int n, sel, top;
};

struct tube_detail {
    struct tube_video v;
    long likes;
    char *desc;            /* heap */
    int loading, failed;
    int saved, subbed;
    int sel, top;
    struct tube_video rel[TUBE_RELATED];
    int n_rel;
};

struct tube_server {
    char url[96];
    int ok, ms;
};

enum tube_fetch { TF_NONE, TF_LIST, TF_MORE, TF_VIDEO, TF_SERVERS };

struct tube_player {
    pid_t pid;
    int video;             /* 1 = mpv owns the display */
    int fd;                /* IPC socket, -1 until mpv is up */
    char sock[280];
    struct tube_video v;
    double pos, dur;
    int paused, buffering, volume;
    long started;          /* ms */
    char buf[4096];
    int len;
};

struct tube {
    char cmd[256], dir[256], cache[256];
    char quality[8], instance[96];

    struct tube_list lists[TUBE_DEPTH];
    int nlists;
    struct tube_detail det[TUBE_DEPTH];
    int ndet;

    pid_t fetch_pid;
    enum tube_fetch fetch;
    char out[300];
    pid_t thumbs_pid;
    char error[96];        /* last failure, for a toast; cleared by the UI */

    struct tube_server servers[TUBE_SERVERS];
    int n_servers, servers_loading;

    char searches[TUBE_SEARCHES][64];   /* newest first */
    int n_searches;

    struct tube_player p;

    /* img == NULL with an id: the file did not decode */
    struct { char id[16]; int big, w, h; struct vx_image *img; unsigned used; } thumbs[TUBE_THUMBS];
};

void tube_init(struct tube *t, const char *cmd, const char *dir, const char *cache);

/* Lists.  Returns -1 when the stack is full. */
int  tube_open_list(struct tube *t, enum tube_kind kind, const char *arg, const char *title);
void tube_pop_list(struct tube *t);
int  tube_load_more(struct tube *t);
static inline struct tube_list *tube_list(struct tube *t) { return t->nlists ? &t->lists[t->nlists - 1] : 0; }

/* Video pages. */
int  tube_open_video(struct tube *t, const struct tube_video *v);
void tube_pop_video(struct tube *t);
static inline struct tube_detail *tube_detail(struct tube *t) { return t->ndet ? &t->det[t->ndet - 1] : 0; }
void tube_reset(struct tube *t);   /* left the app: drop the stacks */

/* Background work; returns 1 when something on screen changed. */
int  tube_poll(struct tube *t);
int  tube_busy(const struct tube *t);

/* Thumbnail of `id` scaled (cropped to fill) to w x h, or NULL while it is
 * not downloaded.  big = the 320x180 picture rather than 120x90. */
struct vx_image *tube_thumb(struct tube *t, const char *id, int big, int w, int h);
void tube_drop_thumbs(struct tube *t);

/* Local lists and settings. */
int  tube_toggle_saved(struct tube *t, const struct tube_video *v);   /* 1 = now saved */
int  tube_toggle_sub(struct tube *t, const char *author_id, const char *author);   /* 1 = now subscribed */
int  tube_count_subs(struct tube *t);
void tube_add_search(struct tube *t, const char *q);
void tube_clear_history(struct tube *t);
void tube_clear_searches(struct tube *t);
void tube_set(struct tube *t, const char *key, const char *value);
int  tube_fetch_servers(struct tube *t);

/* Playback: video = mpv takes the display; otherwise audio in the
 * background.  start = seconds. */
int  tube_play(struct tube *t, const struct tube_video *v, int video, int start, long now);
/* Text command for mpv (input.conf syntax), e.g. "cycle pause". */
void tube_player_cmd(struct tube *t, const char *cmd);
void tube_player_stop(struct tube *t);
/* Returns 0 = nothing new, 1 = state changed, 2 = mpv exited (*code set). */
int  tube_player_poll(struct tube *t, int *code);

void tube_format_count(long n, char *out, size_t size);    /* 1.2M */
void tube_format_time(long secs, char *out, size_t size);  /* 1:02:03 */

#endif
