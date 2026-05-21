// Single translation unit that provides Metal-cpp implementation symbols.
// Per Apple's Metal-cpp guide: these three macros must be defined in EXACTLY
// ONE .cpp file before including the headers. This is that file.

#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
