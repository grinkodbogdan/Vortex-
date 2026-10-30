/* Linux fbdev + evdev backend.
 *
 * On the Nokia 6300 4G the panel is driven by the panel-mipi-dbi DRM driver,
 * whose fbdev emulation gives us a 240x320 RGB565 /dev/fb0.  fbdev keeps
 * this backend tiny (no libdrm, no GPU) and lets a web app take over the
 * display through DRM/KMS while the shell sleeps.
 */
#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>

#define MAX_INPUTS 8

static struct {
    int fb_fd;
    uint8_t *fb_mem;
    size_t fb_len;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    int off_x, off_y;      /* where the surface sits inside the fb */
    int released;

    struct vx_surface surf;

    int tty_fd;
    struct pollfd in[MAX_INPUTS];
    int n_in;
    enum vx_key keymap[VX_KEYMAP_SIZE];
} P = { .fb_fd = -1, .tty_fd = -1 };

static volatile sig_atomic_t quit_requested;

static void on_quit_signal(int sig)
{
    (void)sig;
    quit_requested = 1;
}

static int has_keys(int fd)
{
    unsigned long evbits = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0)
        return 0;
    return (evbits >> EV_KEY) & 1;
}

static void open_inputs(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)) && P.n_in < MAX_INPUTS) {
        char path[64];
        int fd;
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (!has_keys(fd)) {
            close(fd);
            continue;
        }
        P.in[P.n_in].fd = fd;
        P.in[P.n_in].events = POLLIN;
        P.n_in++;
    }
    closedir(d);
}

int plat_init(const struct vx_plat_opts *opts, struct vx_surface *out)
{
    const char *dev = getenv("VORTEX_FBDEV");
    struct sigaction sa;
    int w, h;

    if (!dev)
        dev = "/dev/fb0";
    P.fb_fd = open(dev, O_RDWR | O_CLOEXEC);
    if (P.fb_fd < 0) {
        perror(dev);
        return -1;
    }
    if (ioctl(P.fb_fd, FBIOGET_VSCREENINFO, &P.var) < 0 ||
        ioctl(P.fb_fd, FBIOGET_FSCREENINFO, &P.fix) < 0) {
        perror("fbdev ioctl");
        return -1;
    }
    if (P.var.bits_per_pixel != 16 && P.var.bits_per_pixel != 32) {
        fprintf(stderr, "vortex: unsupported fb depth %u\n", P.var.bits_per_pixel);
        return -1;
    }
    P.fb_len = P.fix.smem_len;
    P.fb_mem = mmap(NULL, P.fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, P.fb_fd, 0);
    if (P.fb_mem == MAP_FAILED) {
        perror("fbdev mmap");
        return -1;
    }

    /* Render at the requested size (default: whole panel) and centre it,
     * so the 240x320 UI can be tried on a desktop VT too. */
    w = opts->width > 0 && opts->width < (int)P.var.xres ? opts->width : (int)P.var.xres;
    h = opts->height > 0 && opts->height < (int)P.var.yres ? opts->height : (int)P.var.yres;
    P.off_x = ((int)P.var.xres - w) / 2;
    P.off_y = ((int)P.var.yres - h) / 2;
    P.surf.w = w;
    P.surf.h = h;
    P.surf.stride = w;
    P.surf.pixels = calloc((size_t)w * h, sizeof(uint32_t));
    if (!P.surf.pixels)
        return -1;

    /* Keep fbcon from drawing a cursor/text over us. */
    P.tty_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (P.tty_fd >= 0 && ioctl(P.tty_fd, KDSETMODE, KD_GRAPHICS) < 0) {
        close(P.tty_fd);
        P.tty_fd = -1;
    }

    keymap_defaults(P.keymap);
    if (opts->keymap_path)
        keymap_load(P.keymap, opts->keymap_path);
    open_inputs();
    if (P.n_in == 0)
        fprintf(stderr, "vortex: warning: no key input devices found\n");

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_quit_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    *out = P.surf;
    return 0;
}

static inline uint16_t to565(uint32_t p)
{
    return (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
}

void plat_present(void)
{
    const int bpp = (int)P.var.bits_per_pixel / 8;
    const int line = (int)P.fix.line_length;
    const uint32_t *src = P.surf.pixels;
    uint8_t *base;

    if (P.released)
        return;
    base = P.fb_mem + (size_t)(P.var.yoffset + P.off_y) * line + (size_t)(P.var.xoffset + P.off_x) * bpp;

    if (bpp == 2) {
        for (int y = 0; y < P.surf.h; y++) {
            uint16_t *dst = (uint16_t *)(base + (size_t)y * line);
            const uint32_t *s = src + (size_t)y * P.surf.stride;
            for (int x = 0; x < P.surf.w; x++)
                dst[x] = to565(s[x]);
        }
    } else if (P.var.red.offset == 16 && P.var.blue.offset == 0) {
        for (int y = 0; y < P.surf.h; y++)
            memcpy(base + (size_t)y * line, src + (size_t)y * P.surf.stride,
                   (size_t)P.surf.w * 4);
    } else {
        /* Arbitrary 32-bit channel order. */
        for (int y = 0; y < P.surf.h; y++) {
            uint32_t *dst = (uint32_t *)(base + (size_t)y * line);
            const uint32_t *s = src + (size_t)y * P.surf.stride;
            for (int x = 0; x < P.surf.w; x++) {
                uint32_t p = s[x];
                dst[x] = ((p >> 16 & 0xff) << P.var.red.offset) |
                         ((p >> 8 & 0xff) << P.var.green.offset) |
                         ((p & 0xff) << P.var.blue.offset);
            }
        }
    }
}

static int read_one(int fd, struct vx_event *ev)
{
    struct input_event ie;
    while (read(fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
        if (ie.type != EV_KEY || ie.code >= VX_KEYMAP_SIZE)
            continue;
        if (P.keymap[ie.code] == VK_NONE)
            continue;
        ev->type = VX_EV_KEY;
        ev->key = P.keymap[ie.code];
        ev->pressed = ie.value != 0;
        ev->repeat = ie.value == 2;
        return 1;
    }
    return 0;
}

int plat_wait_event(struct vx_event *ev, int timeout_ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        int left = timeout_ms, r;
        if (quit_requested) {
            ev->type = VX_EV_QUIT;
            return 1;
        }
        /* Drain anything already queued first (one event per call). */
        for (int i = 0; i < P.n_in; i++)
            if (read_one(P.in[i].fd, ev))
                return 1;
        if (timeout_ms >= 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            left = timeout_ms - (int)((now.tv_sec - start.tv_sec) * 1000 +
                                      (now.tv_nsec - start.tv_nsec) / 1000000);
            if (left <= 0)
                return 0;
        }
        r = poll(P.in, (nfds_t)P.n_in, left);
        if (r == 0)
            return 0;
        if (r < 0 && errno != EINTR)
            return 0;
    }
}

void plat_set_blank(int blank)
{
    ioctl(P.fb_fd, FBIOBLANK, blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK);
}

void plat_release_display(void)
{
    /* The VT stays in graphics mode: in text mode the kernel console would
     * draw over the panel whenever the web app's compositor lets go of it. */
    P.released = 1;
}

void plat_resume_display(void)
{
    P.released = 0;
    if (P.tty_fd >= 0)
        ioctl(P.tty_fd, KDSETMODE, KD_GRAPHICS);
    plat_set_blank(0);
}

void plat_shutdown(void)
{
    for (int i = 0; i < P.n_in; i++)
        close(P.in[i].fd);
    if (P.tty_fd >= 0) {
        ioctl(P.tty_fd, KDSETMODE, KD_TEXT);
        close(P.tty_fd);
    }
    if (P.fb_mem && P.fb_mem != MAP_FAILED)
        munmap(P.fb_mem, P.fb_len);
    if (P.fb_fd >= 0)
        close(P.fb_fd);
    free(P.surf.pixels);
}
