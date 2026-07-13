// Single translation unit that compiles the stb single-header implementations.
// stb_image     -> texture loading (renderer/texture.cpp)
// stb_image_write-> screenshots   (renderer/dx12/device.cpp)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
