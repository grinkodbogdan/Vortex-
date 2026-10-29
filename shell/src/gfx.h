#ifndef VX_GFX_H
#define VX_GFX_H

#include "nk.h"
#include "platform.h"

/* An RGBA image usable with nk_image_ptr().  Pixels are premultiplied-free
 * ARGB8888 (alpha in the top byte). */
struct vx_image {
    int w, h;
    uint32_t px[];
};

struct vx_fonts {
    struct nk_font *small;   /* status bar, soft keys, captions */
    struct nk_font *normal;  /* lists, body text */
    struct nk_font *large;   /* big UI text (Latin-1 only) */
    struct nk_font *clock;   /* home screen clock: digits only */
};

int  gfx_init(struct vx_surface *surf);
/* Bake the TTF at a few sizes into one alpha atlas.  The TTF data and all
 * baking scratch memory are freed before returning. */
int  gfx_load_fonts(const char *ttf_path, int base_size, struct vx_fonts *out);
/* Rasterise the frame's command list.  Returns 0 when the frame is
 * identical to the previous one (nothing drawn, no present needed). */
int  gfx_render(struct nk_context *ctx, struct nk_color clear, int force);
void gfx_shutdown(void);

struct vx_image *gfx_image_new(int w, int h);

#endif
