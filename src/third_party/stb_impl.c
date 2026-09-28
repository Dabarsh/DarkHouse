/* DarkHouse — the single translation unit holding the stb implementations.
 * Decoding always goes through stbi_*_from_memory, so stdio is compiled out of
 * stb_image. stb_image_write (used by tests) keeps it: its HDR writer is only
 * compiled together with the stdio variants. */
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
