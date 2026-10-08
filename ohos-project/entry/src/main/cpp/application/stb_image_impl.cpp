// Single home for the stb_image implementation.
//
// It used to live inside vulkan_sdl.cpp.  That made every other translation
// unit depend on the Vulkan renderer for its decoder symbols -- and it already
// cost us one build: swapping in the minimal triangle renderer (which does not
// use stb at all) failed with `undefined symbol: stbi_load_from_memory`, because
// model_loader.cpp calls into stb but nothing else defined it.
//
// Keeping the implementation in its own TU means a backend only has to bring
// what it actually uses.

#define STBI_NO_STDIO
// The skybox is six JPEGs, and the glTF sample textures are PNGs.  Both
// decoders have to stay enabled: each STBI_ONLY_x only suppresses the matching
// STBI_NO_x, so listing them together keeps exactly these two.
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
// SDL ships a patched stb_image.h: its error path calls SDL_SetError() and the
// HDR path calls pow(), neither of which SDL_stdinc.h brings in.  Both headers
// therefore have to be visible before the implementation is compiled -- it used
// to work only because this code sat behind a dozen other SDL includes.
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_stdinc.h>
#include <cmath>
#include "stb_image.h"
