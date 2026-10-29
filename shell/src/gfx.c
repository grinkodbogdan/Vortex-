/* Software rasteriser for Nuklear draw commands.
 *
 * Draws straight into the XRGB8888 back buffer.  Shapes are rendered with
 * signed-distance coverage so rounded corners and circles are antialiased
 * without any intermediate buffers; text is blitted from a single A8 glyph
 * atlas.  Total extra memory: the atlas (~100 KB) and nothing per frame.
 */
#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    struct vx_surface *s;
    int cx0, cy0, cx1, cy1;     /* clip rect, x1/y1 exclusive */
    uint8_t *atlas;
    int aw, ah;
    struct nk_font_atlas fa;
    int have_fa;
    uint64_t last_hash;
} G;

int gfx_init(struct vx_surface *surf)
{
    G.s = surf;
    G.cx0 = G.cy0 = 0;
    G.cx1 = surf->w;
    G.cy1 = surf->h;
    return 0;
}

/* ---- pixel helpers --------------------------------------------------- */

static inline uint32_t blend(uint32_t dst, struct nk_color c, unsigned a)
{
    unsigned dr, dg, db;
    if (a >= 255)
        return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
    dr = (dst >> 16) & 0xff;
    dg = (dst >> 8) & 0xff;
    db = dst & 0xff;
    dr += ((c.r - (int)dr) * (int)a + 127) / 255;
    dg += ((c.g - (int)dg) * (int)a + 127) / 255;
    db += ((c.b - (int)db) * (int)a + 127) / 255;
    return (dr << 16) | (dg << 8) | db;
}

static inline void plot(int x, int y, struct nk_color c, unsigned cov)
{
    unsigned a;
    if (x < G.cx0 || x >= G.cx1 || y < G.cy0 || y >= G.cy1 || !cov)
        return;
    a = (c.a * cov + 127) / 255;
    if (a) {
        uint32_t *p = &G.s->pixels[(size_t)y * G.s->stride + x];
        *p = blend(*p, c, a);
    }
}

static void hspan(int x0, int x1, int y, struct nk_color c)
{
    uint32_t *row;
    if (y < G.cy0 || y >= G.cy1)
        return;
    if (x0 < G.cx0) x0 = G.cx0;
    if (x1 > G.cx1) x1 = G.cx1;
    if (x0 >= x1)
        return;
    row = &G.s->pixels[(size_t)y * G.s->stride];
    if (c.a == 255) {
        uint32_t v = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
        for (int x = x0; x < x1; x++)
            row[x] = v;
    } else {
        for (int x = x0; x < x1; x++)
            row[x] = blend(row[x], c, c.a);
    }
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Signed distance from p to a rounded box centred at (cx,cy). */
static float sd_rbox(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - (hw - r);
    float qy = fabsf(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static inline unsigned cov_from_sd(float sd)
{
    return (unsigned)(clampf(0.5f - sd, 0.f, 1.f) * 255.f + 0.5f);
}

/* ---- shapes ----------------------------------------------------------- */

static void fill_rect(float fx, float fy, float fw, float fh, float r, struct nk_color c)
{
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    int x1 = (int)ceilf(fx + fw), y1 = (int)ceilf(fy + fh);
    float hw = fw * .5f, hh = fh * .5f, cx = fx + hw, cy = fy + hh;

    if (c.a == 0 || fw <= 0 || fh <= 0)
        return;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    if (r < 0.5f) {
        for (int y = y0; y < y1; y++)
            hspan(x0, x1, y, c);
        return;
    }
    for (int y = y0; y < y1; y++) {
        int ri = (int)ceilf(r);
        /* Rows away from the corners are plain spans. */
        if (y >= y0 + ri && y < y1 - ri) {
            hspan(x0, x1, y, c);
            continue;
        }
        for (int x = x0; x < x1; x++) {
            if (x >= x0 + ri && x < x1 - ri) {
                hspan(x, x1 - ri, y, c);
                x = x1 - ri - 1;
                continue;
            }
            plot(x, y, c, cov_from_sd(sd_rbox(x + .5f, y + .5f, cx, cy, hw, hh, r)));
        }
    }
}

static void stroke_rect(float fx, float fy, float fw, float fh, float r, float t, struct nk_color c)
{
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    int x1 = (int)ceilf(fx + fw), y1 = (int)ceilf(fy + fh);
    float hw = fw * .5f, hh = fh * .5f, cx = fx + hw, cy = fy + hh;

    if (c.a == 0 || t <= 0)
        return;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float sd = sd_rbox(x + .5f, y + .5f, cx, cy, hw, hh, r);
            /* ring of thickness t just inside the edge */
            float ring = fabsf(sd + t * .5f) - t * .5f;
            plot(x, y, c, cov_from_sd(ring));
            /* skip the hollow interior quickly */
            if (sd < -t - 1 && x > x0 + (int)r + 1 && x < x1 - (int)r - 2 &&
                y > y0 + (int)t + 1 && y < y1 - (int)t - 2)
                x = x1 - (int)ceilf(r + t) - 2;
        }
    }
}

static void ellipse(float fx, float fy, float fw, float fh, float t, struct nk_color c, int filled)
{
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    int x1 = (int)ceilf(fx + fw), y1 = (int)ceilf(fy + fh);
    float rx = fw * .5f, ry = fh * .5f, cx = fx + rx, cy = fy + ry;
    float rm = rx < ry ? rx : ry;
    if (rx <= 0 || ry <= 0)
        return;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float dx = (x + .5f - cx) / rx, dy = (y + .5f - cy) / ry;
            float sd = (sqrtf(dx * dx + dy * dy) - 1.f) * rm;
            if (!filled)
                sd = fabsf(sd + t * .5f) - t * .5f;
            plot(x, y, c, cov_from_sd(sd));
        }
    }
}

static void line(float ax, float ay, float bx, float by, float t, struct nk_color c)
{
    float hw = (t < 1 ? 1 : t) * .5f;
    int x0 = (int)floorf((ax < bx ? ax : bx) - hw - 1), x1 = (int)ceilf((ax > bx ? ax : bx) + hw + 1);
    int y0 = (int)floorf((ay < by ? ay : by) - hw - 1), y1 = (int)ceilf((ay > by ? ay : by) + hw + 1);
    float vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float px = x + .5f - ax, py = y + .5f - ay;
            float h = l2 > 0 ? clampf((px * vx + py * vy) / l2, 0, 1) : 0;
            float dx = px - vx * h, dy = py - vy * h;
            plot(x, y, c, cov_from_sd(sqrtf(dx * dx + dy * dy) - hw));
        }
    }
}

static void fill_poly(const struct nk_vec2i *pts, int n, struct nk_color c)
{
    int miny = 1 << 30, maxy = -(1 << 30);
    if (n < 3)
        return;
    for (int i = 0; i < n; i++) {
        if (pts[i].y < miny) miny = pts[i].y;
        if (pts[i].y > maxy) maxy = pts[i].y;
    }
    /* Even-odd scanline fill; fine for the few small polygons we draw. */
    for (int y = miny; y <= maxy; y++) {
        int xs[16], nx = 0;
        float sy = y + .5f;
        for (int i = 0, j = n - 1; i < n && nx < 16; j = i++) {
            float yi = pts[i].y, yj = pts[j].y;
            if ((yi > sy) != (yj > sy))
                xs[nx++] = (int)lroundf(pts[i].x + (sy - yi) * (pts[j].x - pts[i].x) / (yj - yi));
        }
        for (int a = 1; a < nx; a++)
            for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) {
                int tmp = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = tmp;
            }
        for (int k = 0; k + 1 < nx; k += 2)
            hspan(xs[k], xs[k + 1], y, c);
    }
}

static void stroke_poly(const struct nk_vec2i *pts, int n, float t, struct nk_color c, int closed)
{
    for (int i = 0; i + 1 < n; i++)
        line(pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, t, c);
    if (closed && n > 2)
        line(pts[n - 1].x, pts[n - 1].y, pts[0].x, pts[0].y, t, c);
}

static void rect_multi(float fx, float fy, float fw, float fh, struct nk_color l, struct nk_color t,
                       struct nk_color r, struct nk_color b)
{
    /* Nuklear passes the four corner colours as left=TL, top=TR,
     * right=BR, bottom=BL. */
    int x0 = (int)fx, y0 = (int)fy, x1 = (int)(fx + fw), y1 = (int)(fy + fh);
    for (int y = y0; y < y1; y++) {
        float v = fh > 1 ? (y - y0) / (fh - 1) : 0;
        for (int x = x0; x < x1; x++) {
            float u = fw > 1 ? (x - x0) / (fw - 1) : 0;
            struct nk_color c;
            float w0 = (1 - u) * (1 - v), w1 = u * (1 - v), w2 = u * v, w3 = (1 - u) * v;
            c.r = (nk_byte)(l.r * w0 + t.r * w1 + r.r * w2 + b.r * w3);
            c.g = (nk_byte)(l.g * w0 + t.g * w1 + r.g * w2 + b.g * w3);
            c.b = (nk_byte)(l.b * w0 + t.b * w1 + r.b * w2 + b.b * w3);
            c.a = (nk_byte)(l.a * w0 + t.a * w1 + r.a * w2 + b.a * w3);
            plot(x, y, c, 255);
        }
    }
}

/* ---- text & images ---------------------------------------------------- */

static void text(const struct nk_user_font *uf, float x, float y, const char *s, int len,
                 float height, struct nk_color fg)
{
    const struct nk_font *font = (const struct nk_font *)uf->userdata.ptr;
    float scale = height / font->info.height;
    int i = 0;
    while (i < len) {
        nk_rune cp;
        int n = nk_utf_decode(s + i, &cp, len - i);
        const struct nk_font_glyph *g;
        int gx, gy, gw, gh, u0, v0;
        if (!n || cp == NK_UTF_INVALID)
            break;
        i += n;
        g = nk_font_find_glyph(font, cp);
        if (!g)
            continue;
        gx = (int)lroundf(x + g->x0 * scale);
        gy = (int)lroundf(y + g->y0 * scale);
        gw = (int)lroundf((g->x1 - g->x0) * scale);
        gh = (int)lroundf((g->y1 - g->y0) * scale);
        u0 = (int)lroundf(g->u0 * G.aw);
        v0 = (int)lroundf(g->v0 * G.ah);
        if (scale == 1.f) {
            for (int yy = 0; yy < gh; yy++) {
                const uint8_t *src = G.atlas + (size_t)(v0 + yy) * G.aw + u0;
                for (int xx = 0; xx < gw; xx++)
                    if (src[xx])
                        plot(gx + xx, gy + yy, fg, src[xx]);
            }
        } else {
            int sw = (int)lroundf((g->u1 - g->u0) * G.aw), sh = (int)lroundf((g->v1 - g->v0) * G.ah);
            for (int yy = 0; yy < gh; yy++) {
                const uint8_t *src = G.atlas + (size_t)(v0 + yy * sh / (gh ? gh : 1)) * G.aw + u0;
                for (int xx = 0; xx < gw; xx++) {
                    uint8_t a = src[xx * sw / (gw ? gw : 1)];
                    if (a)
                        plot(gx + xx, gy + yy, fg, a);
                }
            }
        }
        x += g->xadvance * scale;
    }
}

static void image(const struct nk_command_image *q)
{
    const struct vx_image *im = q->img.handle.ptr;
    if (!im || q->w == 0 || q->h == 0)
        return;
    for (int y = 0; y < q->h; y++) {
        const uint32_t *row = im->px + (size_t)(y * im->h / q->h) * im->w;
        for (int x = 0; x < q->w; x++) {
            uint32_t p = row[x * im->w / q->w];
            struct nk_color c = { (nk_byte)(p >> 16), (nk_byte)(p >> 8), (nk_byte)p, 255 };
            unsigned a = (p >> 24) * q->col.a / 255;
            c.r = (nk_byte)(c.r * q->col.r / 255);
            c.g = (nk_byte)(c.g * q->col.g / 255);
            c.b = (nk_byte)(c.b * q->col.b / 255);
            plot(q->x + x, q->y + y, c, a);
        }
    }
}

struct vx_image *gfx_image_new(int w, int h)
{
    struct vx_image *im = malloc(sizeof(*im) + (size_t)w * h * sizeof(uint32_t));
    if (im) {
        im->w = w;
        im->h = h;
    }
    return im;
}

/* ---- fonts ------------------------------------------------------------ */

int gfx_load_fonts(const char *ttf_path, int base, struct vx_fonts *out)
{
    /* Body text: Latin-1, Latin Extended-A and Cyrillic cover the names
     * and descriptions of nearly every app in the BananaHackers store.
     * The other sizes only show our own UI strings.  Nuklear doubles the
     * atlas width above 1000 glyphs, so the total stays just under that
     * (atlas: 512 px wide instead of 1024). */
    static const nk_rune text_ranges[] = {
        0x0020, 0x00FF, 0x0100, 0x017F, 0x0400, 0x045F,
        0x2010, 0x2027, 0x20AC, 0x20AC, 0x2190, 0x2193, 0,
    };
    static const nk_rune ui_ranges[] = { 0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0 };
    static const nk_rune clock_ranges[] = { 0x0020, 0x0020, 0x0030, 0x003A, 0 };
    struct { struct nk_font **slot; float size; const nk_rune *range; } want[] = {
        { &out->small, base - 3.f, ui_ranges },
        { &out->normal, (float)base, text_ranges },
        { &out->large, base + 4.f, ui_ranges },
        { &out->clock, base * 3.f, clock_ranges },
    };
    size_t ttf_len;
    char *ttf = vx_read_file(ttf_path, &ttf_len, 8u << 20);
    const void *px;

    if (!ttf) {
        perror(ttf_path);
        return -1;
    }
    nk_font_atlas_init_default(&G.fa);
    nk_font_atlas_begin(&G.fa);
    for (size_t i = 0; i < VX_ARRAY_LEN(want); i++) {
        struct nk_font_config cfg = nk_font_config(want[i].size);
        cfg.ttf_blob = ttf;
        cfg.ttf_size = ttf_len;
        /* "owned" = use our buffer as-is; 0 would make one copy per font */
        cfg.ttf_data_owned_by_atlas = 1;
        cfg.range = want[i].range;
        cfg.oversample_h = cfg.oversample_v = 1;
        cfg.pixel_snap = 1;
        *want[i].slot = nk_font_atlas_add(&G.fa, &cfg);
        if (!*want[i].slot) {
            free(ttf);
            return -1;
        }
    }
    px = nk_font_atlas_bake(&G.fa, &G.aw, &G.ah, NK_FONT_ATLAS_ALPHA8);
    if (!px) {
        free(ttf);
        return -1;
    }
    G.atlas = malloc((size_t)G.aw * G.ah);
    if (!G.atlas) {
        free(ttf);
        return -1;
    }
    memcpy(G.atlas, px, (size_t)G.aw * G.ah);
    fprintf(stderr, "vortex: font atlas %dx%d (%d KB)\n", G.aw, G.ah, G.aw * G.ah / 1024);
    /* end() frees the atlas' own pixel copy; the shared ttf blob we free
     * here.  nk_font_atlas_cleanup() is not used because it would free
     * that blob once per font. */
    nk_font_atlas_end(&G.fa, nk_handle_ptr(G.atlas), NULL);
    free(ttf);
    for (struct nk_font_config *c = G.fa.config; c; c = c->next)
        c->ttf_blob = NULL;
    G.have_fa = 1;
    return 0;
}

/* ---- frame ------------------------------------------------------------ */

static uint64_t hash_commands(struct nk_context *ctx)
{
    const unsigned char *p = nk_buffer_memory_const(&ctx->memory);
    nk_size n = nk_buffer_total(&ctx->memory);
    uint64_t h = 1469598103934665603ull;
    for (nk_size i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

int gfx_render(struct nk_context *ctx, struct nk_color clear, int force)
{
    const struct nk_command *cmd;
    uint64_t h = hash_commands(ctx);

    if (!force && h == G.last_hash) {
        nk_clear(ctx);
        return 0;
    }
    G.last_hash = h;
    G.cx0 = G.cy0 = 0;
    G.cx1 = G.s->w;
    G.cy1 = G.s->h;
    for (int y = 0; y < G.s->h; y++)
        hspan(0, G.s->w, y, (struct nk_color){ clear.r, clear.g, clear.b, 255 });

    nk_foreach(cmd, ctx) {
        switch (cmd->type) {
        case NK_COMMAND_SCISSOR: {
            const struct nk_command_scissor *s = (const void *)cmd;
            G.cx0 = s->x < 0 ? 0 : s->x;
            G.cy0 = s->y < 0 ? 0 : s->y;
            G.cx1 = s->x + s->w > G.s->w ? G.s->w : s->x + s->w;
            G.cy1 = s->y + s->h > G.s->h ? G.s->h : s->y + s->h;
        } break;
        case NK_COMMAND_LINE: {
            const struct nk_command_line *l = (const void *)cmd;
            line(l->begin.x, l->begin.y, l->end.x, l->end.y, l->line_thickness, l->color);
        } break;
        case NK_COMMAND_RECT: {
            const struct nk_command_rect *r = (const void *)cmd;
            stroke_rect(r->x, r->y, r->w, r->h, r->rounding, r->line_thickness, r->color);
        } break;
        case NK_COMMAND_RECT_FILLED: {
            const struct nk_command_rect_filled *r = (const void *)cmd;
            fill_rect(r->x, r->y, r->w, r->h, r->rounding, r->color);
        } break;
        case NK_COMMAND_RECT_MULTI_COLOR: {
            const struct nk_command_rect_multi_color *r = (const void *)cmd;
            rect_multi(r->x, r->y, r->w, r->h, r->left, r->top, r->right, r->bottom);
        } break;
        case NK_COMMAND_CIRCLE: {
            const struct nk_command_circle *c = (const void *)cmd;
            ellipse(c->x, c->y, c->w, c->h, c->line_thickness, c->color, 0);
        } break;
        case NK_COMMAND_CIRCLE_FILLED: {
            const struct nk_command_circle_filled *c = (const void *)cmd;
            ellipse(c->x, c->y, c->w, c->h, 0, c->color, 1);
        } break;
        case NK_COMMAND_TRIANGLE: {
            const struct nk_command_triangle *t = (const void *)cmd;
            struct nk_vec2i p[3] = { t->a, t->b, t->c };
            stroke_poly(p, 3, t->line_thickness, t->color, 1);
        } break;
        case NK_COMMAND_TRIANGLE_FILLED: {
            const struct nk_command_triangle_filled *t = (const void *)cmd;
            struct nk_vec2i p[3] = { t->a, t->b, t->c };
            fill_poly(p, 3, t->color);
        } break;
        case NK_COMMAND_POLYGON: {
            const struct nk_command_polygon *p = (const void *)cmd;
            stroke_poly(p->points, p->point_count, p->line_thickness, p->color, 1);
        } break;
        case NK_COMMAND_POLYGON_FILLED: {
            const struct nk_command_polygon_filled *p = (const void *)cmd;
            fill_poly(p->points, p->point_count, p->color);
        } break;
        case NK_COMMAND_POLYLINE: {
            const struct nk_command_polyline *p = (const void *)cmd;
            stroke_poly(p->points, p->point_count, p->line_thickness, p->color, 0);
        } break;
        case NK_COMMAND_TEXT: {
            const struct nk_command_text *t = (const void *)cmd;
            text(t->font, t->x, t->y, t->string, t->length, t->height, t->foreground);
        } break;
        case NK_COMMAND_IMAGE:
            image((const void *)cmd);
            break;
        default:
            break; /* curves/arcs are not used by the shell */
        }
    }
    nk_clear(ctx);
    return 1;
}

void gfx_shutdown(void)
{
    if (G.have_fa) {
        nk_font_atlas_clear(&G.fa);
        G.have_fa = 0;
    }
    free(G.atlas);
    G.atlas = NULL;
}
