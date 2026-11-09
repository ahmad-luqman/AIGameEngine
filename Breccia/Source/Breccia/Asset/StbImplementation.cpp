// The single translation unit that compiles the implementations of stb_image, stb_image_write and stb_image_resize2.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
// Rejects headers claiming huge sizes before allocating (a few bytes could otherwise request gigabytes).
// Matches the largest texture size Vulkan devices commonly support.
#define STBI_MAX_DIMENSIONS 16384
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
// The SIMD sRGB paths index their lookup table through a pointer placed before its start, which is undefined behavior
// (UBSan reports it). The scalar path indexes it directly; the images resized here (icons) are small.
#define STBIR_NO_SIMD
#include <stb_image_resize2.h>
