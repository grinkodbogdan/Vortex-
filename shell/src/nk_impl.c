/* The one translation unit that compiles Nuklear and stb_image. */
#define NK_IMPLEMENTATION
#include "nk.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../third_party/stb/stb_image.h"
