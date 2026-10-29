/* vortex-splash: boot logo for the Nokia 6300 4G.
 *
 *   vortex-splash [-m MSGFILE] [-p PIDFILE]     animate on /dev/fb0 until SIGTERM
 *   vortex-splash --ppm OUT.ppm [-s WxH] [-t MS] [-m MSGFILE]
 *                                               render one frame (tests)
 *
 * Started by the initramfs as soon as the panel driver is up; it keeps
 * running across switch_root and is stopped by the vortex-shell service
 * just before the shell draws its first frame.  The optional message file
 * (first-boot progress) is re-read every tick; it is opened once up front
 * so it stays reachable after the initramfs moves /dev.
 *
 * No Nuklear, no allocations after start-up: the logo and a small font are
 * compiled in (splash_data.h, from tools/gen-splash-data.py).
 */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/fb.h>
#include <linux/kd.h>

#include "splash_data.h"

#define BG 0x000000u

static uint32_t *buf;        /* XRGB8888 frame */
static int W = 240, H = 320;

static volatile sig_atomic_t stop;

static void on_term(int sig)
{
    (void)sig;
    stop = 1;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static uint32_t grey(int v)
{
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    return (uint32_t)(v << 16 | v << 8 | v);
}

/* Blend an alpha bitmap in grey level `level` (0..255) onto black. */
static void blit_alpha(const unsigned char *a, int w, int h, int stride, int x0, int y0, int level)
{
    for (int y = 0; y < h; y++) {
        int py = y0 + y;
        if (py < 0 || py >= H)
            continue;
        for (int x = 0; x < w; x++) {
            int px = x0 + x;
            if (px < 0 || px >= W || !a[y * stride + x])
                continue;
            buf[py * W + px] = grey(a[y * stride + x] * level / 255);
        }
    }
}

static int text_width(const char *s, int n)
{
    int w = 0;
    for (int i = 0; i < n; i++) {
        unsigned c = (unsigned char)s[i];
        w += (c >= 32 && c < 127) ? splash_glyphs[c - 32].adv : splash_glyphs[0].adv;
    }
    return w;
}

static void draw_text(const char *s, int n, int y, int level)
{
    int x = (W - text_width(s, n)) / 2;
    for (int i = 0; i < n; i++) {
        unsigned c = (unsigned char)s[i];
        if (c < 32 || c >= 127)
            c = ' ';
        blit_alpha(splash_font_px + splash_glyphs[c - 32].off, splash_glyphs[c - 32].w,
                   SPLASH_FONT_H, splash_glyphs[c - 32].w, x, y, level);
        x += splash_glyphs[c - 32].adv;
    }
}

/* Centre a message, wrapping once at a space if it is too wide. */
static void draw_message(const char *msg, int y)
{
    int n = (int)strlen(msg);
    if (text_width(msg, n) <= W - 16) {
        draw_text(msg, n, y, 150);
        return;
    }
    for (int cut = n - 1; cut > 0; cut--) {
        if (msg[cut] == ' ' && text_width(msg, cut) <= W - 16) {
            draw_text(msg, cut, y, 150);
            draw_text(msg + cut + 1, n - cut - 1, y + SPLASH_FONT_H + 2, 150);
            return;
        }
    }
    draw_text(msg, n, y, 150);
}

static void fill_circle(int cx, int cy, int r, uint32_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r && cx + x >= 0 && cx + x < W && cy + y >= 0 && cy + y < H)
                buf[(cy + y) * W + cx + x] = c;
}

static void render(long t, const char *msg)
{
    int lx = (W - SPLASH_LOGO_W) / 2, ly = (H - SPLASH_LOGO_H) / 2 - 12;
    /* fade the logo in over 600 ms */
    int level = t >= 600 ? 255 : (int)(t * 255 / 600);

    for (int i = 0; i < W * H; i++)
        buf[i] = BG;
    blit_alpha(splash_logo, SPLASH_LOGO_W, SPLASH_LOGO_H, SPLASH_LOGO_W, lx, ly, level);

    /* three dots below the logo; a soft highlight travels across them */
    if (t >= 600) {
        int cy = ly + SPLASH_LOGO_H + 26;
        for (int i = 0; i < 3; i++) {
            long phase = ((t - 600) / 10 + 400 - i * 33) % 100; /* 0..99, 1 s period */
            int d = phase < 50 ? (int)phase : 100 - (int)phase; /* 0..50 */
            fill_circle(W / 2 + (i - 1) * 14, cy, 2, grey(60 + d * 4));
        }
    }
    if (msg && *msg)
        draw_message(msg, H - 52);
}

/* ---- fbdev ---------------------------------------------------------- */

static struct {
    int fd;
    uint8_t *mem;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    uint32_t *shadow;  /* what is on the panel now */
} F = { .fd = -1 };

static int fb_open(void)
{
    const char *dev = getenv("VORTEX_FBDEV");
    F.fd = open(dev ? dev : "/dev/fb0", O_RDWR | O_CLOEXEC);
    if (F.fd < 0 || ioctl(F.fd, FBIOGET_VSCREENINFO, &F.var) < 0 ||
        ioctl(F.fd, FBIOGET_FSCREENINFO, &F.fix) < 0)
        return -1;
    if (F.var.bits_per_pixel != 16 && F.var.bits_per_pixel != 32)
        return -1;
    F.mem = mmap(NULL, F.fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, F.fd, 0);
    if (F.mem == MAP_FAILED)
        return -1;
    W = (int)F.var.xres;
    H = (int)F.var.yres;
    F.shadow = malloc((size_t)W * H * 4);
    if (!F.shadow)
        return -1;
    memset(F.shadow, 0xff, (size_t)W * H * 4);  /* force the first full write */
    return 0;
}

/* Write only the rows that changed: the panel is refreshed over SPI from
 * the pages we dirty, so an idle animation costs almost nothing. */
static void fb_present(void)
{
    const int bpp = (int)F.var.bits_per_pixel / 8, line = (int)F.fix.line_length;
    for (int y = 0; y < H; y++) {
        uint32_t *s = buf + y * W;
        uint8_t *d = F.mem + (size_t)(F.var.yoffset + y) * line + (size_t)F.var.xoffset * bpp;
        if (!memcmp(s, F.shadow + y * W, (size_t)W * 4))
            continue;
        memcpy(F.shadow + y * W, s, (size_t)W * 4);
        for (int x = 0; x < W; x++) {
            uint32_t p = s[x];
            if (bpp == 2)
                ((uint16_t *)d)[x] = (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
            else
                ((uint32_t *)d)[x] = ((p >> 16 & 0xff) << F.var.red.offset) |
                                     ((p >> 8 & 0xff) << F.var.green.offset) |
                                     ((p & 0xff) << F.var.blue.offset);
        }
    }
}

static void read_message(int fd, char *msg, size_t size)
{
    ssize_t n;
    msg[0] = 0;
    if (fd < 0)
        return;
    n = pread(fd, msg, size - 1, 0);
    if (n <= 0)
        return;
    msg[n] = 0;
    msg[strcspn(msg, "\n")] = 0;
}

int main(int argc, char **argv)
{
    const char *ppm = NULL, *msgfile = NULL, *pidfile = NULL;
    long t_fixed = 1500;
    char msg[128];
    int msg_fd = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ppm") && i + 1 < argc)
            ppm = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc)
            msgfile = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc)
            pidfile = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            t_fixed = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc && sscanf(argv[i + 1], "%dx%d", &W, &H) == 2)
            i++;
        else {
            fprintf(stderr, "usage: vortex-splash [-m MSGFILE] [-p PIDFILE] | --ppm OUT [-s WxH] [-t MS]\n");
            return 2;
        }
    }
    if (msgfile) {
        /* create it if needed, so writers later reuse this inode */
        msg_fd = open(msgfile, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    }

    if (ppm) {
        FILE *f;
        buf = calloc((size_t)W * H, 4);
        if (!buf)
            return 1;
        read_message(msg_fd, msg, sizeof(msg));
        render(t_fixed, msg);
        f = fopen(ppm, "wb");
        if (!f)
            return 1;
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) {
            unsigned char rgb[3] = { buf[i] >> 16 & 0xff, buf[i] >> 8 & 0xff, buf[i] & 0xff };
            fwrite(rgb, 1, 3, f);
        }
        return fclose(f) != 0;
    }

    if (fb_open() != 0) {
        perror("vortex-splash: framebuffer");
        return 1;
    }
    buf = calloc((size_t)W * H, 4);
    if (!buf)
        return 1;
    {
        /* keep fbcon from drawing text or a cursor over the logo */
        int tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
        if (tty >= 0)
            ioctl(tty, KDSETMODE, KD_GRAPHICS);
        /* left open on purpose: the mode stays while we hold it */
    }
    if (pidfile) {
        FILE *f = fopen(pidfile, "w");
        if (f) {
            fprintf(f, "%d\n", (int)getpid());
            fclose(f);
        }
    }
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGHUP, SIG_IGN);

    long t0 = now_ms();
    while (!stop) {
        long t = now_ms() - t0;
        read_message(msg_fd, msg, sizeof(msg));
        render(t, msg);
        fb_present();
        usleep(t < 600 ? 40000 : 66000);
    }
    return 0;
}
