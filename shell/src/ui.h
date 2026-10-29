#ifndef VX_UI_H
#define VX_UI_H

#include "apps.h"
#include "gfx.h"
#include "store.h"
#include "sysinfo.h"
#include "t9.h"

#include <sys/types.h>

enum screen {
    SCR_HOME, SCR_LAUNCHER,
    SCR_STORE, SCR_STORE_LIST, SCR_STORE_DETAIL,
    SCR_SETTINGS, SCR_WIFI, SCR_SYSINFO, SCR_DIALER, SCR_CALC,
    SCR_INPUT, SCR_MENU,
};

enum job_kind { JOB_NONE, JOB_INSTALL, JOB_REMOVE, JOB_WIFI_SCAN, JOB_WIFI_CONNECT,
                JOB_WIFI_TOGGLE, JOB_CALL };
enum menu_kind { MENU_POWER, MENU_APP_OPTIONS, MENU_CONFIRM_REMOVE };
enum input_kind { INPUT_SEARCH, INPUT_WIFI_PSK };

#define WIFI_MAX 24

struct shell {
    struct vx_config cfg;
    struct vx_fonts fonts;
    struct nk_context ctx;
    struct vx_surface *surf;
    struct vx_apps apps;
    struct store store;
    struct vx_sys sys;
    struct t9 t9;

    enum screen stack[10];
    int depth;

    int launcher_sel, launcher_top;
    int store_cat_sel, store_cat_top;
    int store_cat;                 /* -1 = all */
    char store_query[64];
    int list_idx[512], list_n, list_sel, list_top;
    int detail_app, detail_scroll, detail_lines;
    int settings_sel, settings_top;
    int wifi_sel, wifi_n;
    char wifi_ssid[WIFI_MAX][40];
    int wifi_signal[WIFI_MAX], wifi_secure[WIFI_MAX];
    char wifi_target[40];
    char dial[32];

    /* calculator */
    char calc_cur[24];             /* number being typed / shown */
    char calc_expr[64];            /* "12 × 3 =" line above it */
    double calc_acc;
    char calc_op;                  /* 0 or one of + - * / */
    int calc_fresh;                /* next digit starts a new number */
    int call_active;

    enum menu_kind menu;
    int menu_sel, menu_app;
    enum input_kind input;

    enum job_kind job;
    pid_t job_pid;
    char job_label[48];
    char scan_path[280];

    pid_t app_pid;                 /* running web app, 0 = none */

    char toast[96];
    long toast_until;

    long now;                      /* ms, monotonic */
    long end_down_at;              /* End key held since */
    int end_long_fired;
    int end_repeats;
    int blanked;
    long last_input;
    int brightness;
    int force_redraw;
};

int  shell_init(struct shell *sh, struct vx_surface *surf, const char *config_path);
/* Returns 1 if the screen must be redrawn. */
int  shell_key(struct shell *sh, const struct vx_event *ev);
int  shell_tick(struct shell *sh);
int  shell_timeout(const struct shell *sh);
void shell_draw(struct shell *sh);
int  shell_app_running(const struct shell *sh);
void shell_shutdown(struct shell *sh);

#endif
