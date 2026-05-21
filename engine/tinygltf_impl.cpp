// Single translation unit that compiles tinygltf's implementation + STB image.
// Must define these macros in EXACTLY ONE .cpp file before including the header.

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>
