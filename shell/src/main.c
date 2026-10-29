/* vortex-shell: the Vortex home screen, launcher and app store.
 *
 * usage: vortex-shell [-c config] [-s WxH] [-k keymap] [--script KEYS]
 */
#include "gfx.h"
#include "platform.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: vortex-shell [-c CONFIG] [-s WxH] [-k KEYMAP] [--script KEYS|@FILE]\n"
            "       vortex-shell --version\n");
}

int main(int argc, char **argv)
{
    static struct shell sh;
    struct vx_plat_opts opts = { 0 };
    struct vx_surface surf;
    const char *config = "/etc/vortex/vortex.conf";
    int force = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--version")) {
            puts("vortex-shell " VORTEX_VERSION);
            return 0;
        } else if (!strcmp(a, "-c") && v) {
            config = argv[++i];
        } else if (!strcmp(a, "-s") && v) {
            if (sscanf(argv[++i], "%dx%d", &opts.width, &opts.height) != 2) {
                usage();
                return 2;
            }
        } else if (!strcmp(a, "-k") && v) {
            opts.keymap_path = argv[++i];
        } else if (!strcmp(a, "--script") && v) {
            opts.script = argv[++i];
        } else {
            usage();
            return 2;
        }
    }

    {
        /* The keymap path lives in the config; read it before the platform
         * opens its input devices. */
        struct vx_config tmp;
        static char keymap[256];
        vx_config_load(&tmp, config);
        if (!opts.keymap_path) {
            vx_strlcpy(keymap, tmp.keymap, sizeof(keymap));
            opts.keymap_path = keymap;
        }
    }

    if (plat_init(&opts, &surf) != 0)
        return 1;
    gfx_init(&surf);
    if (shell_init(&sh, &surf, config) != 0) {
        plat_shutdown();
        return 1;
    }

    sh.now = now_ms();
    shell_tick(&sh);
    shell_draw(&sh);
    gfx_render(&sh.ctx, nk_rgb(15, 18, 22), 1);
    plat_present();
    force = 0;

    for (;;) {
        struct vx_event ev;
        int got, dirty = 0;

        sh.now = now_ms();
        got = plat_wait_event(&ev, shell_timeout(&sh));
        sh.now = now_ms();
        if (got && ev.type == VX_EV_QUIT)
            break;
        if (got && ev.type == VX_EV_KEY)
            dirty |= shell_key(&sh, &ev);
        dirty |= shell_tick(&sh);

        if (sh.force_redraw) {
            force = 1;
            sh.force_redraw = 0;
        }
        if (shell_app_running(&sh) || sh.blanked || (!dirty && !force))
            continue;
        shell_draw(&sh);
        if (gfx_render(&sh.ctx, nk_rgb(15, 18, 22), force))
            plat_present();
        force = 0;
    }

    shell_shutdown(&sh);
    plat_shutdown();
    return 0;
}
