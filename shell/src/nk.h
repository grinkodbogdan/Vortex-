/* Nuklear configuration shared by every translation unit.
 *
 * NK_INCLUDE_FONT_BAKING pulls in stb_truetype so we can rasterise a TTF
 * once at start-up into a small alpha atlas; after that only the atlas and
 * glyph tables stay in memory.  We deliberately do NOT define
 * NK_INCLUDE_VERTEX_BUFFER_OUTPUT: the software renderer consumes Nuklear's
 * high-level draw commands directly, which needs far less memory.
 */
#ifndef VX_NK_H
#define VX_NK_H

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_STANDARD_BOOL
#define NK_BUTTON_TRIGGER_ON_RELEASE

#include "../third_party/nuklear/nuklear.h"

#endif
