/* Vortex shell screens.
 *
 * Feature phones have no pointer, so focus is managed here rather than by
 * Nuklear's mouse-driven widgets: every screen keeps its own selection
 * index and draws itself onto the Nuklear window canvas.  Nuklear supplies
 * the context, fonts, text measurement, clipping and the command list that
 * gfx.c rasterises.
 */
#include "ui.h"
#include "proc.h"

#include <dirent.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SB_H  20   /* status bar */
#define HDR_H 30   /* screen title */
#define SK_H  28   /* soft key bar */
#define ROW_H 30
#define ROW2_H 42

/* "Steel night" theme: a navy-to-steel gradient wallpaper under
 * translucent dark panels, azure selection bars and letter-spaced cyan
 * section titles -- the look of the Vortex desktop, shrunk to 240x320. */
#define C(rgb) nk_rgb(((rgb) >> 16) & 0xff, ((rgb) >> 8) & 0xff, (rgb) & 0xff)
static const struct {
    uint32_t bg, panel, status, accent, accent_dim, text, dim, warn, ok, sep, ink, ink_dim, cyan, border;
} T = {
    .bg = 0x0a1120, .panel = 0x0c1420, .status = 0x04070c,
    .accent = 0x2f8fe8,     /* azure: selection, focus, progress */
    .accent_dim = 0x1b4f80,
    .text = 0xeef3f8, .dim = 0x8d9bb0,
    .warn = 0xffb35c, .ok = 0x5fd59a, .sep = 0x1c2838,
    .ink = 0xffffff,        /* text on the azure selection */
    .ink_dim = 0xcfe4f8,
    .cyan = 0x4cc3ff,       /* section titles and labels */
    .border = 0x2c3d52,
};

static struct nk_command_buffer *cv;

/* defined further down */
static void messages_load(void);
static void torch_toggle(void);
static void tube_screen_closed(enum screen s);
static void tube_stop(void);
static int move(int *sel, int n, int delta);
static struct shell *S;

/* ---- drawing helpers ------------------------------------------------- */

static float text_w(const struct nk_font *f, const char *s, int len)
{
    return f->handle.width(f->handle.userdata, f->handle.height, s, len);
}

/* Fit `s` into `maxw` pixels, cutting at a UTF-8 boundary and adding "…". */
static const char *ellipsize(const struct nk_font *f, const char *s, float maxw, int *len_out)
{
    static char buf[256];
    int len = (int)strlen(s), cut;
    float ew;
    if (text_w(f, s, len) <= maxw) {
        *len_out = len;
        return s;
    }
    ew = text_w(f, "\xe2\x80\xa6", 3);
    cut = len < (int)sizeof(buf) - 4 ? len : (int)sizeof(buf) - 4;
    while (cut > 0) {
        while (cut > 0 && ((unsigned char)s[cut] & 0xc0) == 0x80)
            cut--;
        if (text_w(f, s, cut) + ew <= maxw)
            break;
        cut--;
    }
    memcpy(buf, s, (size_t)cut);
    memcpy(buf + cut, "\xe2\x80\xa6", 4);
    *len_out = cut + 3;
    return buf;
}

enum { AL_LEFT, AL_CENTER, AL_RIGHT };

static void label(struct nk_rect r, const char *s, const struct nk_font *f, uint32_t col, int align)
{
    int len;
    const char *t = ellipsize(f, s, r.w, &len);
    float w = text_w(f, t, len);
    float x = align == AL_LEFT ? r.x : align == AL_CENTER ? r.x + (r.w - w) / 2 : r.x + r.w - w;
    float y = r.y + (r.h - f->handle.height) / 2;
    nk_draw_text(cv, nk_rect(x, y, w + 1, f->handle.height), t, len, &f->handle,
                 nk_rgba(0, 0, 0, 0), C(col));
}

static void fill(struct nk_rect r, float round, uint32_t col)
{
    nk_fill_rect(cv, r, round, C(col));
}

/* Vertical gradient (Nuklear corner order: TL, TR, BR, BL). */
static void vgrad(struct nk_rect r, uint32_t top, uint32_t bottom)
{
    nk_fill_rect_multi_color(cv, r, C(top), C(top), C(bottom), C(bottom));
}

static void fill_a(struct nk_rect r, float round, uint32_t rgb, int alpha)
{
    nk_fill_rect(cv, r, round, nk_rgba((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, alpha));
}

/* Full-screen wallpaper: deep navy at the top fading to steel blue. */
static void wallpaper(void)
{
    float w = (float)S->surf->w, h = (float)S->surf->h;
    vgrad(nk_rect(0, 0, w, h * 0.55f), 0x070c17, 0x1a2a40);
    vgrad(nk_rect(0, h * 0.55f, w, h * 0.45f + 1), 0x1a2a40, 0x47607c);
}

/* Translucent dark glass, like the desktop's windows. */
static void glass(struct nk_rect r, float round)
{
    fill_a(r, round, T.panel, 205);
    nk_stroke_rect(cv, r, round, 1, C(T.border));
}

/* Azure selection bar with a slight top sheen. */
static void highlight(struct nk_rect r)
{
    vgrad(r, 0x3d9df2, 0x2582da);
    fill_a(nk_rect(r.x, r.y, r.w, 1), 0, 0xffffff, 70);
}

/* Letter-spaced capitals in cyan: "SYSTEM INFO". */
static void caps(float x, float y, const char *s)
{
    const struct nk_font *f = S->fonts.small;
    char up[2] = { 0, 0 };
    for (; *s; s++) {
        up[0] = (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
        nk_draw_text(cv, nk_rect(x, y, 20, f->handle.height), up, 1, &f->handle, nk_rgba(0, 0, 0, 0), C(T.cyan));
        x += text_w(f, up, 1) + 2.5f;
    }
}

/* Small arrow head pointing '^', 'v', '<' or '>', centred on (cx, cy). */
static void arrow(float cx, float cy, char dir, uint32_t col)
{
    const float a = 4.5f, b = 3.f;
    switch (dir) {
    case '^': nk_fill_triangle(cv, cx - a, cy + b, cx + a, cy + b, cx, cy - b, C(col)); break;
    case 'v': nk_fill_triangle(cv, cx - a, cy - b, cx + a, cy - b, cx, cy + b, C(col)); break;
    case '<': nk_fill_triangle(cv, cx + b, cy - a, cx + b, cy + a, cx - b, cy, C(col)); break;
    default: nk_fill_triangle(cv, cx - b, cy - a, cx - b, cy + a, cx + b, cy, C(col)); break;
    }
}

/* Thin progress bar. */
static void meter(struct nk_rect r, int pct, uint32_t col)
{
    fill_a(r, r.h / 2, 0xffffff, 28);
    if (pct > 0)
        fill(nk_rect(r.x, r.y, r.w * (pct > 100 ? 100 : pct) / 100.f, r.h), r.h / 2, col);
}

static struct nk_rect content_rect(int with_header)
{
    float top = SB_H + (with_header ? HDR_H : 0);
    return nk_rect(0, top, (float)S->surf->w, S->surf->h - top - SK_H);
}

/* Calls fn for every wrapped line of `s` that fits `maxw`. */
static int wrap(const struct nk_font *f, const char *s, float maxw,
                void (*fn)(const char *line, int len, int idx, void *u), void *u)
{
    int n = 0;
    while (*s) {
        int end = 0, last_space = -1;
        while (s[end] && s[end] != '\n') {
            int nxt = end + 1;
            while (((unsigned char)s[nxt] & 0xc0) == 0x80)
                nxt++;
            if (text_w(f, s, nxt) > maxw)
                break;
            if (s[end] == ' ')
                last_space = end;
            end = nxt;
        }
        if (s[end] && s[end] != '\n' && last_space > 0)
            end = last_space;
        if (end == 0 && s[0] != '\n') { /* one glyph wider than the line */
            end = 1;
            while (((unsigned char)s[end] & 0xc0) == 0x80)
                end++;
        }
        if (fn)
            fn(s, end, n, u);
        n++;
        s += end;
        if (*s == ' ' || *s == '\n')
            s++;
    }
    return n;
}

static void status_bar(void)
{
    char clock[8];
    time_t t = time(NULL);
    struct tm tm;
    float w = (float)S->surf->w;
    const struct nk_font *f = S->fonts.small;

    localtime_r(&t, &tm);
    strftime(clock, sizeof(clock), "%H:%M", &tm);
    fill_a(nk_rect(0, 0, w, SB_H), 0, T.status, 225);
    fill(nk_rect(0, SB_H - 1, w, 1), 0, T.sep);

    /* mobile signal bars when there is a modem, Wi-Fi otherwise */
    if (S->modem_seen && strcmp(S->modem_state, "none")) {
        int bars = S->modem_signal >= 70 ? 4 : S->modem_signal >= 45 ? 3 : S->modem_signal >= 20 ? 2 :
                   S->modem_signal > 0 ? 1 : 0;
        for (int i = 0; i < 4; i++)
            fill(nk_rect(5 + i * 4, 14 - i * 3, 3, 3 + i * 3), 0, i < bars ? T.cyan : 0x2a3444);
    } else {
        for (int i = 0; i < 4; i++)
            fill(nk_rect(5 + i * 4, 14 - i * 3, 3, 3 + i * 3), 0, S->sys.net_up ? T.cyan : 0x2a3444);
    }
    if (S->msg_unread)
        fill(nk_rect(24, 6, 9, 7), 1, T.accent); /* unread messages */
    if (S->job != JOB_NONE || S->store.fetch_pid > 0 || S->tube.fetch_pid > 0)
        arrow(31, SB_H / 2.f, 'v', T.cyan); /* download activity */
    if (S->tube.p.pid > 0)
        arrow(41, SB_H / 2.f, '>', S->tube.p.paused ? T.dim : T.ok); /* Tube is playing */

    /* right to left: clock, RAM, battery -- like the desktop panel */
    {
        float x = w - 6, tw;
        char ram[16];
        tw = text_w(S->fonts.small, clock, (int)strlen(clock));
        label(nk_rect(x - tw, 0, tw + 1, SB_H), clock, f, T.text, AL_LEFT);
        label(nk_rect(x - tw + 0.6f, 0, tw + 1, SB_H), clock, f, T.text, AL_LEFT); /* faux bold */
        x -= tw + 8;
        if (S->sys.mem_total_kb > 0) {
            snprintf(ram, sizeof(ram), "RAM %ldM", (S->sys.mem_total_kb - S->sys.mem_avail_kb) / 1024);
            tw = text_w(f, ram, (int)strlen(ram));
            label(nk_rect(x - tw, 0, tw + 1, SB_H), ram, f, T.dim, AL_LEFT);
            x -= tw + 8;
        }
        if (S->sys.battery >= 0) {
            char pct[8];
            uint32_t col = S->sys.charging ? T.ok : S->sys.battery <= 15 ? T.warn : T.text;
            snprintf(pct, sizeof(pct), "%d%%", S->sys.battery);
            tw = text_w(f, pct, (int)strlen(pct));
            label(nk_rect(x - tw, 0, tw + 1, SB_H), pct, f, T.text, AL_LEFT);
            x -= tw + 22;
            nk_stroke_rect(cv, nk_rect(x, 6, 17, 9), 1, 1, C(T.dim));
            fill(nk_rect(x + 17, 8, 2, 5), 0, T.dim);
            fill(nk_rect(x + 2, 8, 13.f * S->sys.battery / 100.f, 5), 0, col);
        }
    }
}

static void header(const char *title, const char *sub)
{
    float w = (float)S->surf->w;
    const struct nk_font *f = S->fonts.normal;
    float tw;
    int len;
    const char *t = ellipsize(f, title, w * ((sub && *sub) ? 0.68f : 1.f) - 20, &len);
    fill_a(nk_rect(0, SB_H, w, HDR_H), 0, 0x060b14, 200);
    fill(nk_rect(0, SB_H + HDR_H - 1, w, 1), 0, T.border);
    label(nk_rect(10, SB_H, w - 20, HDR_H - 2), t, f, T.text, AL_LEFT);
    /* azure underline under the title, like the active tab */
    tw = text_w(f, t, len);
    fill(nk_rect(8, SB_H + HDR_H - 3, tw + 6, 2), 1, T.accent);
    if (sub && *sub)
        label(nk_rect(w * 0.68f, SB_H, w * 0.32f - 8, HDR_H - 2), sub, S->fonts.small, T.dim, AL_RIGHT);
}

static void softkeys(const char *l, const char *c, const char *r)
{
    float w = (float)S->surf->w, y = S->surf->h - SK_H;
    vgrad(nk_rect(0, y, w, SK_H), 0x0b1424, 0x04070c);
    fill(nk_rect(0, y, w, 1), 0, T.border);
    if (l)
        label(nk_rect(8, y, w / 3 - 8, SK_H), l, S->fonts.normal, T.text, AL_LEFT);
    if (c)
        label(nk_rect(w / 3, y, w / 3, SK_H), c, S->fonts.normal, T.cyan, AL_CENTER);
    if (r)
        label(nk_rect(w * 2 / 3, y, w / 3 - 8, SK_H), r, S->fonts.normal, T.text, AL_RIGHT);
}

static void spinner(float cx, float cy, float rad)
{
    int phase = (int)(S->now / 120) % 8;
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159265f / 4;
        int age = (phase - i + 8) % 8;
        uint32_t col = age == 0 ? T.cyan : age < 3 ? T.accent : 0x2a3a50;
        nk_fill_circle(cv, nk_rect(cx + rad * cosf(a) - 2.5f, cy + rad * sinf(a) - 2.5f, 5, 5), C(col));
    }
}

/* Keep `sel` inside a window of `rows` starting at *top. */
static void keep_visible(int sel, int *top, int rows, int count)
{
    if (sel < *top)
        *top = sel;
    if (sel >= *top + rows)
        *top = sel - rows + 1;
    if (*top > count - rows)
        *top = count - rows;
    if (*top < 0)
        *top = 0;
}

static void scrollbar(struct nk_rect area, int top, int rows, int count)
{
    float h, y;
    if (count <= rows)
        return;
    h = area.h * rows / count;
    if (h < 12) h = 12;
    y = area.y + (area.h - h) * top / (count - rows);
    fill_a(nk_rect(area.x + area.w - 4, area.y, 3, area.h), 1, 0xffffff, 20);
    fill(nk_rect(area.x + area.w - 4, y, 3, h), 1, T.accent);
}

static void list_row(struct nk_rect r, int selected, const char *title, const char *sub,
                     const char *right, uint32_t right_col)
{
    const struct nk_font *f = S->fonts.normal;
    float rw = right ? text_w(S->fonts.small, right, (int)strlen(right)) + 10 : 0;
    if (selected)
        highlight(nk_rect(r.x + 4, r.y + 1, r.w - 10, r.h - 2));
    else
        fill(nk_rect(r.x + 8, r.y + r.h - 1, r.w - 16, 1), 0, T.sep);
    if (sub) {
        label(nk_rect(r.x + 10, r.y + 3, r.w - 20 - rw, r.h / 2), title, f, selected ? T.ink : T.text, AL_LEFT);
        label(nk_rect(r.x + 10, r.y + r.h / 2 - 1, r.w - 20 - rw, r.h / 2), sub, S->fonts.small,
              selected ? T.ink_dim : T.dim, AL_LEFT);
    } else {
        label(nk_rect(r.x + 10, r.y, r.w - 20 - rw, r.h), title, f, selected ? T.ink : T.text, AL_LEFT);
    }
    if (right)
        label(nk_rect(r.x + r.w - rw - 10, r.y, rw, r.h), right, S->fonts.small,
              selected ? T.ink_dim : right_col, AL_RIGHT);
}

static void toast_overlay(void)
{
    const char *msg = NULL;
    float w = (float)S->surf->w, y = S->surf->h - SK_H - 34;
    if (S->job != JOB_NONE)
        msg = S->job_label;
    else if (S->toast[0] && S->now < S->toast_until)
        msg = S->toast;
    if (!msg)
        return;
    fill_a(nk_rect(8, y, w - 16, 28), 6, 0x08101c, 240);
    nk_stroke_rect(cv, nk_rect(8, y, w - 16, 28), 6, 1, C(T.accent));
    if (S->job != JOB_NONE) {
        spinner(24, y + 14, 6);
        label(nk_rect(38, y, w - 54, 28), msg, S->fonts.small, T.text, AL_LEFT);
    } else {
        label(nk_rect(16, y, w - 32, 28), msg, S->fonts.small, T.text, AL_CENTER);
    }
}

static void toast(const char *msg)
{
    vx_strlcpy(S->toast, msg, sizeof(S->toast));
    S->toast_until = S->now + 2500;
}

/* ---- navigation ------------------------------------------------------ */

static enum screen top(void)
{
    return S->stack[S->depth - 1];
}

static void push(enum screen s)
{
    if (S->depth < (int)VX_ARRAY_LEN(S->stack))
        S->stack[S->depth++] = s;
}

static void pop(void)
{
    if (S->depth > 1) {
        tube_screen_closed(top());
        S->depth--;
    }
}

static void go_home(void)
{
    while (S->depth > 1)
        pop();
}

/* ---- background jobs ------------------------------------------------- */

/* Fire-and-forget helpers (hang up, power off) still need reaping; the
 * shell cannot waitpid(-1) because that would steal the exit status of the
 * tracked app/job/store children. */
static pid_t detached[8];

static void spawn_detached(char *const argv[])
{
    pid_t p = proc_spawn(argv);
    for (size_t i = 0; p > 0 && i < VX_ARRAY_LEN(detached); i++)
        if (!detached[i]) {
            detached[i] = p;
            return;
        }
}

static void reap_detached(void)
{
    for (size_t i = 0; i < VX_ARRAY_LEN(detached); i++)
        if (detached[i] && proc_poll(detached[i], NULL) != 0)
            detached[i] = 0;
}

static int start_job(enum job_kind k, const char *lbl, char *const argv[])
{
    if (S->job != JOB_NONE) {
        toast("Busy, please wait");
        return -1;
    }
    S->job_pid = proc_spawn(argv);
    if (S->job_pid < 0) {
        toast("Could not start helper");
        return -1;
    }
    S->job = k;
    vx_strlcpy(S->job_label, lbl, sizeof(S->job_label));
    return 0;
}

static int installed_index(const char *id)
{
    for (int i = 0; i < S->apps.count; i++)
        if (strcmp(S->apps.list[i].id, id) == 0)
            return i;
    return -1;
}

static void launch_app(int idx)
{
    struct vx_app *a = &S->apps.list[idx];
    char dir[400];
    char *argv[] = { S->cfg.run_cmd, dir, NULL };

    switch (a->kind) {
    case APP_BUILTIN_STORE:
        if (S->store.state == STORE_EMPTY && store_load_cache(&S->store) != 0)
            store_refresh(&S->store);
        S->store_cat_sel = S->store_cat_top = 0;
        push(SCR_STORE);
        return;
    case APP_BUILTIN_SETTINGS:
        S->settings_sel = S->settings_top = 0;
        push(SCR_SETTINGS);
        return;
    case APP_BUILTIN_SYSINFO:
        sys_update(&S->sys);
        push(SCR_SYSINFO);
        return;
    case APP_BUILTIN_DIALER:
        S->dial[0] = '\0';
        push(SCR_DIALER);
        return;
    case APP_BUILTIN_CALC:
        push(SCR_CALC);
        return;
    case APP_BUILTIN_MESSAGES:
        messages_load();
        S->conv_sel = S->conv_top = 0;
        push(SCR_MESSAGES);
        return;
    case APP_BUILTIN_TORCH:
        torch_toggle();
        return;
    case APP_BUILTIN_TUBE:
        S->tube_sel = S->tube_top = 0;
        push(SCR_TUBE);
        return;
    case APP_KAIOS:
        break;
    }

    snprintf(dir, sizeof(dir), "%s/%s", S->cfg.apps_dir, a->id);
    S->app_pid = proc_spawn(argv);
    S->app_started = S->now;
    S->app_closing = 0;
    if (S->app_pid < 0) {
        S->app_pid = 0;
        toast("Could not start the web runtime");
        return;
    }
    /* Hand the display over and give back every byte we can spare: the
     * web engine needs it far more than we do. */
    plat_release_display();
    store_unload(&S->store);
    apps_drop_icons(&S->apps);
    tube_drop_thumbs(&S->tube);
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

static void open_detail(int store_idx)
{
    S->detail_app = store_idx;
    S->detail_scroll = 0;
    push(SCR_STORE_DETAIL);
}

static void refilter(void)
{
    S->list_n = store_filter(&S->store, S->store_cat, S->store_query, S->list_idx,
                             (int)VX_ARRAY_LEN(S->list_idx));
    if (S->list_sel >= S->list_n)
        S->list_sel = S->list_n ? S->list_n - 1 : 0;
}

static void install_selected(void)
{
    const struct store_app *a = &S->store.apps[S->detail_app];
    char lbl[64];
    char *argv[] = { S->cfg.install_cmd, "--id", a->slug, "--type", a->type,
                     "--name", a->name, "--url", a->url, NULL };
    snprintf(lbl, sizeof(lbl), "Installing %s\xe2\x80\xa6", a->name);
    start_job(JOB_INSTALL, lbl, argv);
}

static void wifi_scan(void)
{
    char *argv[] = { "/bin/sh", "-c", NULL, NULL };
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "exec vortex-wifi scan > '%s'", S->scan_path);
    argv[2] = cmd;
    S->wifi_n = 0;
    start_job(JOB_WIFI_SCAN, "Scanning for networks\xe2\x80\xa6", argv);
}

static void wifi_load_scan(void)
{
    FILE *f = fopen(S->scan_path, "r");
    char line[160];
    S->wifi_n = 0;
    S->wifi_sel = 0;
    if (!f)
        return;
    /* "ssid<TAB>signal-percent<TAB>secure(0/1)" per line */
    while (S->wifi_n < WIFI_MAX && fgets(line, sizeof(line), f)) {
        char *t1 = strchr(line, '\t'), *t2;
        if (!t1)
            continue;
        *t1++ = '\0';
        t2 = strchr(t1, '\t');
        if (!line[0])
            continue;
        vx_strlcpy(S->wifi_ssid[S->wifi_n], line, sizeof(S->wifi_ssid[0]));
        S->wifi_signal[S->wifi_n] = atoi(t1);
        S->wifi_secure[S->wifi_n] = t2 ? atoi(t2 + 1) : 1;
        S->wifi_n++;
    }
    fclose(f);
}

static void wifi_connect(const char *ssid, const char *psk)
{
    char *argv[] = { "vortex-wifi", "connect", (char *)ssid, (char *)psk, NULL };
    char lbl[64];
    snprintf(lbl, sizeof(lbl), "Connecting to %s\xe2\x80\xa6", ssid);
    start_job(JOB_WIFI_CONNECT, lbl, argv);
}

/* Bluetooth goes through the root helper (doas allows exactly this). */
#define RADIO "/usr/libexec/vortex/vortex-radio"

static void bt_scan(void)
{
    char *argv[] = { "/bin/sh", "-c", NULL, NULL };
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "exec doas " RADIO " bt scan 8 > '%s'", S->bt_scan_path);
    argv[2] = cmd;
    S->bt_n = 0;
    start_job(JOB_BT_SCAN, "Looking for devices\xe2\x80\xa6", argv);
}

static void bt_load_scan(void)
{
    FILE *f = fopen(S->bt_scan_path, "r");
    char line[160];
    S->bt_n = 0;
    S->bt_sel = 0;
    if (!f)
        return;
    /* "MAC<TAB>name<TAB>paired(0/1)" per line; paired devices first */
    for (int pass = 1; pass >= 0; pass--) {
        rewind(f);
        while (S->bt_n < BT_MAX && fgets(line, sizeof(line), f)) {
            char *t1 = strchr(line, '\t'), *t2;
            if (!t1)
                continue;
            *t1++ = '\0';
            t2 = strchr(t1, '\t');
            if (t2)
                *t2++ = '\0';
            if ((t2 && atoi(t2) == 1) != pass)
                continue;
            vx_strlcpy(S->bt_mac[S->bt_n], line, sizeof(S->bt_mac[0]));
            vx_strlcpy(S->bt_name[S->bt_n], t1, sizeof(S->bt_name[0]));
            S->bt_name[S->bt_n][strcspn(S->bt_name[S->bt_n], "\n")] = '\0';
            S->bt_paired[S->bt_n] = pass;
            S->bt_n++;
        }
    }
    fclose(f);
}

static void bt_connect(int i)
{
    char *argv[] = { "doas", RADIO, "bt", "connect", S->bt_mac[i], NULL };
    char lbl[64];
    snprintf(lbl, sizeof(lbl), "Connecting to %s\xe2\x80\xa6", S->bt_name[i]);
    start_job(JOB_BT_CONNECT, lbl, argv);
}

static void dial(void)
{
    char *argv[] = { S->cfg.call_cmd, "dial", S->dial, NULL };
    if (!S->dial[0])
        return;
    if (start_job(JOB_CALL, "Calling\xe2\x80\xa6", argv) == 0)
        S->call_active = 1;
}

static void hangup(void)
{
    char *argv[] = { S->cfg.call_cmd, "hangup", NULL };
    spawn_detached(argv);
    S->call_active = 0;
    toast("Call ended");
}

static void open_menu(enum menu_kind k)
{
    S->menu = k;
    S->menu_sel = 0;
    push(SCR_MENU);
}

static int menu_items(const char **items)
{
    switch (S->menu) {
    case MENU_POWER:
        items[0] = "Power off"; items[1] = "Restart"; items[2] = "Cancel";
        return 3;
    case MENU_APP_OPTIONS:
        items[0] = "Open"; items[1] = "Uninstall"; items[2] = "Cancel";
        return 3;
    case MENU_CONFIRM_REMOVE:
        items[0] = "Uninstall"; items[1] = "Cancel";
        return 2;
    }
    return 0;
}

static void menu_select(void)
{
    char *argv[4] = { NULL };
    int sel = S->menu_sel;
    enum menu_kind k = S->menu;
    pop();
    switch (k) {
    case MENU_POWER:
        if (sel == 0 || sel == 1) {
            argv[0] = "vortex-power";
            argv[1] = sel == 0 ? "off" : "reboot";
            spawn_detached(argv);
        }
        break;
    case MENU_APP_OPTIONS:
        if (sel == 0)
            launch_app(S->menu_app);
        else if (sel == 1)
            open_menu(MENU_CONFIRM_REMOVE);
        break;
    case MENU_CONFIRM_REMOVE:
        if (sel == 0) {
            char lbl[64];
            argv[0] = S->cfg.remove_cmd;
            argv[1] = S->apps.list[S->menu_app].id;
            snprintf(lbl, sizeof(lbl), "Removing %s\xe2\x80\xa6", S->apps.list[S->menu_app].name);
            start_job(JOB_REMOVE, lbl, argv);
        }
        break;
    }
}

/* ---- settings model -------------------------------------------------- */

enum { SET_BRIGHTNESS, SET_TIMEOUT, SET_WIFI, SET_WIFI_NETS, SET_BT, SET_BT_DEVS,
       SET_STORE_REFRESH, SET_SYSINFO, SET_POWER, SET_COUNT };

static void settings_value(int i, char *buf, size_t n)
{
    buf[0] = '\0';
    switch (i) {
    case SET_BRIGHTNESS:
        if (S->brightness >= 0)
            snprintf(buf, n, "< %d%% >", S->brightness);
        else
            snprintf(buf, n, "n/a");
        break;
    case SET_TIMEOUT:
        if (S->cfg.screen_timeout)
            snprintf(buf, n, "< %d s >", S->cfg.screen_timeout);
        else
            snprintf(buf, n, "< Never >");
        break;
    case SET_WIFI:
        snprintf(buf, n, "%s", S->sys.wifi_up ? "Connected" : S->sys.wifi_on ? "On" : "Off");
        break;
    case SET_BT:
        snprintf(buf, n, "%s", S->sys.bt_on ? "On" : "Off");
        break;
    default:
        break;
    }
}

static const char *settings_name(int i)
{
    static const char *const names[SET_COUNT] = {
        "Brightness", "Screen timeout", "Wi-Fi", "Wi-Fi networks", "Bluetooth",
        "Bluetooth devices", "Update store list",
        "System info", "Power",
    };
    return names[i];
}

static void settings_adjust(int dir)
{
    static const int timeouts[] = { 15, 30, 60, 120, 300, 0 };
    if (S->settings_sel == SET_BRIGHTNESS && S->brightness >= 0) {
        S->brightness += dir * 10;
        if (S->brightness < 10) S->brightness = 10;
        if (S->brightness > 100) S->brightness = 100;
        sys_backlight_set(S->brightness);
    } else if (S->settings_sel == SET_TIMEOUT) {
        int i = 0, n = (int)VX_ARRAY_LEN(timeouts);
        while (i < n && timeouts[i] != S->cfg.screen_timeout)
            i++;
        i = ((i == n ? 1 : i) + dir + n) % n;
        S->cfg.screen_timeout = timeouts[i];
    }
}

static void settings_activate(void)
{
    char *argv[3] = { "vortex-wifi", NULL, NULL };
    switch (S->settings_sel) {
    case SET_WIFI:
        sys_update(&S->sys);
        argv[1] = S->sys.wifi_on ? "off" : "on";
        start_job(JOB_WIFI_TOGGLE, S->sys.wifi_on ? "Turning Wi-Fi off\xe2\x80\xa6" : "Turning Wi-Fi on\xe2\x80\xa6", argv);
        break;
    case SET_WIFI_NETS:
        push(SCR_WIFI);
        wifi_scan();
        break;
    case SET_BT: {
        char *bt[] = { "doas", RADIO, "bt", NULL, NULL };
        sys_update(&S->sys);
        bt[3] = S->sys.bt_on ? "off" : "on";
        start_job(JOB_BT_TOGGLE, S->sys.bt_on ? "Turning Bluetooth off\xe2\x80\xa6" :
                                                "Turning Bluetooth on\xe2\x80\xa6", bt);
        break;
    }
    case SET_BT_DEVS:
        push(SCR_BT);
        bt_scan();
        break;
    case SET_STORE_REFRESH:
        if (store_refresh(&S->store) == 0)
            toast("Updating store list\xe2\x80\xa6");
        break;
    case SET_SYSINFO:
        sys_update(&S->sys);
        push(SCR_SYSINFO);
        break;
    case SET_POWER:
        open_menu(MENU_POWER);
        break;
    default:
        settings_adjust(1);
        break;
    }
}

/* ---- screens: drawing ----------------------------------------------- */

static void draw_home(void)
{
    char hm[8], date[32];
    time_t t = time(NULL);
    struct tm tm;
    float w = (float)S->surf->w;

    localtime_r(&t, &tm);
    strftime(hm, sizeof(hm), "%H:%M", &tm);
    strftime(date, sizeof(date), "%A, %e %B", &tm);
    float ch = S->fonts.clock->handle.height, cy = SB_H + 14;
    float card_y = cy + ch + 34, card_h = 30 + 4 * 26 + 2, ry;
    long used = (S->sys.mem_total_kb - S->sys.mem_avail_kb) / 1024;
    char v[48];
    label(nk_rect(0, cy, w, ch), hm, S->fonts.clock, T.text, AL_CENTER);
    label(nk_rect(0, cy + ch + 4, w, 20), date, S->fonts.normal, T.dim, AL_CENTER);

    /* the desktop's SYSTEM INFO panel, as a card */
    glass(nk_rect(8, card_y, w - 16, card_h), 8);
    caps(18, card_y + 8, "System info");
    ry = card_y + 30;
    label(nk_rect(18, ry, 70, 16), "Battery", S->fonts.small, T.cyan, AL_LEFT);
    if (S->sys.battery >= 0) {
        snprintf(v, sizeof(v), "%d%%%s", S->sys.battery, S->sys.charging ? " charging" : "");
        label(nk_rect(90, ry, w - 108, 16), v, S->fonts.small, T.text, AL_LEFT);
        meter(nk_rect(90, ry + 17, w - 108, 3), S->sys.battery, S->sys.battery <= 15 ? T.warn : T.accent);
    } else {
        label(nk_rect(90, ry, w - 108, 16), "AC", S->fonts.small, T.text, AL_LEFT);
    }
    ry += 26;
    label(nk_rect(18, ry, 70, 16), "Memory", S->fonts.small, T.cyan, AL_LEFT);
    snprintf(v, sizeof(v), "%ld / %ld MB", used, S->sys.mem_total_kb / 1024);
    label(nk_rect(90, ry, w - 108, 16), v, S->fonts.small, T.text, AL_LEFT);
    meter(nk_rect(90, ry + 17, w - 108, 3),
          S->sys.mem_total_kb ? (int)(used * 100 * 1024 / S->sys.mem_total_kb) : 0, T.accent);
    ry += 26;
    label(nk_rect(18, ry, 70, 16), "Network", S->fonts.small, T.cyan, AL_LEFT);
    label(nk_rect(90, ry, w - 108, 16), S->sys.wifi_up ? "Wi-Fi connected" : S->sys.net_up ? "Connected" : "Offline",
          S->fonts.small, S->sys.net_up ? T.text : T.dim, AL_LEFT);
    ry += 26;
    label(nk_rect(18, ry, 70, 16), "Uptime", S->fonts.small, T.cyan, AL_LEFT);
    snprintf(v, sizeof(v), "%ldh %02ldm", S->sys.uptime_s / 3600, S->sys.uptime_s / 60 % 60);
    label(nk_rect(90, ry, w - 108, 16), v, S->fonts.small, T.text, AL_LEFT);
    softkeys("Store", "Menu", "Settings");
}

/* ---- built-in app icons ------------------------------------------------
 * Drawn from primitives on a 24x24 grid, so they cost no image memory and
 * scale to any tile size.  Each is a tinted rounded tile with a white glyph. */

static float ic_x, ic_y, ic_s;          /* tile origin and size */
#define GX(v) (ic_x + (v) * ic_s / 24.f)
#define GY(v) (ic_y + (v) * ic_s / 24.f)
#define GS(v) ((v) * ic_s / 24.f)

static void ic_circle(float cx, float cy, float r, uint32_t col)
{
    nk_fill_circle(cv, nk_rect(GX(cx - r), GY(cy - r), GS(2 * r), GS(2 * r)), C(col));
}

static void ic_poly(const float *pts, int n, uint32_t col)
{
    float buf[64];
    for (int i = 0; i < n && i < 32; i++) {
        buf[2 * i] = GX(pts[2 * i]);
        buf[2 * i + 1] = GY(pts[2 * i + 1]);
    }
    nk_fill_polygon(cv, buf, n, C(col));
}

static void icon_tile(uint32_t base)
{
    float r = GS(5);
    nk_fill_rect(cv, nk_rect(ic_x, ic_y, ic_s, ic_s), r, C(base));
    /* soft sheen on the upper half, darker rim at the bottom */
    nk_fill_rect(cv, nk_rect(ic_x + 1, ic_y + 1, ic_s - 2, ic_s * 0.48f), r, nk_rgba(255, 255, 255, 46));
    nk_fill_rect(cv, nk_rect(ic_x + GS(3), ic_y + ic_s - 2, ic_s - GS(6), 2), 1, nk_rgba(0, 0, 0, 50));
}

static void icon_store(void)
{
    icon_tile(0xe8a22c);
    /* bag handle: a ring whose lower half hides behind the bag */
    nk_stroke_circle(cv, nk_rect(GX(8.6f), GY(4.6f), GS(6.8f), GS(6.8f)), GS(1.6f), C(0xffffff));
    nk_fill_rect(cv, nk_rect(GX(5), GY(8), GS(14), GS(12)), GS(2), C(0xffffff));
    /* banana: a thick yellow arc with brown tips */
    {
        float pts[2 * 9];
        for (int i = 0; i < 9; i++) {
            float a = (25.f + i * 16.25f) * 3.14159265f / 180.f;
            pts[2 * i] = GX(12 + 4.6f * cosf(a));
            pts[2 * i + 1] = GY(10.2f + 4.6f * sinf(a));
        }
        nk_stroke_polyline(cv, pts, 9, GS(2.6f), C(0xf7c825));
        ic_circle(12 + 4.6f * cosf(0.436f), 10.2f + 4.6f * sinf(0.436f), 0.9f, 0x6b4310);
        ic_circle(12 - 4.6f * cosf(0.436f), 10.2f + 4.6f * sinf(0.436f), 0.9f, 0x6b4310);
    }
}

static void icon_phone(void)
{
    /* handset, simplified from the familiar "call" glyph */
    static const float handset[] = {
        4.0f, 3.2f, 7.4f, 3.2f, 8.4f, 4.2f, 9.0f, 7.6f, 8.7f, 8.6f, 6.6f, 10.8f,
        8.6f, 13.8f, 10.2f, 15.4f, 13.2f, 17.4f, 15.4f, 15.3f, 16.4f, 15.0f,
        19.8f, 15.6f, 20.8f, 16.6f, 20.8f, 19.8f, 19.8f, 20.8f, 16.8f, 20.6f,
        13.0f, 19.5f, 9.5f, 17.5f, 6.4f, 14.6f, 4.4f, 11.0f, 3.2f, 7.0f, 3.1f, 4.2f,
    };
    icon_tile(0x29b36a);
    ic_poly(handset, (int)(VX_ARRAY_LEN(handset) / 2), 0xffffff);
}

static void icon_settings(void)
{
    const uint32_t tile = 0x557497;
    icon_tile(tile);
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159265f / 4, ca = cosf(a), sa = sinf(a);
        /* tooth: a small square pushed out along angle a */
        float px[4] = { -1.7f, 1.7f, 1.7f, -1.7f }, py[4] = { 5.0f, 5.0f, 8.4f, 8.4f }, pts[8];
        for (int k = 0; k < 4; k++) {
            pts[2 * k] = 12 + px[k] * ca - py[k] * sa;
            pts[2 * k + 1] = 12 + px[k] * sa + py[k] * ca;
        }
        ic_poly(pts, 4, 0xffffff);
    }
    ic_circle(12, 12, 6.2f, 0xffffff);
    ic_circle(12, 12, 2.6f, tile);
}

static void icon_system(void)
{
    static const float h[] = { 5, 9, 13 };
    icon_tile(0x2a86de);
    for (int i = 0; i < 3; i++)
        nk_fill_rect(cv, nk_rect(GX(5.5f + i * 4.8f), GY(18.5f - h[i]), GS(3.4f), GS(h[i])), GS(1), C(0xffffff));
    nk_fill_rect(cv, nk_rect(GX(4.5f), GY(19.2f), GS(15), GS(1.2f)), GS(0.6f), nk_rgba(255, 255, 255, 200));
}

static void icon_calc(void)
{
    const uint32_t tile = 0x7b5cd6;
    icon_tile(tile);
    nk_fill_rect(cv, nk_rect(GX(6), GY(3.5f), GS(12), GS(17)), GS(2), C(0xffffff));
    nk_fill_rect(cv, nk_rect(GX(7.6f), GY(5.2f), GS(8.8f), GS(3.6f)), GS(0.8f), C(tile));
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            nk_fill_rect(cv, nk_rect(GX(7.6f + c * 3.2f), GY(10.4f + r * 3.2f), GS(2.4f), GS(2.4f)), GS(0.6f),
                         C(r == 2 && c == 2 ? 0xf2994a : tile));
}

static void icon_messages(void)
{
    const uint32_t tile = 0x2f80d1;
    icon_tile(tile);
    /* speech bubble with three dots */
    nk_fill_rect(cv, nk_rect(GX(4), GY(5), GS(16), GS(11.5f)), GS(3), C(0xffffff));
    nk_fill_triangle(cv, GX(7), GY(15), GX(12), GY(15), GX(6.5f), GY(19.5f), C(0xffffff));
    for (int i = 0; i < 3; i++)
        ic_circle(8.5f + i * 3.5f, 10.8f, 1.1f, tile);
}

static void icon_torch(void)
{
    const uint32_t tile = 0xd9a227;
    icon_tile(tile);
    /* torch head, body and beam */
    nk_fill_rect(cv, nk_rect(GX(9.5f), GY(11), GS(5), GS(9.5f)), GS(1.2f), C(0xffffff));
    nk_fill_triangle(cv, GX(7), GY(8), GX(17), GY(8), GX(14.5f), GY(11.5f), C(0xffffff));
    nk_fill_triangle(cv, GX(7), GY(8), GX(14.5f), GY(11.5f), GX(9.5f), GY(11.5f), C(0xffffff));
    nk_fill_rect(cv, nk_rect(GX(11.2f), GY(13.5f), GS(1.6f), GS(2.5f)), 0, C(tile));
    for (int i = -1; i <= 1; i++)
        nk_stroke_line(cv, GX(12 + i * 3.2f), GY(6.2f), GX(12 + i * 4.6f), GY(3.2f), GS(1.2f), C(0xfff3c4));
}

static void icon_tube(void)
{
    const uint32_t tile = 0xd63a35;
    icon_tile(tile);
    /* a screen with a play button */
    nk_fill_rect(cv, nk_rect(GX(3.5f), GY(6), GS(17), GS(12)), GS(3.5f), C(0xffffff));
    nk_fill_triangle(cv, GX(10), GY(9), GX(10), GY(15), GX(15.2f), GY(12), C(tile));
}

/* Returns 0 if `kind` has no built-in icon. */
static int builtin_icon(enum app_kind kind, struct nk_rect r)
{
    ic_x = r.x;
    ic_y = r.y;
    ic_s = r.w;
    switch (kind) {
    case APP_BUILTIN_STORE: icon_store(); return 1;
    case APP_BUILTIN_DIALER: icon_phone(); return 1;
    case APP_BUILTIN_SETTINGS: icon_settings(); return 1;
    case APP_BUILTIN_SYSINFO: icon_system(); return 1;
    case APP_BUILTIN_CALC: icon_calc(); return 1;
    case APP_BUILTIN_MESSAGES: icon_messages(); return 1;
    case APP_BUILTIN_TORCH: icon_torch(); return 1;
    case APP_BUILTIN_TUBE: icon_tube(); return 1;
    default: return 0;
    }
}

#define GRID_COLS 3

static void draw_launcher(void)
{
    struct nk_rect area = content_rect(1);
    int rows = 3;
    float cw = area.w / GRID_COLS, ch = area.h / rows;
    int count = S->apps.count;
    int sel_row = S->launcher_sel / GRID_COLS, total_rows = (count + GRID_COLS - 1) / GRID_COLS;
    char sub[16];
    const struct vx_app *sel = &S->apps.list[S->launcher_sel];

    snprintf(sub, sizeof(sub), "%d/%d", S->launcher_sel + 1, count);
    header(sel->name, sub);
    keep_visible(sel_row, &S->launcher_top, rows, total_rows);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int i = (S->launcher_top + r) * GRID_COLS + c;
            struct nk_rect cell = nk_rect(area.x + c * cw, area.y + r * ch, cw, ch);
            struct vx_app *a;
            struct vx_image *icon;
            float isz = 48, ix = cell.x + (cw - isz) / 2, iy = cell.y + (ch - isz) / 2;
            if (i >= count)
                continue;
            a = &S->apps.list[i];
            if (i == S->launcher_sel) {
                /* azure focus frame around the focused icon */
                struct nk_rect fr = nk_rect(ix - 9, iy - 9, isz + 18, isz + 18);
                fill_a(fr, 10, T.accent, 70);
                nk_stroke_rect(cv, fr, 10, 2, C(0x5aaef5));
            }
            icon = a->kind == APP_KAIOS ? apps_icon(&S->apps, i, (int)isz) : NULL;
            if (builtin_icon(a->kind, nk_rect(ix, iy, isz, isz))) {
                /* vector icon drawn above */
            } else if (icon) {
                struct nk_image img = nk_image_ptr(icon);
                nk_draw_image(cv, nk_rect(ix, iy, isz, isz), &img, nk_rgb(255, 255, 255));
            } else {
                /* Letter tile for apps without a PNG icon. */
                char letter[5] = { 0 };
                nk_rune cp = 0;
                int bl = nk_utf_decode(a->name, &cp, (int)strlen(a->name));
                memcpy(letter, a->name, (size_t)(bl > 0 && bl < 5 ? bl : 0));
                nk_fill_rect(cv, nk_rect(ix, iy, isz, isz), 10,
                             nk_rgb((a->color >> 16) & 0xff, (a->color >> 8) & 0xff, a->color & 0xff));
                /* glossy top half */
                nk_fill_rect(cv, nk_rect(ix + 2, iy + 2, isz - 4, isz / 2 - 2), 8, nk_rgba(255, 255, 255, 60));
                label(nk_rect(ix, iy, isz, isz), letter, cp <= 0xff ? S->fonts.large : S->fonts.normal,
                      0x0d1420, AL_CENTER);
            }
        }
    }
    scrollbar(area, S->launcher_top, rows, total_rows);
    softkeys(sel->kind == APP_KAIOS ? "Options" : NULL, "Open", "Back");
}

static void draw_store(void)
{
    struct nk_rect area = content_rect(1);
    struct store *st = &S->store;
    int rows = (int)(area.h / ROW_H), n = st->n_cats + 2;
    char sub[32] = "";

    if (st->fetch_pid > 0)
        snprintf(sub, sizeof(sub), "updating\xe2\x80\xa6");
    else if (st->n_apps)
        snprintf(sub, sizeof(sub), "%d apps", st->n_apps);
    header("Banana Store", sub);

    if (st->state == STORE_FETCHING || (st->state != STORE_READY && st->fetch_pid > 0)) {
        spinner(area.w / 2, area.y + area.h / 2 - 20, 10);
        label(nk_rect(0, area.y + area.h / 2, area.w, 24), "Downloading app list\xe2\x80\xa6",
              S->fonts.normal, T.dim, AL_CENTER);
        softkeys(NULL, NULL, "Back");
        return;
    }
    if (st->state != STORE_READY) {
        label(nk_rect(10, area.y + 30, area.w - 20, 24), "Store unavailable", S->fonts.large, T.text, AL_CENTER);
        label(nk_rect(10, area.y + 60, area.w - 20, 20), st->error[0] ? st->error : "No data yet",
              S->fonts.small, T.dim, AL_CENTER);
        label(nk_rect(10, area.y + 84, area.w - 20, 20), "Retrying automatically when online\xe2\x80\xa6",
              S->fonts.small, T.dim, AL_CENTER);
        softkeys("Refresh", NULL, "Back");
        return;
    }

    keep_visible(S->store_cat_sel, &S->store_cat_top, rows, n);
    for (int r = 0; r < rows && S->store_cat_top + r < n; r++) {
        int i = S->store_cat_top + r, cnt;
        char right[16];
        const char *name;
        int tmp[1];
        if (i == 0) {
            name = "Search\xe2\x80\xa6";
            right[0] = '\0';
        } else if (i == 1) {
            name = "All apps";
            snprintf(right, sizeof(right), "%d", st->n_apps);
        } else {
            name = st->cats[i - 2].name;
            cnt = 0;
            for (int a = 0; a < st->n_apps; a++)
                cnt += (st->apps[a].cats >> (i - 2)) & 1;
            snprintf(right, sizeof(right), "%d", cnt);
            (void)tmp;
        }
        list_row(nk_rect(area.x, area.y + r * ROW_H, area.w, ROW_H), i == S->store_cat_sel, name, NULL,
                 right[0] ? right : NULL, T.dim);
    }
    scrollbar(area, S->store_cat_top, rows, n);
    softkeys("Refresh", "Select", "Back");
}

static void draw_store_list(void)
{
    struct nk_rect area = content_rect(1);
    int rows = (int)(area.h / ROW2_H);
    char title[64], sub[16];

    if (S->store_query[0])
        snprintf(title, sizeof(title), "\xe2\x80\x9c%s\xe2\x80\x9d", S->store_query);
    else
        snprintf(title, sizeof(title), "%s", S->store_cat >= 0 ? S->store.cats[S->store_cat].name : "All apps");
    snprintf(sub, sizeof(sub), "%d", S->list_n);
    header(title, sub);
    if (!S->list_n) {
        label(nk_rect(0, area.y + 40, area.w, 24), "No apps found", S->fonts.normal, T.dim, AL_CENTER);
        softkeys("Search", NULL, "Back");
        return;
    }
    keep_visible(S->list_sel, &S->list_top, rows, S->list_n);
    for (int r = 0; r < rows && S->list_top + r < S->list_n; r++) {
        int i = S->list_top + r;
        const struct store_app *a = &S->store.apps[S->list_idx[i]];
        char sub2[96];
        int inst = installed_index(a->slug) >= 0;
        snprintf(sub2, sizeof(sub2), "%s%s%s", a->author[0] ? a->author : a->type,
                 a->has_ads ? " \xc2\xb7 ads" : "", a->has_tracking ? " \xc2\xb7 tracking" : "");
        list_row(nk_rect(area.x, area.y + r * ROW2_H, area.w, ROW2_H), i == S->list_sel, a->name, sub2,
                 inst ? "Installed" : NULL, T.ok);
    }
    scrollbar(area, S->list_top, rows, S->list_n);
    softkeys("Search", "View", "Back");
}

struct wrapdraw { float x, y, lh; int first, last; uint32_t col; };

static void draw_wrapped_line(const char *line, int len, int idx, void *u)
{
    struct wrapdraw *wd = u;
    if (idx < wd->first || idx > wd->last)
        return;
    nk_draw_text(cv, nk_rect(wd->x, wd->y + (idx - wd->first) * wd->lh, S->surf->w, wd->lh), line, len,
                 &S->fonts.normal->handle, nk_rgba(0, 0, 0, 0), C(wd->col));
}

static void draw_store_detail(void)
{
    struct nk_rect area = content_rect(1);
    const struct store_app *a = &S->store.apps[S->detail_app];
    int inst = installed_index(a->slug) >= 0;
    float y = area.y + 6, lh = S->fonts.normal->handle.height + 3;
    char meta[160];
    struct wrapdraw wd;
    int vis;

    header(a->name, NULL);
    snprintf(meta, sizeof(meta), "%s", a->author[0] ? a->author : "Unknown author");
    label(nk_rect(10, y, area.w - 20, 18), meta, S->fonts.small, T.dim, AL_LEFT);
    y += 18;
    snprintf(meta, sizeof(meta), "%s \xc2\xb7 %s%s", a->type, a->license, inst ? " \xc2\xb7 installed" : "");
    label(nk_rect(10, y, area.w - 20, 18), meta, S->fonts.small, T.dim, AL_LEFT);
    y += 18;
    if (a->has_ads || a->has_tracking) {
        snprintf(meta, sizeof(meta), "\xe2\x9a\xa0 %s%s%s", a->has_ads ? "Contains ads" : "",
                 a->has_ads && a->has_tracking ? ", " : "", a->has_tracking ? "tracking" : "");
        label(nk_rect(10, y, area.w - 20, 18), meta + 4, S->fonts.small, T.warn, AL_LEFT);
        y += 18;
    }
    if (!strcmp(a->type, "certified") || !strcmp(a->type, "root")) {
        label(nk_rect(10, y, area.w - 20, 18), "Needs system APIs: may not work", S->fonts.small, T.warn, AL_LEFT);
        y += 18;
    }
    fill(nk_rect(10, y + 3, area.w - 20, 1), 0, T.sep);
    y += 8;

    vis = (int)((area.y + area.h - y) / lh);
    S->detail_lines = wrap(S->fonts.normal, a->description, area.w - 20, NULL, NULL);
    if (S->detail_scroll > S->detail_lines - vis)
        S->detail_scroll = S->detail_lines - vis;
    if (S->detail_scroll < 0)
        S->detail_scroll = 0;
    wd = (struct wrapdraw){ 10, y, lh, S->detail_scroll, S->detail_scroll + vis - 1, T.text };
    nk_push_scissor(cv, nk_rect(0, y, area.w, area.y + area.h - y));
    wrap(S->fonts.normal, a->description, area.w - 20, draw_wrapped_line, &wd);
    nk_push_scissor(cv, nk_rect(0, 0, S->surf->w, S->surf->h));
    scrollbar(nk_rect(area.x, y, area.w, area.y + area.h - y), S->detail_scroll, vis, S->detail_lines);

    softkeys(inst ? "Open" : NULL, inst ? "Update" : "Install", "Back");
}

static void draw_settings(void)
{
    struct nk_rect area = content_rect(1);
    int rows = (int)(area.h / ROW_H);
    header("Settings", NULL);
    keep_visible(S->settings_sel, &S->settings_top, rows, SET_COUNT);
    for (int r = 0; r < rows && S->settings_top + r < SET_COUNT; r++) {
        int i = S->settings_top + r;
        char val[32];
        settings_value(i, val, sizeof(val));
        list_row(nk_rect(area.x, area.y + r * ROW_H, area.w, ROW_H), i == S->settings_sel, settings_name(i),
                 NULL, val[0] ? val : NULL, T.dim);
    }
    scrollbar(area, S->settings_top, rows, SET_COUNT);
    softkeys(NULL, S->settings_sel <= SET_TIMEOUT ? NULL : "Select", "Back");
}

static void draw_wifi(void)
{
    struct nk_rect area = content_rect(1);
    int rows = (int)(area.h / ROW_H), top = 0;
    header("Wi-Fi networks", NULL);
    if (S->job == JOB_WIFI_SCAN) {
        softkeys(NULL, NULL, "Back");
        return;
    }
    if (!S->wifi_n) {
        label(nk_rect(0, area.y + 40, area.w, 24), "No networks found", S->fonts.normal, T.dim, AL_CENTER);
        softkeys("Rescan", NULL, "Back");
        return;
    }
    keep_visible(S->wifi_sel, &top, rows, S->wifi_n);
    for (int r = 0; r < rows && top + r < S->wifi_n; r++) {
        int i = top + r;
        char right[16];
        snprintf(right, sizeof(right), "%s%d%%", S->wifi_secure[i] ? "\xc2\xb7 " : "", S->wifi_signal[i]);
        list_row(nk_rect(area.x, area.y + r * ROW_H, area.w, ROW_H), i == S->wifi_sel, S->wifi_ssid[i],
                 NULL, right, T.dim);
    }
    softkeys("Rescan", "Connect", "Back");
}

static void draw_bt(void)
{
    struct nk_rect area = content_rect(1);
    int rows = (int)(area.h / ROW_H), top = 0;
    header("Bluetooth", NULL);
    if (S->job == JOB_BT_SCAN) {
        softkeys(NULL, NULL, "Back");
        return;
    }
    if (!S->bt_n) {
        label(nk_rect(0, area.y + 40, area.w, 24), "No devices found", S->fonts.normal, T.dim, AL_CENTER);
        softkeys("Rescan", NULL, "Back");
        return;
    }
    keep_visible(S->bt_sel, &top, rows, S->bt_n);
    for (int r = 0; r < rows && top + r < S->bt_n; r++) {
        int i = top + r;
        list_row(nk_rect(area.x, area.y + r * ROW_H, area.w, ROW_H), i == S->bt_sel, S->bt_name[i],
                 NULL, S->bt_paired[i] ? "Paired" : NULL, T.dim);
    }
    softkeys("Rescan", "Connect", "Back");
}

static void draw_sysinfo(void)
{
    struct nk_rect area = content_rect(1);
    const char *k[9];
    char v[9][48];
    int n = 0;
    float y;
    long used = (S->sys.mem_total_kb - S->sys.mem_avail_kb) / 1024;

#define ROW(key, ...) do { k[n] = key; snprintf(v[n], sizeof(v[n]), __VA_ARGS__); n++; } while (0)
    header("System", NULL);
    ROW("Host", "vortex");
    ROW("Kernel", "Linux %s", S->sys.kernel);
    ROW("Uptime", "%ldh %02ldm", S->sys.uptime_s / 3600, S->sys.uptime_s / 60 % 60);
    ROW("Memory", "%ld / %ld MB", used, S->sys.mem_total_kb / 1024);
    ROW("Shell", "%ld KB RSS", S->sys.self_rss_kb);
    if (S->sys.battery >= 0)
        ROW("Battery", "%d%%%s", S->sys.battery, S->sys.charging ? " charging" : "");
    ROW("Network", "%s", S->sys.wifi_up ? "Wi-Fi connected" : S->sys.net_up ? "connected" : "offline");
    ROW("Apps", "%d installed", S->apps.count - APPS_BUILTIN);
    ROW("Vortex", "%s", VORTEX_VERSION);
#undef ROW

    glass(nk_rect(6, area.y + 6, area.w - 12, area.h - 12), 8);
    caps(16, area.y + 14, "System info");
    y = area.y + 36;
    for (int i = 0; i < n; i++, y += 21) {
        label(nk_rect(16, y, 70, 20), k[i], S->fonts.small, T.cyan, AL_LEFT);
        label(nk_rect(84, y, area.w - 100, 20), v[i], S->fonts.small, T.text, AL_LEFT);
        if (!strcmp(k[i], "Memory"))
            meter(nk_rect(84, y + 18, area.w - 100, 2),
                  S->sys.mem_total_kb ? (int)(used * 100 * 1024 / S->sys.mem_total_kb) : 0, T.accent);
    }
    softkeys(NULL, NULL, "Back");
}

static void draw_dialer(void)
{
    struct nk_rect area = content_rect(0);
    header("Phone", S->call_active ? "in call" : NULL);
    glass(nk_rect(8, area.y + 56, area.w - 16, 56), 8);
    label(nk_rect(8, area.y + 70, area.w - 16, S->fonts.large->handle.height + 8),
          S->dial[0] ? S->dial : "Enter number", S->fonts.large, S->dial[0] ? T.text : T.dim, AL_CENTER);
    if (S->call_active)
        label(nk_rect(0, area.y + 110, area.w, 20), "Press End to hang up", S->fonts.small, T.dim, AL_CENTER);
    softkeys(S->dial[0] ? "Clear" : NULL, "Call", "Back");
}

/* ---- calculator ------------------------------------------------------
 * KaiOS-style: digits type, the D-pad picks the operator (up +, down -,
 * left ×, right ÷), OK is "=", * is the decimal point, # flips the sign,
 * C deletes a digit. */

static const char *op_glyph(char op)
{
    switch (op) {
    case '+': return "+";
    case '-': return "-";
    case '*': return "\xc3\x97";  /* × */
    case '/': return "\xc3\xb7";  /* ÷ */
    default: return "";
    }
}

static void calc_format(double v, char *out, size_t n)
{
    if (isnan(v) || isinf(v)) {
        snprintf(out, n, "Error");
        return;
    }
    snprintf(out, n, "%.10g", v);
}

static double calc_apply(double a, char op, double b)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b == 0 ? NAN : a / b;
    default: return b;
    }
}

static double calc_value(void)
{
    return S->calc_cur[0] ? strtod(S->calc_cur, NULL) : 0;
}

static void calc_reset(void)
{
    S->calc_cur[0] = S->calc_expr[0] = '\0';
    S->calc_acc = 0;
    S->calc_op = 0;
    S->calc_fresh = 0;
}

static void calc_operator(char op)
{
    char a[24];
    if (!strcmp(S->calc_cur, "Error"))
        calc_reset();
    if (S->calc_op && !S->calc_fresh)
        S->calc_acc = calc_apply(S->calc_acc, S->calc_op, calc_value());
    else if (!S->calc_op)
        S->calc_acc = calc_value();
    S->calc_op = op;
    calc_format(S->calc_acc, a, sizeof(a));
    vx_strlcpy(S->calc_cur, a, sizeof(S->calc_cur));
    snprintf(S->calc_expr, sizeof(S->calc_expr), "%s %s", a, op_glyph(op));
    S->calc_fresh = 1;
}

static void calc_equals(void)
{
    char a[24], b[24], r[24];
    double res;
    if (!S->calc_op || S->calc_fresh)
        return;
    calc_format(S->calc_acc, a, sizeof(a));
    calc_format(calc_value(), b, sizeof(b));
    res = calc_apply(S->calc_acc, S->calc_op, calc_value());
    calc_format(res, r, sizeof(r));
    snprintf(S->calc_expr, sizeof(S->calc_expr), "%s %s %s =", a, op_glyph(S->calc_op), b);
    vx_strlcpy(S->calc_cur, r, sizeof(S->calc_cur));
    S->calc_op = 0;
    S->calc_fresh = 1;
}

static void key_calc(enum vx_key k)
{
    size_t len = strlen(S->calc_cur);
    if (k >= VK_0 && k <= VK_9) {
        if (S->calc_fresh || !strcmp(S->calc_cur, "Error") || !strcmp(S->calc_cur, "0")) {
            if (S->calc_fresh && !S->calc_op)
                S->calc_expr[0] = '\0'; /* new calculation after "=" */
            S->calc_cur[0] = '\0';
            len = 0;
            S->calc_fresh = 0;
        }
        if (len < 15) {
            S->calc_cur[len] = (char)('0' + (k - VK_0));
            S->calc_cur[len + 1] = '\0';
        }
        return;
    }
    switch (k) {
    case VK_STAR: /* decimal point */
        if (S->calc_fresh) {
            S->calc_cur[0] = '\0';
            S->calc_fresh = 0;
            len = 0;
        }
        if (!strchr(S->calc_cur, '.') && len < 14)
            strcat(S->calc_cur, len ? "." : "0.");
        break;
    case VK_HASH: { /* sign */
        char v[24];
        if (!S->calc_cur[0] || !strcmp(S->calc_cur, "Error"))
            break;
        calc_format(-calc_value(), v, sizeof(v));
        vx_strlcpy(S->calc_cur, v, sizeof(S->calc_cur));
        break;
    }
    case VK_UP: calc_operator('+'); break;
    case VK_DOWN: calc_operator('-'); break;
    case VK_LEFT: calc_operator('*'); break;
    case VK_RIGHT: calc_operator('/'); break;
    case VK_OK: calc_equals(); break;
    case VK_SOFT_LEFT: calc_reset(); break;
    case VK_BACK:
        if (len && !S->calc_fresh)
            S->calc_cur[len - 1] = '\0';
        else if (!len)
            pop();
        break;
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

static void draw_calc(void)
{
    struct nk_rect area = content_rect(1);
    static const struct { const char *key, *op; char id; } chips[] = {
        /* "^v<>" are drawn as arrow triangles (the font has no arrows) */
        { "^", "+", '+' }, { "v", "-", '-' }, { "<", "\xc3\x97", '*' }, { ">", "\xc3\xb7", '/' },
        { "OK", "=", '=' }, { "*", ".", '.' }, { "#", "+/-", '#' },
    };
    float w = area.w, y = area.y + 8, cw = (w - 24) / 4;

    header("Calculator", NULL);
    glass(nk_rect(8, y, w - 16, 70), 8);
    label(nk_rect(16, y + 6, w - 32, 18), S->calc_expr, S->fonts.small, T.dim, AL_RIGHT);
    label(nk_rect(16, y + 28, w - 32, 36), S->calc_cur[0] ? S->calc_cur : "0", S->fonts.large,
          !strcmp(S->calc_cur, "Error") ? T.warn : T.text, AL_RIGHT);

    /* operator keys: what each phone key does, the pending one lit */
    y += 84;
    for (int i = 0; i < (int)VX_ARRAY_LEN(chips); i++) {
        struct nk_rect r = nk_rect(8 + (i % 4) * (cw + 2.7f), y + (i / 4) * 44, cw, 38);
        int lit = chips[i].id == S->calc_op;
        if (lit)
            highlight(r);
        else
            glass(r, 6);
        if (i < 4)
            arrow(r.x + r.w / 2, r.y + 11, chips[i].key[0], lit ? T.ink_dim : T.cyan);
        else
            label(nk_rect(r.x, r.y + 3, r.w, 16), chips[i].key, S->fonts.small, lit ? T.ink_dim : T.cyan, AL_CENTER);
        label(nk_rect(r.x, r.y + 17, r.w, 18), chips[i].op, S->fonts.normal, T.text, AL_CENTER);
    }
    softkeys("Clear", "=", "Back");
}

/* ---- phone: calls and messages ---------------------------------------
 * vortex-modem (a root service) talks to ModemManager and leaves its state
 * in files; the shell polls them and acts through cfg.call_cmd
 * (vortex-call: dial, answer, hangup, speaker, sms).  VORTEX_RUN_DIR and
 * VORTEX_SMS_DIR move the files (tests). */

static const char *run_dir(void)
{
    const char *d = getenv("VORTEX_RUN_DIR");
    return d ? d : "/run/vortex";
}

static const char *sms_dir(void)
{
    const char *d = getenv("VORTEX_SMS_DIR");
    return d ? d : "/var/lib/vortex/sms";
}

/* Reads a one-line tab-separated file into up to n fields. */
static int read_fields(const char *name, char *line, size_t size, char **f, int n)
{
    char path[300];
    FILE *fp;
    int got = 0;
    snprintf(path, sizeof(path), "%s/%s", run_dir(), name);
    fp = fopen(path, "r");
    if (!fp)
        return -1;
    if (!fgets(line, (int)size, fp))
        line[0] = '\0';
    fclose(fp);
    line[strcspn(line, "\n")] = '\0';
    if (!line[0])
        return 0;
    for (char *p = line; got < n; got++) {
        f[got] = p;
        p = strchr(p, '\t');
        if (!p) {
            got++;
            break;
        }
        *p++ = '\0';
    }
    return got;
}

static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

static long seen_time(const char *number)
{
    char path[300], line[96];
    long t = 0;
    FILE *f;
    snprintf(path, sizeof(path), "%s/seen", sms_dir());
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (tab && (size_t)(tab - line) == strlen(number) && !strncmp(line, number, (size_t)(tab - line)))
            t = atol(tab + 1);
    }
    fclose(f);
    return t;
}

static void mark_seen(const char *number)
{
    char path[300];
    FILE *f;
    snprintf(path, sizeof(path), "%s/seen", sms_dir());
    f = fopen(path, "a");  /* the last line for a number wins */
    if (f) {
        fprintf(f, "%s\t%ld\n", number, (long)time(NULL));
        fclose(f);
    }
}

/* Load the newest MSG_MAX messages and group them by number, newest first. */
static void messages_load(void)
{
    char path[300], line[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/messages.tsv", sms_dir());
    S->msg_n = S->conv_n = S->msg_unread = 0;
    f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *fld[5], *p = line;
        int n = 0;
        struct sms *m;
        line[strcspn(line, "\n")] = '\0';
        while (n < 5 && p) {
            fld[n++] = p;
            p = n < 5 ? strchr(p, '\t') : NULL;
            if (p)
                *p++ = '\0';
        }
        if (n < 5)
            continue;
        if (S->msg_n == MSG_MAX) {  /* keep the newest */
            memmove(S->msgs, S->msgs + 1, sizeof(S->msgs[0]) * (MSG_MAX - 1));
            S->msg_n--;
        }
        m = &S->msgs[S->msg_n++];
        m->t = atol(fld[0]);
        m->out = !strcmp(fld[1], "out");
        m->failed = !strcmp(fld[3], "failed");
        vx_strlcpy(m->number, fld[2], sizeof(m->number));
        vx_strlcpy(m->text, fld[4], sizeof(m->text));
        unescape(m->text);
    }
    fclose(f);
    for (int i = S->msg_n - 1; i >= 0; i--) {
        struct sms *m = &S->msgs[i];
        int c = 0;
        while (c < S->conv_n && strcmp(S->conv[c].number, m->number))
            c++;
        if (c == S->conv_n) {
            if (S->conv_n == CONV_MAX)
                continue;
            vx_strlcpy(S->conv[c].number, m->number, sizeof(S->conv[c].number));
            S->conv[c].last = i;
            S->conv[c].unread = 0;
            S->conv_n++;
        }
        if (!m->out && m->t > seen_time(m->number)) {
            S->conv[c].unread++;
            S->msg_unread++;
        }
    }
    /* newest conversation first, by the time of its last message */
    for (int i = 1; i < S->conv_n; i++)
        for (int j = i; j > 0 && S->msgs[S->conv[j].last].t > S->msgs[S->conv[j - 1].last].t; j--) {
            struct { char number[24]; int last, unread; } tmp;
            memcpy(&tmp, &S->conv[j], sizeof(tmp));
            memcpy(&S->conv[j], &S->conv[j - 1], sizeof(tmp));
            memcpy(&S->conv[j - 1], &tmp, sizeof(tmp));
        }
    if (S->conv_sel >= S->conv_n)
        S->conv_sel = S->conv_n ? S->conv_n - 1 : 0;
}

static void open_thread(const char *number)
{
    vx_strlcpy(S->thread_number, number, sizeof(S->thread_number));
    S->thread_scroll = 0;
    mark_seen(number);
    messages_load();
    push(SCR_THREAD);
}

static void compose(void)
{
    t9_reset(&S->t9);
    S->input = INPUT_SMS;
    push(SCR_INPUT);
}

static void send_sms(void)
{
    char *argv[] = { S->cfg.call_cmd, "sms", S->thread_number, S->t9.text, NULL };
    if (!S->t9.text[0])
        return;
    start_job(JOB_SMS, "Sending\xe2\x80\xa6", argv);
}

static void call_cmd(const char *what, const char *arg)
{
    char *argv[] = { S->cfg.call_cmd, (char *)what, (char *)arg, NULL };
    spawn_detached(argv);
}

/* Follow vortex-modem's files: status bar, the call screen, new messages. */
static void modem_poll(void)
{
    char line[160], *f[4];
    int n;
    struct stat st;
    char path[300];

    /* no throttle: vortex-modem's SIGUSR1 must never be missed, and the
     * shell only ticks on events and timers anyway */

    n = read_fields("modem", line, sizeof(line), f, 3);
    if (n >= 1) {
        S->modem_seen = 1;
        vx_strlcpy(S->modem_state, f[0], sizeof(S->modem_state));
        vx_strlcpy(S->modem_op, n >= 2 ? f[1] : "", sizeof(S->modem_op));
        S->modem_signal = n >= 3 ? atoi(f[2]) : 0;
    }

    n = read_fields("call", line, sizeof(line), f, 4);
    if (n >= 4) {
        int was = S->call_state[0] != '\0';
        vx_strlcpy(S->call_state, f[0], sizeof(S->call_state));
        vx_strlcpy(S->call_number, f[1], sizeof(S->call_number));
        S->call_incoming = !strcmp(f[2], "in");
        S->call_since = atol(f[3]);
        S->call_active = 1;
        if (!was || top() != SCR_CALL) {
            /* a call takes the screen, even from a web app or a video */
            if (S->tube.p.pid > 0)
                tube_stop();
            if (S->app_pid) {
                S->app_closing = 1;
                proc_kill_group(S->app_pid, 1500);
            }
            if (S->blanked) {
                S->blanked = 0;
                plat_set_blank(0);
            }
            if (top() != SCR_CALL)
                push(SCR_CALL);
            S->force_redraw = 1;
        }
    } else if (n == 0 && S->call_state[0]) {
        S->call_state[0] = '\0';
        S->call_active = 0;
        S->call_speaker = 0;
        if (top() == SCR_CALL)
            pop();
        toast("Call ended");
        S->force_redraw = 1;
    }

    snprintf(path, sizeof(path), "%s/sms-new", run_dir());
    if (stat(path, &st) == 0 && (long)st.st_mtime != S->sms_new_mtime) {
        int first = S->sms_new_mtime == 0;
        S->sms_new_mtime = (long)st.st_mtime;
        messages_load();
        if (!first && S->msg_n) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Message from %s", S->msgs[S->msg_n - 1].number);
            toast(msg);
        }
        S->force_redraw = 1;
    }
}

/* Every LED whose name has "torch" in it is switched together: on the
 * 6300 4G the torch needs two GPIOs high at once (see the device tree). */
static void torch_toggle(void)
{
    const char *base = getenv("VORTEX_LEDS_DIR");
    char path[512];
    DIR *d;
    struct dirent *e;
    int found = 0, on = -1, failed = 0;

    if (!base)
        base = "/sys/class/leds";
    d = opendir(base);
    if (!d) {
        toast("No torch on this phone");
        return;
    }
    /* Two passes: read the state from the first torch LED, then write all. */
    for (int pass = 0; pass < 2; pass++) {
        rewinddir(d);
        while ((e = readdir(d))) {
            long v = 0;
            FILE *f;
            if (!strstr(e->d_name, "torch") && !strstr(e->d_name, "flash"))
                continue;
            snprintf(path, sizeof(path), "%s/%s/brightness", base, e->d_name);
            if (pass == 0) {
                if (vx_read_int(path, &v) == 0) {
                    found = 1;
                    if (on < 0)
                        on = v ? 0 : 1;
                }
                continue;
            }
            f = fopen(path, "w");
            if (!f) {
                failed = 1;
                continue;
            }
            fprintf(f, "%d\n", on ? 255 : 0);
            fclose(f);
        }
        if (!found)
            break;
    }
    closedir(d);
    if (!found)
        toast("No torch on this phone");
    else if (failed)
        toast("Torch: no permission");
    else
        toast(on ? "Torch on" : "Torch off");
}

static void draw_call(void)
{
    struct nk_rect area = content_rect(0);
    const char *what = "Call";
    char dur[16] = "";
    int ringing = !strcmp(S->call_state, "ringing-in");

    if (ringing)
        what = "Incoming call";
    else if (!strcmp(S->call_state, "dialing") || !strcmp(S->call_state, "ringing-out"))
        what = "Calling\xe2\x80\xa6";
    else if (!strcmp(S->call_state, "active")) {
        long d = (long)time(NULL) - S->call_since;
        what = S->call_speaker ? "Speaker on" : "In call";
        snprintf(dur, sizeof(dur), "%02ld:%02ld", d / 60, d % 60);
    } else if (!strcmp(S->call_state, "held"))
        what = "On hold";
    header("Phone", NULL);
    label(nk_rect(0, area.y + 50, area.w, 20), what, S->fonts.normal, ringing ? T.accent : T.dim, AL_CENTER);
    label(nk_rect(0, area.y + 80, area.w, S->fonts.large->handle.height + 8),
          S->call_number[0] ? S->call_number : "Unknown", S->fonts.large, T.text, AL_CENTER);
    if (dur[0])
        label(nk_rect(0, area.y + 130, area.w, 20), dur, S->fonts.normal, T.text, AL_CENTER);
    if (ringing)
        softkeys("Answer", NULL, "Reject");
    else
        softkeys(!strcmp(S->call_state, "active") ? (S->call_speaker ? "Earpiece" : "Speaker") : NULL,
                 NULL, "End");
}

static void key_call(enum vx_key k)
{
    int ringing = !strcmp(S->call_state, "ringing-in");
    if (ringing && (k == VK_CALL || k == VK_OK || k == VK_SOFT_LEFT)) {
        call_cmd("answer", NULL);
    } else if (k == VK_SOFT_RIGHT || k == VK_BACK) {
        call_cmd("hangup", NULL);
    } else if (k == VK_SOFT_LEFT && !strcmp(S->call_state, "active")) {
        S->call_speaker = !S->call_speaker;
        call_cmd("speaker", S->call_speaker ? "on" : "off");
    }
}

static void draw_messages(void)
{
    struct nk_rect area = content_rect(1);
    int rows = (int)(area.h / ROW2_H);
    header("Messages", S->msg_unread ? "new" : NULL);
    if (!S->conv_n) {
        label(nk_rect(0, area.y + 40, area.w, 24), "No messages", S->fonts.normal, T.dim, AL_CENTER);
        softkeys("New", NULL, "Back");
        return;
    }
    keep_visible(S->conv_sel, &S->conv_top, rows, S->conv_n);
    for (int r = 0; r < rows && S->conv_top + r < S->conv_n; r++) {
        int i = S->conv_top + r;
        const struct sms *m = &S->msgs[S->conv[i].last];
        char preview[48], badge[8] = "";
        snprintf(preview, sizeof(preview), "%s%s", m->out ? "You: " : "", m->text);
        preview[strcspn(preview, "\n")] = '\0';
        if (S->conv[i].unread)
            snprintf(badge, sizeof(badge), "%d new", S->conv[i].unread);
        list_row(nk_rect(area.x, area.y + r * ROW2_H, area.w, ROW2_H), i == S->conv_sel,
                 S->conv[i].number, preview, badge[0] ? badge : NULL, T.accent);
    }
    softkeys("New", "Open", "Back");
}

static void key_messages(enum vx_key k)
{
    switch (k) {
    case VK_UP: move(&S->conv_sel, S->conv_n, -1); break;
    case VK_DOWN: move(&S->conv_sel, S->conv_n, 1); break;
    case VK_OK:
        if (S->conv_n)
            open_thread(S->conv[S->conv_sel].number);
        break;
    case VK_SOFT_LEFT:
        t9_reset(&S->t9);
        S->t9.mode = T9_DIGITS;
        S->input = INPUT_SMS_NUMBER;
        push(SCR_INPUT);
        break;
    case VK_CALL:
        if (S->conv_n) {
            vx_strlcpy(S->dial, S->conv[S->conv_sel].number, sizeof(S->dial));
            dial();
        }
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

/* Thread: bubbles, newest at the bottom; scrolling moves up the history. */
static void draw_thread(void)
{
    struct nk_rect area = content_rect(1);
    const struct nk_font *f = S->fonts.normal;
    float lh = f->handle.height + 2, y = area.y + area.h - 4, maxw = area.w - 60;
    int skip = S->thread_scroll, shown = 0;

    header(S->thread_number, NULL);
    nk_push_scissor(cv, area);
    for (int i = S->msg_n - 1; i >= 0 && y > area.y; i--) {
        const struct sms *m = &S->msgs[i];
        struct wrapdraw wd;
        float h, bw = 0, x;
        int lines;
        if (strcmp(m->number, S->thread_number))
            continue;
        if (skip > 0) {
            skip--;
            continue;
        }
        lines = wrap(f, m->text, maxw, NULL, NULL);
        h = lines * lh + 8;
        for (const char *p = m->text; *p;) {  /* bubble width: the widest line */
            size_t len = strcspn(p, "\n");
            float w = text_w(f, p, (int)len);
            bw = w > bw ? w : bw;
            p += len + (p[len] == '\n');
        }
        bw = (bw > maxw ? maxw : bw) + 14;
        y -= h + 6;
        x = m->out ? area.w - bw - 6 : 6;
        fill_a(nk_rect(x, y, bw, h), 8, m->out ? T.accent : 0x22324a, m->out ? 200 : 230);
        wd = (struct wrapdraw){ x + 7, y + 4, lh, 0, lines - 1, m->failed ? T.warn : T.text };
        wrap(f, m->text, maxw, draw_wrapped_line, &wd);
        shown++;
    }
    nk_push_scissor(cv, nk_rect(0, 0, S->surf->w, S->surf->h));
    if (!shown)
        label(nk_rect(0, area.y + 40, area.w, 24), "No messages yet", f, T.dim, AL_CENTER);
    softkeys("Reply", "Call", "Back");
}

static void key_thread(enum vx_key k)
{
    switch (k) {
    case VK_UP: S->thread_scroll++; break;
    case VK_DOWN: if (S->thread_scroll) S->thread_scroll--; break;
    case VK_SOFT_LEFT: compose(); break;
    case VK_OK:
    case VK_CALL:
        vx_strlcpy(S->dial, S->thread_number, sizeof(S->dial));
        dial();
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

/* ---- Tube: Invidious client ------------------------------------------- */

#define TROW_H 48   /* video rows: thumbnail, title, two small lines */

static int tube_video_playing(void)
{
    return S->tube.p.pid > 0 && S->tube.p.video;
}

static int tube_audio_playing(void)
{
    return S->tube.p.pid > 0 && !S->tube.p.video;
}

static const char *host_of(const char *url)
{
    const char *h = strstr(url, "://");
    return h ? h + 3 : url;
}

/* Hand the display to mpv and free what the shell can spare for it. */
static void tube_watch(const struct tube_video *v, int start)
{
    char err[300];
    snprintf(err, sizeof(err), "%s.err", S->tube.p.sock);
    unlink(err);
    if (tube_play(&S->tube, v, 1, start, S->now) != 0) {
        toast("Could not start the player");
        return;
    }
    plat_release_display();
    store_unload(&S->store);
    apps_drop_icons(&S->apps);
    tube_drop_thumbs(&S->tube);
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

static void tube_listen(const struct tube_video *v, int start)
{
    char err[300];
    snprintf(err, sizeof(err), "%s.err", S->tube.p.sock);
    unlink(err);
    if (tube_play(&S->tube, v, 0, start, S->now) != 0) {
        toast("Could not start the player");
        return;
    }
    if (top() != SCR_TUBE_PLAYER)
        push(SCR_TUBE_PLAYER);
}

/* mpv is gone (stopped, finished or failed). */
static void tube_player_gone(int code, int stopped)
{
    int was_video = S->tube.p.video, quick = S->now - S->tube.p.started < 20000;
    S->tube.p.video = 0;
    if (was_video) {
        plat_resume_display();
        S->force_redraw = 1;
        S->last_input = S->now;
        if (S->store.state != STORE_READY)
            store_load_cache(&S->store);
    }
    if (!stopped && code != 0 && quick) {
        char path[300], *msg;
        size_t len;
        snprintf(path, sizeof(path), "%s.err", S->tube.p.sock);
        msg = vx_read_file(path, &len, 200);
        if (msg && msg[0] == '!') {
            msg[strcspn(msg, "\n")] = '\0';
            toast(msg + 1);
        } else {
            toast(code == 2 ? "This video cannot be played" : "Playback failed");
        }
        free(msg);
    }
    if (top() == SCR_TUBE_PLAYER)
        pop();
}

static void tube_stop(void)
{
    if (S->tube.p.pid <= 0)
        return;
    tube_player_stop(&S->tube);
    tube_player_gone(0, 1);
}

static void tube_open(enum tube_kind kind, const char *arg, const char *title)
{
    S->tube_err[0] = '\0';
    if (tube_open_list(&S->tube, kind, arg, title) != 0) {
        toast("Go back a little first");
        return;
    }
    push(SCR_TUBE_LIST);
}

static void tube_search(const char *q)
{
    char title[80];
    if (!q[0])
        return;
    tube_add_search(&S->tube, q);
    snprintf(title, sizeof(title), "\xe2\x80\x9c%s\xe2\x80\x9d", q);
    tube_open(TK_SEARCH, q, title);
}

static void tube_new_search(void)
{
    t9_reset(&S->t9);
    S->t9.mode = T9_LOWER;
    S->input = INPUT_TUBE_SEARCH;
    push(SCR_INPUT);
}

static void tube_show_video(const struct tube_video *v)
{
    if (tube_open_video(&S->tube, v) != 0) {
        toast("Go back a little first");
        return;
    }
    push(SCR_TUBE_VIDEO);
}

/* Small "12:34 · 1.2M views" style line. */
static void tube_meta(const struct tube_video *v, char *out, size_t n)
{
    char dur[16] = "", views[16] = "";
    if (v->secs > 0)
        tube_format_time(v->secs, dur, sizeof(dur));
    if (v->views > 0)
        tube_format_count(v->views, views, sizeof(views));
    snprintf(out, n, "%s%s%s%s%s%s", dur, dur[0] && views[0] ? " \xc2\xb7 " : "", views,
             views[0] ? " views" : "", (dur[0] || views[0]) && v->published[0] ? " \xc2\xb7 " : "",
             v->published);
}

static void thumb_box(struct nk_rect r, const char *id, int big)
{
    struct vx_image *im = id[0] ? tube_thumb(&S->tube, id, big, (int)r.w, (int)r.h) : NULL;
    if (im) {
        struct nk_image img = nk_image_ptr(im);
        nk_draw_image(cv, r, &img, nk_rgb(255, 255, 255));
    } else {
        fill(r, 3, 0x1a2638);
        arrow(r.x + r.w / 2, r.y + r.h / 2, '>', 0x3d5068);
    }
}

/* -- home ----------------------------------------------------------------- */

enum { TH_PLAYING, TH_SEARCH, TH_SUBS, TH_POPULAR, TH_TRENDING, TH_HISTORY, TH_SAVED, TH_SETTINGS };

static int tube_home_items(int *items)
{
    int n = 0;
    if (tube_audio_playing())
        items[n++] = TH_PLAYING;
    for (int i = TH_SEARCH; i <= TH_SETTINGS; i++)
        items[n++] = i;
    return n;
}

static void draw_tube(void)
{
    struct nk_rect area = content_rect(1);
    int items[8], n = tube_home_items(items), rows = (int)(area.h / ROW_H);
    static const char *const names[] = { "Now playing", "Search", "Subscriptions", "Popular",
                                         "Trending", "History", "Saved", "Settings" };
    header("Tube", S->tube.instance[0] ? host_of(S->tube.instance) : NULL);
    if (S->tube_sel >= n)
        S->tube_sel = n - 1;
    keep_visible(S->tube_sel, &S->tube_top, rows, n);
    for (int r = 0; r < rows && S->tube_top + r < n; r++) {
        int i = S->tube_top + r, it = items[i];
        char right[48] = "";
        if (it == TH_PLAYING)
            vx_strlcpy(right, S->tube.p.v.title, sizeof(right));
        else if (it == TH_SUBS)
            snprintf(right, sizeof(right), "%d", tube_count_subs(&S->tube));
        else if (it == TH_SETTINGS)
            snprintf(right, sizeof(right), "%sp", S->tube.quality);
        list_row(nk_rect(area.x, area.y + r * ROW_H, area.w, ROW_H), i == S->tube_sel, names[it],
                 NULL, right[0] ? right : NULL, it == TH_PLAYING ? T.cyan : T.dim);
    }
    scrollbar(area, S->tube_top, rows, n);
    softkeys("Search", "Open", "Back");
}

static void key_tube(enum vx_key k)
{
    int items[8], n = tube_home_items(items);
    switch (k) {
    case VK_UP: move(&S->tube_sel, n, -1); break;
    case VK_DOWN: move(&S->tube_sel, n, 1); break;
    case VK_SOFT_LEFT: tube_new_search(); break;
    case VK_OK:
        switch (items[S->tube_sel]) {
        case TH_PLAYING: push(SCR_TUBE_PLAYER); break;
        case TH_SEARCH:
            if (S->tube.n_searches) {
                S->tube_sel = S->tube_top = 0;
                push(SCR_TUBE_SEARCH);
            } else {
                tube_new_search();
            }
            break;
        case TH_SUBS: tube_open(TK_FEED, NULL, "Subscriptions"); break;
        case TH_POPULAR: tube_open(TK_POPULAR, NULL, "Popular"); break;
        case TH_TRENDING: tube_open(TK_TRENDING, NULL, "Trending"); break;
        case TH_HISTORY: tube_open(TK_HISTORY, NULL, "History"); break;
        case TH_SAVED: tube_open(TK_SAVED, NULL, "Saved"); break;
        case TH_SETTINGS:
            S->tube_sel = S->tube_top = 0;
            push(SCR_TUBE_SETTINGS);
            break;
        }
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

/* -- recent searches ------------------------------------------------------- */

static void draw_tube_search(void)
{
    struct nk_rect area = content_rect(1);
    int n = S->tube.n_searches + 1, rows = (int)(area.h / ROW_H);
    header("Search", NULL);
    keep_visible(S->tube_sel, &S->tube_top, rows, n);
    for (int r = 0; r < rows && S->tube_top + r < n; r++) {
        int i = S->tube_top + r;
        list_row(nk_rect(area.x, area.y + r * ROW_H, area.w, ROW_H), i == S->tube_sel,
                 i == 0 ? "New search\xe2\x80\xa6" : S->tube.searches[i - 1], NULL, NULL, 0);
    }
    scrollbar(area, S->tube_top, rows, n);
    softkeys(S->tube.n_searches ? "Clear" : NULL, "Search", "Back");
}

static void key_tube_search(enum vx_key k)
{
    int n = S->tube.n_searches + 1;
    switch (k) {
    case VK_UP: move(&S->tube_sel, n, -1); break;
    case VK_DOWN: move(&S->tube_sel, n, 1); break;
    case VK_OK:
        if (S->tube_sel == 0) {
            tube_new_search();
        } else {
            char q[64];
            vx_strlcpy(q, S->tube.searches[S->tube_sel - 1], sizeof(q));
            S->tube_sel = 0;
            tube_search(q);
        }
        break;
    case VK_SOFT_LEFT:
        tube_clear_searches(&S->tube);
        S->tube_sel = 0;
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default:
        /* typing straight away starts a new search with that letter */
        if (k >= VK_2 && k <= VK_9) {
            tube_new_search();
            t9_key(&S->t9, k, S->now);
        }
        break;
    }
}

/* -- video lists --------------------------------------------------------------- */

static void draw_tube_list(void)
{
    struct nk_rect area = content_rect(1);
    struct tube_list *l = tube_list(&S->tube);
    int rows = (int)(area.h / TROW_H), n;
    char sub[16];
    if (!l) {
        header("Tube", NULL);
        return;
    }
    n = l->n + (l->kind == TK_SEARCH && (l->more || S->tube.fetch == TF_MORE));
    snprintf(sub, sizeof(sub), l->n ? "%d/%d" : "", l->sel + 1, l->n);
    header(l->title, sub);
    if (!l->n) {
        if (l->loading) {
            spinner(area.w / 2, area.y + 60, 10);
            label(nk_rect(0, area.y + 80, area.w, 24), "Loading\xe2\x80\xa6", S->fonts.normal, T.dim, AL_CENTER);
        } else {
            label(nk_rect(0, area.y + 40, area.w, 24),
                  l->kind == TK_HISTORY ? "Nothing watched yet" :
                  l->kind == TK_SAVED ? "Nothing saved yet" : "No videos",
                  S->fonts.normal, T.dim, AL_CENTER);
            if (S->tube_err[0]) {
                struct wrapdraw wd = { 12, area.y + 70, S->fonts.normal->handle.height + 2, 0, 4, T.warn };
                wrap(S->fonts.normal, S->tube_err, area.w - 24, draw_wrapped_line, &wd);
            }
        }
        softkeys(NULL, NULL, "Back");
        return;
    }
    if (l->sel >= n)
        l->sel = n - 1;
    keep_visible(l->sel, &l->top, rows, n);
    for (int r = 0; r < rows && l->top + r < n; r++) {
        int i = l->top + r, sel = i == l->sel;
        struct nk_rect row = nk_rect(area.x, area.y + r * TROW_H, area.w, TROW_H);
        const struct tube_video *v;
        char meta[96];
        if (i == l->n) {  /* the "more" row at the end of search results */
            if (sel)
                highlight(nk_rect(row.x + 4, row.y + 1, row.w - 10, row.h - 2));
            if (S->tube.fetch == TF_MORE)
                spinner(row.x + 24, row.y + row.h / 2, 6);
            label(nk_rect(row.x + 40, row.y, row.w - 50, row.h),
                  S->tube.fetch == TF_MORE ? "Loading more\xe2\x80\xa6" : "More results",
                  S->fonts.normal, sel ? T.ink : T.dim, AL_LEFT);
            continue;
        }
        v = &l->v[i];
        if (sel)
            highlight(nk_rect(row.x + 4, row.y + 1, row.w - 10, row.h - 2));
        else
            fill(nk_rect(row.x + 8, row.y + row.h - 1, row.w - 16, 1), 0, T.sep);
        thumb_box(nk_rect(row.x + 8, row.y + 6, 64, 36), v->id, 0);
        label(nk_rect(row.x + 78, row.y + 2, row.w - 88, 18), v->title, S->fonts.normal,
              sel ? T.ink : T.text, AL_LEFT);
        label(nk_rect(row.x + 78, row.y + 19, row.w - 88, 14), v->author, S->fonts.small,
              sel ? T.ink_dim : T.cyan, AL_LEFT);
        tube_meta(v, meta, sizeof(meta));
        label(nk_rect(row.x + 78, row.y + 32, row.w - 88, 14), meta, S->fonts.small,
              sel ? T.ink_dim : T.dim, AL_LEFT);
    }
    scrollbar(area, l->top, rows, n);
    softkeys("Listen", "Open", "Back");
}

static void key_tube_list(enum vx_key k)
{
    struct tube_list *l = tube_list(&S->tube);
    int n;
    if (!l) {
        pop();
        return;
    }
    n = l->n + (l->kind == TK_SEARCH && (l->more || S->tube.fetch == TF_MORE));
    switch (k) {
    case VK_UP: move(&l->sel, n, -1); break;
    case VK_DOWN: move(&l->sel, n, 1); break;
    case VK_LEFT:  /* a page up / down */
        l->sel = l->sel >= 4 ? l->sel - 4 : 0;
        break;
    case VK_RIGHT:
        l->sel = l->sel + 4 < n ? l->sel + 4 : (n ? n - 1 : 0);
        break;
    case VK_OK:
        if (l->sel == l->n)
            tube_load_more(&S->tube);
        else if (l->sel < l->n)
            tube_show_video(&l->v[l->sel]);
        break;
    case VK_SOFT_LEFT:
        if (l->sel < l->n)
            tube_listen(&l->v[l->sel], 0);
        break;
    case VK_CALL:
        if (l->sel < l->n)
            tube_watch(&l->v[l->sel], 0);
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
    /* reaching the end of the results fetches the next page */
    if (l->kind == TK_SEARCH && l->more && l->sel >= l->n - 1 && S->tube.fetch == TF_NONE)
        tube_load_more(&S->tube);
}

/* -- one video -------------------------------------------------------------- */

enum { TV_WATCH, TV_LISTEN, TV_RELATED, TV_CHANNEL, TV_SAVE, TV_SUB, TV_DESC };

static int tube_video_items(const struct tube_detail *d, int *items)
{
    int n = 0;
    items[n++] = TV_WATCH;
    items[n++] = TV_LISTEN;
    if (d->n_rel || d->loading)
        items[n++] = TV_RELATED;
    if (d->v.author_id[0]) {
        items[n++] = TV_CHANNEL;
    }
    items[n++] = TV_SAVE;
    if (d->v.author_id[0])
        items[n++] = TV_SUB;
    if (d->desc && d->desc[0])
        items[n++] = TV_DESC;
    return n;
}

static void draw_tube_video(void)
{
    struct nk_rect area = content_rect(1);
    struct tube_detail *d = tube_detail(&S->tube);
    const struct nk_font *f = S->fonts.normal;
    float y = area.y, lh = f->handle.height + 1, rh = 28, x2 = 136;
    int items[8], n, rows;
    char buf[64];
    struct wrapdraw wd;

    if (!d) {
        header("Tube", NULL);
        return;
    }
    header(d->v.author[0] ? d->v.author : "Video", NULL);
    thumb_box(nk_rect(8, y + 6, 120, 68), d->v.id, 0);
    if (d->v.secs > 0) {
        tube_format_time(d->v.secs, buf, sizeof(buf));
        label(nk_rect(x2, y + 6, area.w - x2 - 8, 18), buf, f, T.cyan, AL_LEFT);
    }
    if (d->v.views > 0) {
        tube_format_count(d->v.views, buf, sizeof(buf));
        strcat(buf, " views");
        label(nk_rect(x2, y + 25, area.w - x2 - 8, 15), buf, S->fonts.small, T.text, AL_LEFT);
    }
    if (d->likes > 0) {
        tube_format_count(d->likes, buf, sizeof(buf));
        strcat(buf, " likes");
        label(nk_rect(x2, y + 40, area.w - x2 - 8, 15), buf, S->fonts.small, T.dim, AL_LEFT);
    }
    if (d->v.published[0])
        label(nk_rect(x2, y + 55, area.w - x2 - 8, 15), d->v.published, S->fonts.small, T.dim, AL_LEFT);
    if (d->loading)
        spinner(area.w - 16, y + 64, 5);
    y += 78;
    /* the title, up to two lines */
    wd = (struct wrapdraw){ 8, y, lh, 0, 1, T.text };
    if (wrap(f, d->v.title, area.w - 16, NULL, NULL) > 1) {
        wrap(f, d->v.title, area.w - 16, draw_wrapped_line, &wd);
        y += 2 * lh;
    } else {
        wrap(f, d->v.title, area.w - 16, draw_wrapped_line, &wd);
        y += lh;
    }
    fill(nk_rect(8, y + 3, area.w - 16, 1), 0, T.sep);
    y += 6;

    n = tube_video_items(d, items);
    rows = (int)((area.y + area.h - y) / rh);
    if (d->sel >= n)
        d->sel = n - 1;
    keep_visible(d->sel, &d->top, rows, n);
    for (int r = 0; r < rows && d->top + r < n; r++) {
        int i = d->top + r, it = items[i];
        const char *name = "";
        char right[48] = "";
        switch (it) {
        case TV_WATCH: name = "Watch"; snprintf(right, sizeof(right), "%sp", S->tube.quality); break;
        case TV_LISTEN: name = "Listen"; vx_strlcpy(right, "audio only", sizeof(right)); break;
        case TV_RELATED:
            name = "Related videos";
            if (d->n_rel)
                snprintf(right, sizeof(right), "%d", d->n_rel);
            break;
        case TV_CHANNEL: name = "Channel"; break;
        case TV_SAVE: name = d->saved ? "Saved \xe2\x80\x94 remove" : "Save"; break;
        case TV_SUB: name = d->subbed ? "Unsubscribe" : "Subscribe"; break;
        case TV_DESC: name = "Description"; break;
        }
        list_row(nk_rect(area.x, y + r * rh, area.w, rh), i == d->sel, name, NULL, right[0] ? right : NULL, T.dim);
    }
    scrollbar(nk_rect(area.x, y, area.w, rows * rh), d->top, rows, n);
    softkeys("Listen", "Select", "Back");
}

static void key_tube_video(enum vx_key k)
{
    struct tube_detail *d = tube_detail(&S->tube);
    int items[8], n;
    char title[80];
    if (!d) {
        pop();
        return;
    }
    n = tube_video_items(d, items);
    switch (k) {
    case VK_UP: move(&d->sel, n, -1); break;
    case VK_DOWN: move(&d->sel, n, 1); break;
    case VK_SOFT_LEFT: tube_listen(&d->v, 0); break;
    case VK_CALL: tube_watch(&d->v, 0); break;
    case VK_OK:
        switch (items[d->sel]) {
        case TV_WATCH: tube_watch(&d->v, 0); break;
        case TV_LISTEN: tube_listen(&d->v, 0); break;
        case TV_RELATED:
            if (d->n_rel)
                tube_open(TK_RELATED, d->v.id, "Related");
            else
                toast("Still loading\xe2\x80\xa6");
            break;
        case TV_CHANNEL:
            vx_strlcpy(title, d->v.author, sizeof(title));
            tube_open(TK_CHANNEL, d->v.author_id, title);
            break;
        case TV_SAVE:
            d->saved = tube_toggle_saved(&S->tube, &d->v);
            toast(d->saved ? "Saved" : "Removed from saved");
            break;
        case TV_SUB:
            d->subbed = tube_toggle_sub(&S->tube, d->v.author_id, d->v.author);
            toast(d->subbed ? "Subscribed" : "Unsubscribed");
            break;
        case TV_DESC:
            S->tube_text_scroll = 0;
            push(SCR_TUBE_TEXT);
            break;
        }
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

static void draw_tube_text(void)
{
    struct nk_rect area = content_rect(1);
    struct tube_detail *d = tube_detail(&S->tube);
    const struct nk_font *f = S->fonts.normal;
    float lh = f->handle.height + 3;
    int vis = (int)(area.h / lh) - 1;
    struct wrapdraw wd;
    header("Description", NULL);
    if (!d || !d->desc) {
        softkeys(NULL, NULL, "Back");
        return;
    }
    S->tube_text_lines = wrap(f, d->desc, area.w - 20, NULL, NULL);
    if (S->tube_text_scroll > S->tube_text_lines - vis)
        S->tube_text_scroll = S->tube_text_lines - vis;
    if (S->tube_text_scroll < 0)
        S->tube_text_scroll = 0;
    wd = (struct wrapdraw){ 10, area.y + 6, lh, S->tube_text_scroll, S->tube_text_scroll + vis, T.text };
    nk_push_scissor(cv, area);
    wrap(f, d->desc, area.w - 20, draw_wrapped_line, &wd);
    nk_push_scissor(cv, nk_rect(0, 0, S->surf->w, S->surf->h));
    scrollbar(area, S->tube_text_scroll, vis, S->tube_text_lines);
    softkeys(NULL, NULL, "Back");
}

static void key_tube_text(enum vx_key k)
{
    switch (k) {
    case VK_UP: if (S->tube_text_scroll > 0) S->tube_text_scroll--; break;
    case VK_DOWN: S->tube_text_scroll++; break;
    case VK_LEFT: S->tube_text_scroll -= 8; break;
    case VK_RIGHT: S->tube_text_scroll += 8; break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

/* -- playing ------------------------------------------------------------------ */

/* Keys while mpv shows a video (the shell draws nothing then). */
static void key_tube_video_playing(enum vx_key k)
{
    switch (k) {
    case VK_OK: tube_player_cmd(&S->tube, "cycle pause"); break;
    case VK_LEFT: tube_player_cmd(&S->tube, "osd-msg-bar seek -10"); break;
    case VK_RIGHT: tube_player_cmd(&S->tube, "osd-msg-bar seek 10"); break;
    case VK_4: tube_player_cmd(&S->tube, "osd-msg-bar seek -60"); break;
    case VK_6: tube_player_cmd(&S->tube, "osd-msg-bar seek 60"); break;
    case VK_UP:
    case VK_VOL_UP: tube_player_cmd(&S->tube, "osd-msg-bar add volume 10"); break;
    case VK_DOWN:
    case VK_VOL_DOWN: tube_player_cmd(&S->tube, "osd-msg-bar add volume -10"); break;
    case VK_5:
    case VK_SOFT_LEFT: tube_player_cmd(&S->tube, "show-progress"); break;
    /* hold the phone sideways for a bigger picture */
    case VK_HASH: tube_player_cmd(&S->tube, "cycle-values video-rotate 90 0"); break;
    /* picture off: sound only, much less work for the CPU */
    case VK_STAR: tube_player_cmd(&S->tube, "cycle video"); break;
    case VK_BACK:
    case VK_SOFT_RIGHT: tube_stop(); break;
    default: break;
    }
}

static void draw_tube_player(void)
{
    struct nk_rect area = content_rect(1);
    struct tube_player *p = &S->tube.p;
    const struct nk_font *f = S->fonts.normal;
    float y = area.y + 4, lh = f->handle.height + 1;
    char a[16], b[16], st[48];
    struct wrapdraw wd;
    int pct;

    header("Now playing", NULL);
    if (p->pid <= 0) {
        label(nk_rect(0, area.y + 40, area.w, 24), "Nothing playing", f, T.dim, AL_CENTER);
        softkeys(NULL, NULL, "Back");
        return;
    }
    thumb_box(nk_rect(40, y, 160, 90), p->v.id, 0);
    y += 96;
    wd = (struct wrapdraw){ 10, y, lh, 0, 1, T.text };
    wrap(f, p->v.title, area.w - 20, draw_wrapped_line, &wd);
    y += 2 * lh;
    label(nk_rect(10, y, area.w - 20, 16), p->v.author, S->fonts.small, T.cyan, AL_LEFT);
    y += 22;
    pct = p->dur > 0 ? (int)(p->pos * 100 / p->dur) : 0;
    meter(nk_rect(10, y, area.w - 20, 4), pct, T.accent);
    y += 8;
    tube_format_time((long)p->pos, a, sizeof(a));
    tube_format_time((long)p->dur, b, sizeof(b));
    label(nk_rect(10, y, 80, 16), a, S->fonts.small, T.text, AL_LEFT);
    label(nk_rect(area.w - 90, y, 80, 16), b, S->fonts.small, T.dim, AL_RIGHT);
    if (p->fd < 0 || p->buffering)
        vx_strlcpy(st, "Loading\xe2\x80\xa6", sizeof(st));
    else if (p->paused)
        vx_strlcpy(st, "Paused", sizeof(st));
    else
        snprintf(st, sizeof(st), "Volume %d", p->volume);
    label(nk_rect(0, y, area.w, 16), st, S->fonts.small, p->paused ? T.warn : T.dim, AL_CENTER);
    softkeys("Watch", p->paused ? "Play" : "Pause", "Stop");
}

static void key_tube_player(enum vx_key k)
{
    struct tube_player *p = &S->tube.p;
    if (p->pid <= 0) {
        if (k == VK_BACK || k == VK_SOFT_RIGHT)
            pop();
        return;
    }
    switch (k) {
    case VK_OK: tube_player_cmd(&S->tube, "cycle pause"); break;
    case VK_LEFT: tube_player_cmd(&S->tube, "seek -10"); break;
    case VK_RIGHT: tube_player_cmd(&S->tube, "seek 10"); break;
    case VK_4: tube_player_cmd(&S->tube, "seek -60"); break;
    case VK_6: tube_player_cmd(&S->tube, "seek 60"); break;
    case VK_UP:
    case VK_VOL_UP: tube_player_cmd(&S->tube, "add volume 10"); break;
    case VK_DOWN:
    case VK_VOL_DOWN: tube_player_cmd(&S->tube, "add volume -10"); break;
    case VK_SOFT_LEFT: {
        struct tube_video v = p->v;
        int at = (int)p->pos;
        pop();
        tube_watch(&v, at > 3 ? at - 2 : 0);
        break;
    }
    case VK_SOFT_RIGHT: tube_stop(); break;
    case VK_BACK: pop(); break;   /* keeps playing */
    default: break;
    }
}

/* -- settings -------------------------------------------------------------------- */

static void draw_tube_settings(void)
{
    struct nk_rect area = content_rect(1);
    static const char *const names[] = { "Quality", "Server", "Clear history", "Clear searches" };
    char q[16];
    header("Tube settings", NULL);
    snprintf(q, sizeof(q), "%sp", S->tube.quality);
    for (int i = 0; i < 4; i++)
        list_row(nk_rect(area.x, area.y + i * ROW_H, area.w, ROW_H), i == S->tube_sel, names[i], NULL,
                 i == 0 ? q : i == 1 ? (S->tube.instance[0] ? host_of(S->tube.instance) : "automatic") : NULL,
                 T.cyan);
    label(nk_rect(10, area.y + 4 * ROW_H + 10, area.w - 20, 16), "Lower quality plays smoother", S->fonts.small,
          T.dim, AL_LEFT);
    softkeys(NULL, "Select", "Back");
}

static void tube_cycle_quality(int dir)
{
    static const char *const q[] = { "144", "240", "360" };
    int i = 1;
    for (int j = 0; j < 3; j++)
        if (!strcmp(S->tube.quality, q[j]))
            i = j;
    i = (i + dir + 3) % 3;
    tube_set(&S->tube, "quality", q[i]);
}

static void key_tube_settings(enum vx_key k)
{
    switch (k) {
    case VK_UP: move(&S->tube_sel, 4, -1); break;
    case VK_DOWN: move(&S->tube_sel, 4, 1); break;
    case VK_LEFT:
    case VK_RIGHT:
        if (S->tube_sel == 0)
            tube_cycle_quality(k == VK_LEFT ? -1 : 1);
        break;
    case VK_OK:
        switch (S->tube_sel) {
        case 0: tube_cycle_quality(1); break;
        case 1:
            S->tube_srv_sel = S->tube_srv_top = 0;
            tube_fetch_servers(&S->tube);
            push(SCR_TUBE_SERVERS);
            break;
        case 2: tube_clear_history(&S->tube); toast("History cleared"); break;
        case 3: tube_clear_searches(&S->tube); toast("Searches cleared"); break;
        }
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

static void draw_tube_servers(void)
{
    struct nk_rect area = content_rect(1);
    int n = S->tube.n_servers, rows = (int)(area.h / ROW2_H);
    header("Server", NULL);
    if (S->tube.servers_loading) {
        spinner(area.w / 2, area.y + 60, 10);
        label(nk_rect(0, area.y + 80, area.w, 24), "Testing servers\xe2\x80\xa6", S->fonts.normal, T.dim, AL_CENTER);
        softkeys("Type", NULL, "Back");
        return;
    }
    if (!n)
        label(nk_rect(0, area.y + 40, area.w, 24), "No servers found", S->fonts.normal, T.dim, AL_CENTER);
    keep_visible(S->tube_srv_sel, &S->tube_srv_top, rows, n);
    for (int r = 0; r < rows && S->tube_srv_top + r < n; r++) {
        int i = S->tube_srv_top + r;
        const struct tube_server *s = &S->tube.servers[i];
        char sub[48];
        if (s->ok)
            snprintf(sub, sizeof(sub), "works \xc2\xb7 %d ms", s->ms);
        else
            vx_strlcpy(sub, "does not answer the app", sizeof(sub));
        list_row(nk_rect(area.x, area.y + r * ROW2_H, area.w, ROW2_H), i == S->tube_srv_sel, host_of(s->url),
                 sub, !strcmp(s->url, S->tube.instance) ? "in use" : NULL, T.ok);
    }
    scrollbar(area, S->tube_srv_top, rows, n);
    softkeys("Type", "Use", "Back");
}

static void key_tube_servers(enum vx_key k)
{
    int n = S->tube.n_servers;
    switch (k) {
    case VK_UP: move(&S->tube_srv_sel, n, -1); break;
    case VK_DOWN: move(&S->tube_srv_sel, n, 1); break;
    case VK_OK:
        if (n && !S->tube.servers_loading) {
            const struct tube_server *s = &S->tube.servers[S->tube_srv_sel];
            tube_set(&S->tube, "instance", s->url);
            toast(s->ok ? "Server changed" : "Server changed (it did not answer the test)");
            pop();
        }
        break;
    case VK_SOFT_LEFT:
        t9_reset(&S->t9);
        vx_strlcpy(S->t9.text, "https://", sizeof(S->t9.text));
        S->t9.len = (int)strlen(S->t9.text);
        S->input = INPUT_TUBE_SERVER;
        push(SCR_INPUT);
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

/* Leaving Tube screens: their list / video page goes with them. */
static void tube_screen_closed(enum screen s)
{
    if (s == SCR_TUBE_LIST)
        tube_pop_list(&S->tube);
    else if (s == SCR_TUBE_VIDEO)
        tube_pop_video(&S->tube);
}

static void draw_input(void)
{
    struct nk_rect area = content_rect(1);
    float fy = area.y + 30;
    const char *title = S->input == INPUT_SEARCH ? "Search apps" :
                        S->input == INPUT_TUBE_SEARCH ? "Search videos" :
                        S->input == INPUT_TUBE_SERVER ? "Server address" :
                        S->input == INPUT_SMS ? "Message" :
                        S->input == INPUT_SMS_NUMBER ? "To (number)" : "Wi-Fi password";
    char shown[80];
    int n;

    header(title, t9_mode_label(&S->t9));
    if (S->input == INPUT_WIFI_PSK)
        label(nk_rect(10, area.y + 6, area.w - 20, 20), S->wifi_target, S->fonts.small, T.dim, AL_LEFT);
    if (S->input == INPUT_SMS) {
        char left[32];
        snprintf(left, sizeof(left), "to %s \xc2\xb7 %d left", S->thread_number, 160 - S->t9.len);
        label(nk_rect(10, area.y + 6, area.w - 20, 20), left, S->fonts.small, T.dim, AL_LEFT);
    }
    fill_a(nk_rect(8, fy, area.w - 16, 34), 6, 0x050a12, 230);
    nk_stroke_rect(cv, nk_rect(8, fy, area.w - 16, 34), 6, 1, C(T.accent));
    snprintf(shown, sizeof(shown), "%s|", S->t9.text);
    label(nk_rect(16, fy, area.w - 32, 34), shown, S->fonts.normal, T.ink, AL_LEFT);
    if (S->input == INPUT_SEARCH) {
        int tmp[512];
        char res[32];
        n = store_filter(&S->store, -1, S->t9.text, tmp, 512);
        snprintf(res, sizeof(res), "%d matches", n);
        label(nk_rect(10, fy + 42, area.w - 20, 20), res, S->fonts.small, T.dim, AL_LEFT);
    }
    label(nk_rect(10, area.y + area.h - 46, area.w - 20, 20), "# mode  \xc2\xb7  0 space  \xc2\xb7  C delete",
          S->fonts.small, T.dim, AL_CENTER);
    softkeys(NULL, "OK", "Cancel");
}

static void draw_menu(void)
{
    const char *items[4];
    int n = menu_items(items);
    float w = (float)S->surf->w, mh = n * ROW_H + 36, my = S->surf->h - SK_H - mh;
    const char *title = S->menu == MENU_POWER ? "Power" :
                        S->menu == MENU_APP_OPTIONS ? S->apps.list[S->menu_app].name : "Uninstall app?";

    /* dim the screen underneath */
    nk_fill_rect(cv, nk_rect(0, SB_H, w, S->surf->h - SB_H), 0, nk_rgba(2, 5, 10, 150));
    /* a small "window": title bar with an azure badge, glass body */
    fill_a(nk_rect(4, my - 4, w - 8, mh + 2), 8, 0x0b1320, 245);
    nk_stroke_rect(cv, nk_rect(4, my - 4, w - 8, mh + 2), 8, 1, C(T.border));
    fill(nk_rect(12, my + 9, 9, 9), 2, T.accent);
    label(nk_rect(28, my - 2, w - 40, 32), title, S->fonts.normal, T.text, AL_LEFT);
    fill(nk_rect(4, my + 29, w - 8, 1), 0, T.border);
    for (int i = 0; i < n; i++)
        list_row(nk_rect(0, my + 34 + i * ROW_H, w, ROW_H), i == S->menu_sel, items[i], NULL, NULL, 0);
    softkeys(NULL, "Select", "Cancel");
}

static void draw_screen(enum screen scr)
{
    /* never draw store records that are not loaded */
    if ((scr == SCR_STORE_LIST || scr == SCR_STORE_DETAIL) && S->store.state != STORE_READY)
        scr = SCR_STORE;
    if (scr == SCR_STORE_DETAIL && S->detail_app >= S->store.n_apps)
        scr = SCR_STORE;
    switch (scr) {
    case SCR_HOME: draw_home(); break;
    case SCR_LAUNCHER: draw_launcher(); break;
    case SCR_STORE: draw_store(); break;
    case SCR_STORE_LIST: draw_store_list(); break;
    case SCR_STORE_DETAIL: draw_store_detail(); break;
    case SCR_SETTINGS: draw_settings(); break;
    case SCR_WIFI: draw_wifi(); break;
    case SCR_BT: draw_bt(); break;
    case SCR_CALL: draw_call(); break;
    case SCR_MESSAGES: draw_messages(); break;
    case SCR_THREAD: draw_thread(); break;
    case SCR_SYSINFO: draw_sysinfo(); break;
    case SCR_DIALER: draw_dialer(); break;
    case SCR_CALC: draw_calc(); break;
    case SCR_TUBE: draw_tube(); break;
    case SCR_TUBE_SEARCH: draw_tube_search(); break;
    case SCR_TUBE_LIST: draw_tube_list(); break;
    case SCR_TUBE_VIDEO: draw_tube_video(); break;
    case SCR_TUBE_TEXT: draw_tube_text(); break;
    case SCR_TUBE_PLAYER: draw_tube_player(); break;
    case SCR_TUBE_SETTINGS: draw_tube_settings(); break;
    case SCR_TUBE_SERVERS: draw_tube_servers(); break;
    case SCR_INPUT: draw_input(); break;
    case SCR_MENU: draw_menu(); break;
    }
}

void shell_draw(struct shell *sh)
{
    S = sh;
    if (!nk_begin(&sh->ctx, "vortex", nk_rect(0, 0, (float)sh->surf->w, (float)sh->surf->h),
                  NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND)) {
        nk_end(&sh->ctx);
        return;
    }
    cv = nk_window_get_canvas(&sh->ctx);
    nk_push_scissor(cv, nk_rect(0, 0, (float)sh->surf->w, (float)sh->surf->h));

    wallpaper();
    {
        enum screen under = top() == SCR_MENU && S->depth > 1 ? S->stack[S->depth - 2] : top();
        /* darken the content area of list screens so text stays crisp */
        if (under != SCR_HOME && under != SCR_LAUNCHER)
            fill_a(content_rect(1), 0, 0x05090f, 120);
    }
    if (top() == SCR_MENU && S->depth > 1)
        draw_screen(S->stack[S->depth - 2]);
    draw_screen(top());
    status_bar();
    toast_overlay();
    nk_end(&sh->ctx);
}

/* ---- screens: keys --------------------------------------------------- */

static int move(int *sel, int n, int delta)
{
    int old = *sel;
    if (n <= 0)
        return 0;
    *sel = (*sel + delta + n) % n; /* wrap around like KaiOS lists */
    return *sel != old;
}

static void key_home(enum vx_key k)
{
    if (k == VK_OK || k == VK_UP) {
        S->launcher_sel = 0;
        push(SCR_LAUNCHER);
    } else if (k == VK_SOFT_LEFT) {
        launch_app(0); /* store */
    } else if (k == VK_SOFT_RIGHT) {
        launch_app(2); /* settings */
    } else if ((k >= VK_0 && k <= VK_9) || k == VK_STAR || k == VK_HASH || k == VK_CALL) {
        S->dial[0] = '\0';
        push(SCR_DIALER);
        if (k != VK_CALL) {
            char c = k == VK_STAR ? '*' : k == VK_HASH ? '#' : (char)('0' + (k - VK_0));
            S->dial[0] = c;
            S->dial[1] = '\0';
        }
    }
}

static void key_launcher(enum vx_key k)
{
    int n = S->apps.count;
    switch (k) {
    case VK_LEFT: move(&S->launcher_sel, n, -1); break;
    case VK_RIGHT: move(&S->launcher_sel, n, 1); break;
    case VK_UP:
        if (S->launcher_sel - GRID_COLS >= 0)
            S->launcher_sel -= GRID_COLS;
        break;
    case VK_DOWN:
        if (S->launcher_sel + GRID_COLS < n)
            S->launcher_sel += GRID_COLS;
        else if (S->launcher_sel / GRID_COLS < (n - 1) / GRID_COLS)
            S->launcher_sel = n - 1;
        break;
    case VK_OK: launch_app(S->launcher_sel); break;
    case VK_SOFT_LEFT:
        if (S->apps.list[S->launcher_sel].kind == APP_KAIOS) {
            S->menu_app = S->launcher_sel;
            open_menu(MENU_APP_OPTIONS);
        }
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default:
        if (k >= VK_1 && k <= VK_9) {
            int i = S->launcher_top * GRID_COLS + (k - VK_1);
            if (i < n) {
                S->launcher_sel = i;
                launch_app(i);
            }
        }
        break;
    }
}

static void start_search(void)
{
    t9_reset(&S->t9);
    S->t9.mode = T9_LOWER;
    S->input = INPUT_SEARCH;
    push(SCR_INPUT);
}

static void key_store(enum vx_key k)
{
    struct store *st = &S->store;
    int n = st->n_cats + 2;
    if (k == VK_BACK || k == VK_SOFT_RIGHT) {
        pop();
        return;
    }
    if (k == VK_SOFT_LEFT) {
        store_refresh(st);
        return;
    }
    if (st->state != STORE_READY)
        return;
    switch (k) {
    case VK_UP: move(&S->store_cat_sel, n, -1); break;
    case VK_DOWN: move(&S->store_cat_sel, n, 1); break;
    case VK_OK:
        if (S->store_cat_sel == 0) {
            start_search();
            break;
        }
        S->store_cat = S->store_cat_sel - 2;
        S->store_query[0] = '\0';
        S->list_sel = S->list_top = 0;
        refilter();
        push(SCR_STORE_LIST);
        break;
    default:
        break;
    }
}

static void key_store_list(enum vx_key k)
{
    switch (k) {
    case VK_UP: move(&S->list_sel, S->list_n, -1); break;
    case VK_DOWN: move(&S->list_sel, S->list_n, 1); break;
    case VK_OK:
        if (S->list_n)
            open_detail(S->list_idx[S->list_sel]);
        break;
    case VK_SOFT_LEFT: start_search(); break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

static void key_store_detail(enum vx_key k)
{
    const struct store_app *a = &S->store.apps[S->detail_app];
    int inst = installed_index(a->slug);
    switch (k) {
    case VK_UP: if (S->detail_scroll > 0) S->detail_scroll--; break;
    case VK_DOWN: S->detail_scroll++; break; /* clamped while drawing */
    case VK_OK: install_selected(); break;
    case VK_SOFT_LEFT:
        if (inst >= 0)
            launch_app(inst);
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

static void key_settings(enum vx_key k)
{
    switch (k) {
    case VK_UP: move(&S->settings_sel, SET_COUNT, -1); break;
    case VK_DOWN: move(&S->settings_sel, SET_COUNT, 1); break;
    case VK_LEFT: settings_adjust(-1); break;
    case VK_RIGHT: settings_adjust(1); break;
    case VK_OK: settings_activate(); break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

static void key_bt(enum vx_key k)
{
    switch (k) {
    case VK_UP: move(&S->bt_sel, S->bt_n, -1); break;
    case VK_DOWN: move(&S->bt_sel, S->bt_n, 1); break;
    case VK_SOFT_LEFT: bt_scan(); break;
    case VK_OK:
        if (S->bt_n && S->job == JOB_NONE)
            bt_connect(S->bt_sel);
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

static void key_wifi(enum vx_key k)
{
    switch (k) {
    case VK_UP: move(&S->wifi_sel, S->wifi_n, -1); break;
    case VK_DOWN: move(&S->wifi_sel, S->wifi_n, 1); break;
    case VK_SOFT_LEFT: wifi_scan(); break;
    case VK_OK:
        if (!S->wifi_n || S->job != JOB_NONE)
            break;
        vx_strlcpy(S->wifi_target, S->wifi_ssid[S->wifi_sel], sizeof(S->wifi_target));
        if (!S->wifi_secure[S->wifi_sel]) {
            wifi_connect(S->wifi_target, "");
        } else {
            t9_reset(&S->t9);
            S->input = INPUT_WIFI_PSK;
            push(SCR_INPUT);
        }
        break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

static void key_dialer(enum vx_key k)
{
    size_t len = strlen(S->dial);
    if ((k >= VK_0 && k <= VK_9) || k == VK_STAR || k == VK_HASH) {
        if (len + 1 < sizeof(S->dial)) {
            S->dial[len] = k == VK_STAR ? '*' : k == VK_HASH ? '#' : (char)('0' + (k - VK_0));
            S->dial[len + 1] = '\0';
        }
    } else if (k == VK_BACK && len) {
        S->dial[len - 1] = '\0';
    } else if (k == VK_SOFT_LEFT) {
        S->dial[0] = '\0';
    } else if (k == VK_CALL || k == VK_OK) {
        dial();
    } else if (k == VK_SOFT_RIGHT || k == VK_BACK) {
        pop();
    }
}

static void key_input(enum vx_key k)
{
    if (t9_key(&S->t9, k, S->now))
        return;
    if (k == VK_OK) {
        pop();
        if (S->input == INPUT_SEARCH) {
            vx_strlcpy(S->store_query, S->t9.text, sizeof(S->store_query));
            S->store_cat = -1;
            S->list_sel = S->list_top = 0;
            refilter();
            if (top() != SCR_STORE_LIST)
                push(SCR_STORE_LIST);
        } else if (S->input == INPUT_SMS) {
            send_sms();
        } else if (S->input == INPUT_SMS_NUMBER) {
            if (S->t9.text[0]) {
                char number[24];
                vx_strlcpy(number, S->t9.text, sizeof(number));
                open_thread(number);
                compose();
            }
        } else if (S->input == INPUT_TUBE_SEARCH) {
            char q[64];
            vx_strlcpy(q, S->t9.text, sizeof(q));
            tube_search(q);
        } else if (S->input == INPUT_TUBE_SERVER) {
            char url[96];
            size_t n = vx_strlcpy(url, S->t9.text, sizeof(url));
            while (n > 0 && url[n - 1] == '/')
                url[--n] = '\0';
            if (n > 8 && strstr(url, "://")) {
                tube_set(&S->tube, "instance", url);
                toast("Server changed");
                if (top() == SCR_TUBE_SERVERS)
                    pop();
            } else {
                toast("Type the whole address");
            }
        } else {
            wifi_connect(S->wifi_target, S->t9.text);
        }
    } else if (k == VK_SOFT_RIGHT || k == VK_BACK) {
        pop();
    }
}

static void key_menu(enum vx_key k)
{
    const char *items[4];
    int n = menu_items(items);
    switch (k) {
    case VK_UP: move(&S->menu_sel, n, -1); break;
    case VK_DOWN: move(&S->menu_sel, n, 1); break;
    case VK_OK: menu_select(); break;
    case VK_BACK:
    case VK_SOFT_RIGHT: pop(); break;
    default: break;
    }
}

static void end_short(void)
{
    if (S->call_active) {
        hangup();
        return;
    }
    go_home();
}

int shell_key(struct shell *sh, const struct vx_event *ev)
{
    S = sh;
    sh->last_input = sh->now;

    if (sh->blanked) {
        /* the key that wakes the screen does nothing else */
        if (ev->pressed && !ev->repeat) {
            sh->blanked = 0;
            plat_set_blank(0);
            if (sh->brightness > 0)
                sys_backlight_set(sh->brightness);
            sh->end_down_at = -1;
        }
        return 1;
    }

    /* End: short press = home / close app / hang up, hold = power menu. */
    if (ev->key == VK_END) {
        if (ev->pressed && !ev->repeat) {
            sh->end_down_at = sh->now;
            sh->end_long_fired = 0;
            sh->end_repeats = 0;
            return 0;
        }
        if (sh->end_down_at < 0)
            return 0;
        /* time-based, with a repeat count as a fallback for when events
         * arrive in a burst (slow wakeup, headless tests) */
        if (ev->repeat)
            sh->end_repeats++;
        if (ev->pressed && !sh->end_long_fired &&
            (sh->now - sh->end_down_at >= 900 || sh->end_repeats >= 25)) {
            sh->end_long_fired = 1;
            if (sh->app_pid || tube_video_playing())
                return 0;
            if (top() != SCR_MENU)
                open_menu(MENU_POWER);
            return 1;
        }
        if (!ev->pressed) {
            sh->end_down_at = -1;
            if (sh->end_long_fired)
                return 0;
            if (tube_video_playing()) {
                tube_stop();
                return 1;
            }
            if (sh->app_pid) {
                sh->app_closing = 1;
                proc_kill_group(sh->app_pid, 1500);
                /* shell_tick notices the exit */
                return 0;
            }
            end_short();
            return 1;
        }
        return 0;
    }

    if (tube_video_playing()) {
        if (ev->pressed)
            key_tube_video_playing(ev->key);
        return 0;
    }
    if (sh->app_pid || !ev->pressed)
        return 0;

    switch (top()) {
    case SCR_HOME: key_home(ev->key); break;
    case SCR_LAUNCHER: key_launcher(ev->key); break;
    case SCR_STORE: key_store(ev->key); break;
    case SCR_STORE_LIST: key_store_list(ev->key); break;
    case SCR_STORE_DETAIL: key_store_detail(ev->key); break;
    case SCR_SETTINGS: key_settings(ev->key); break;
    case SCR_WIFI: key_wifi(ev->key); break;
    case SCR_BT: key_bt(ev->key); break;
    case SCR_CALL: key_call(ev->key); break;
    case SCR_MESSAGES: key_messages(ev->key); break;
    case SCR_THREAD: key_thread(ev->key); break;
    case SCR_SYSINFO:
        if (ev->key == VK_BACK || ev->key == VK_SOFT_RIGHT)
            pop();
        break;
    case SCR_DIALER: key_dialer(ev->key); break;
    case SCR_CALC: key_calc(ev->key); break;
    case SCR_TUBE: key_tube(ev->key); break;
    case SCR_TUBE_SEARCH: key_tube_search(ev->key); break;
    case SCR_TUBE_LIST: key_tube_list(ev->key); break;
    case SCR_TUBE_VIDEO: key_tube_video(ev->key); break;
    case SCR_TUBE_TEXT: key_tube_text(ev->key); break;
    case SCR_TUBE_PLAYER: key_tube_player(ev->key); break;
    case SCR_TUBE_SETTINGS: key_tube_settings(ev->key); break;
    case SCR_TUBE_SERVERS: key_tube_servers(ev->key); break;
    case SCR_INPUT: key_input(ev->key); break;
    case SCR_MENU: key_menu(ev->key); break;
    }
    return 1;
}

/* ---- ticks ---------------------------------------------------------- */

static void job_done(int code)
{
    enum job_kind k = S->job;
    S->job = JOB_NONE;
    S->job_pid = 0;
    switch (k) {
    case JOB_INSTALL:
        apps_scan(&S->apps, S->cfg.apps_dir);
        toast(code == 0 ? "Installed" : "Install failed");
        break;
    case JOB_REMOVE:
        apps_scan(&S->apps, S->cfg.apps_dir);
        if (S->launcher_sel >= S->apps.count)
            S->launcher_sel = S->apps.count - 1;
        toast(code == 0 ? "Removed" : "Could not remove app");
        break;
    case JOB_WIFI_SCAN:
        wifi_load_scan();
        if (code != 0)
            toast("Wi-Fi scan failed");
        break;
    case JOB_WIFI_CONNECT:
        toast(code == 0 ? "Connected" : "Could not connect");
        sys_update(&S->sys);
        break;
    case JOB_WIFI_TOGGLE:
        sys_update(&S->sys);
        if (code == 2) {
            toast("No Wi-Fi hardware found");
        } else if (code != 0) {
            toast("Could not switch Wi-Fi");
        } else if (S->sys.wifi_on && !S->sys.wifi_up) {
            /* just switched on and not connected: show the networks */
            push(SCR_WIFI);
            wifi_scan();
        }
        break;
    case JOB_BT_TOGGLE:
        sys_update(&S->sys);
        if (code != 0) {
            toast("Could not switch Bluetooth");
        } else if (S->sys.bt_on) {
            push(SCR_BT);
            bt_scan();
        }
        break;
    case JOB_BT_SCAN:
        bt_load_scan();
        if (code != 0)
            toast("Bluetooth scan failed");
        break;
    case JOB_BT_CONNECT:
        toast(code == 0 ? "Connected" : "Could not connect");
        if (code == 0 && !S->bt_paired[S->bt_sel])
            bt_scan();  /* show it as paired */
        break;
    case JOB_SMS:
        messages_load();
        toast(code == 0 ? "Sent" : "Not sent");
        break;
    case JOB_CALL:
        if (code != 0) {
            S->call_active = 0;
            toast("Call failed (no modem?)");
        }
        break;
    case JOB_NONE:
        break;
    }
}

int shell_tick(struct shell *sh)
{
    static long last_sys, last_min = -1;
    int dirty = 0, code = 0;
    time_t t = time(NULL);

    S = sh;
    /* first: an incoming call must be noticed even while a web app runs */
    modem_poll();
    if (sh->call_state[0] && !strcmp(sh->call_state, "active")) {
        static long last_sec;
        if ((long)t != last_sec) {  /* the call timer */
            last_sec = (long)t;
            dirty = 1;
        }
    }
    /* Tube: mpv, then the helper's downloads */
    if (sh->tube.p.pid > 0) {
        int r = tube_player_poll(&sh->tube, &code);
        if (r == 2) {
            tube_player_gone(code, 0);
            dirty = 1;
        } else if (r == 1 && (top() == SCR_TUBE_PLAYER || top() == SCR_TUBE)) {
            dirty = 1;
        }
    }
    if (tube_poll(&sh->tube) || sh->tube.fetch_pid > 0)
        dirty = 1;
    if (sh->tube.error[0]) {
        vx_strlcpy(sh->tube_err, sh->tube.error, sizeof(sh->tube_err));
        toast(sh->tube.error);
        sh->tube.error[0] = '\0';
        dirty = 1;
    }
    if (shell_app_running(sh) && !sh->app_pid)
        return 0;   /* a video is on: nothing else to draw */
    if (sh->app_pid) {
        if (proc_poll(sh->app_pid, &code) != 0) {
            sh->app_pid = 0;
            plat_resume_display();
            /* It ended by itself, with an error, soon after starting: it
             * never really ran.  The runtime logs why. */
            if (!sh->app_closing && code != 0 && sh->now - sh->app_started < 30000)
                toast("App failed to start");
            sh->force_redraw = 1;
            sh->last_input = sh->now;
            apps_scan(&sh->apps, sh->cfg.apps_dir);
            /* The store records were dropped to make room for the app;
             * bring them back if a store screen is on the stack (same
             * cache file, so the saved indices stay valid). */
            if (sh->store.state != STORE_READY && store_load_cache(&sh->store) != 0)
                while (sh->depth > 1 && (top() == SCR_STORE_LIST || top() == SCR_STORE_DETAIL))
                    pop();
            return 1;
        }
        return 0;
    }
    if (sh->job != JOB_NONE) {
        if (proc_poll(sh->job_pid, &code) != 0)
            job_done(code);
        dirty = 1; /* spinner */
    }
    reap_detached();
    /* Store failed to download (e.g. opened before the network was up):
     * retry by itself every few seconds while its screen is showing and a
     * link is up. */
    if (top() == SCR_STORE && sh->store.state == STORE_ERROR && sh->store.fetch_pid <= 0) {
        static long last_retry;
        if (sh->now - last_retry > 5000) {
            last_retry = sh->now;
            sys_update(&sh->sys);
            if (sh->sys.net_up && store_refresh(&sh->store) == 0)
                dirty = 1;
        }
    }
    if (store_poll(&sh->store)) {
        if (sh->store.error[0] && sh->store.state == STORE_READY)
            toast(sh->store.error);
        if (top() == SCR_STORE_LIST)
            refilter();
        dirty = 1;
    }
    if (sh->store.fetch_pid > 0)
        dirty = 1;
    if (t9_tick(&sh->t9, sh->now))
        dirty = 1;
    if (sh->toast[0] && sh->now >= sh->toast_until) {
        sh->toast[0] = '\0';
        dirty = 1;
    }
    if (t / 60 != last_min) {
        last_min = t / 60;
        dirty = 1;
    }
    if (sh->now - last_sys > (top() == SCR_SYSINFO ? 2000 : 30000)) {
        last_sys = sh->now;
        sys_update(&sh->sys);
        dirty = 1;
    }
    if (!sh->blanked && sh->cfg.screen_timeout > 0 && sh->job == JOB_NONE && !sh->call_active &&
        !(sh->tube.p.pid > 0 && sh->tube.p.video) &&
        sh->now - sh->last_input > sh->cfg.screen_timeout * 1000L) {
        sh->blanked = 1;
        if (sh->brightness > 0)
            sys_backlight_set(0);
        plat_set_blank(1);
        go_home();
    }
    return dirty && !sh->blanked;
}

int shell_timeout(const struct shell *sh)
{
    long ms_to_minute;
    /* an app or a video ending wakes us (SIGCHLD); keys arrive as events */
    if (sh->app_pid || (sh->tube.p.pid > 0 && sh->tube.p.video))
        return 1000;
    if (sh->tube.fetch_pid > 0 || sh->tube.thumbs_pid > 0)
        return 150;
    if (sh->tube.p.pid > 0)  /* read mpv's news; redraw the time */
        return sh->stack[sh->depth - 1] == SCR_TUBE_PLAYER && !sh->blanked ? 500 : 1000;
    if (sh->call_state[0])
        return 500;
    if (sh->job != JOB_NONE || sh->store.fetch_pid > 0)
        return 120;
    if (sh->stack[sh->depth - 1] == SCR_STORE && sh->store.state == STORE_ERROR)
        return 5000; /* automatic store retry */
    if (sh->t9.pending_key)
        return T9_COMMIT_MS / 3;
    if (sh->toast[0] || sh->end_down_at >= 0)
        return 200;
    if (sh->blanked)
        return 60000;
    ms_to_minute = 60000 - (long)(time(NULL) % 60) * 1000;
    /* vortex-modem signals a call or a message (SIGUSR1); this is only
     * the safety net */
    if (sh->modem_seen && ms_to_minute > 30000)
        ms_to_minute = 30000;
    if (sh->cfg.screen_timeout > 0) {
        long blank_in = sh->cfg.screen_timeout * 1000L - (sh->now - sh->last_input);
        if (blank_in < ms_to_minute)
            return blank_in > 0 ? (int)blank_in : 0;
    }
    return (int)ms_to_minute;
}

int shell_app_running(const struct shell *sh)
{
    return sh->app_pid != 0 || (sh->tube.p.pid > 0 && sh->tube.p.video);
}

/* ---- setup ---------------------------------------------------------- */

static void apply_style(struct shell *sh)
{
    struct nk_style *st = &sh->ctx.style;
    nk_style_default(&sh->ctx);
    st->window.fixed_background = nk_style_item_color(C(T.bg));
    st->window.background = C(T.bg);
    st->window.padding = nk_vec2(0, 0);
    st->window.spacing = nk_vec2(0, 0);
    st->window.border = 0;
    st->text.color = C(T.text);
}

int shell_init(struct shell *sh, struct vx_surface *surf, const char *config_path)
{
    static char nk_mem[128 * 1024];
    memset(sh, 0, sizeof(*sh));
    S = sh;
    sh->surf = surf;
    sh->end_down_at = -1;
    vx_config_load(&sh->cfg, config_path);
    if (gfx_load_fonts(sh->cfg.font, sh->cfg.font_size, &sh->fonts) != 0) {
        fprintf(stderr, "vortex: cannot load font %s\n", sh->cfg.font);
        return -1;
    }
    /* Fixed memory: Nuklear never allocates after this. */
    if (!nk_init_fixed(&sh->ctx, nk_mem, sizeof(nk_mem), &sh->fonts.normal->handle))
        return -1;
    apply_style(sh);

    sh->stack[0] = SCR_HOME;
    sh->depth = 1;
    sh->store_cat = -1;
    apps_scan(&sh->apps, sh->cfg.apps_dir);
    store_init(&sh->store, sh->cfg.cache_dir, sh->cfg.store_url);
    snprintf(sh->scan_path, sizeof(sh->scan_path), "%s/wifi-scan.txt", sh->cfg.cache_dir);
    snprintf(sh->bt_scan_path, sizeof(sh->bt_scan_path), "%s/bt-scan.txt", sh->cfg.cache_dir);
    {
        char tube_cache[300];
        snprintf(tube_cache, sizeof(tube_cache), "%s/tube", sh->cfg.cache_dir);
        tube_init(&sh->tube, sh->cfg.tube_cmd, sh->cfg.tube_dir, tube_cache);
    }
    sys_update(&sh->sys);
    sh->brightness = sys_backlight_init(sh->cfg.backlight) == 0 ? sys_backlight_get() : -1;
    if (sh->brightness == 0)
        sh->brightness = 60;
    return 0;
}

void shell_shutdown(struct shell *sh)
{
    if (sh->app_pid)
        proc_kill_group(sh->app_pid, 1000);
    tube_player_stop(&sh->tube);
    tube_reset(&sh->tube);
    tube_drop_thumbs(&sh->tube);
    store_unload(&sh->store);
    apps_free(&sh->apps);
    nk_free(&sh->ctx);
    gfx_shutdown();
}
