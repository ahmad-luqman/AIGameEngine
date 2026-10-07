// The single translation unit that compiles stb_image's implementation.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
// Rejects headers claiming huge sizes before allocating (a few bytes could otherwise request gigabytes).
// Matches the largest texture size Vulkan devices commonly support.
#define STBI_MAX_DIMENSIONS 16384
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
