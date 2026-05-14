#pragma once

// Single include point for OpenGL on all platforms.
// macOS: system OpenGL framework gives us up to 4.1 Core Profile.
#ifdef __APPLE__
#  ifndef GL_SILENCE_DEPRECATION
#    define GL_SILENCE_DEPRECATION
#  endif
#  include <OpenGL/gl3.h>
#else
#  include <glad/glad.h>   // other platforms use GLAD
#endif

#include <cstdio>

#ifndef NDEBUG
#  define GL_CHECK(stmt)                                                 \
     do {                                                                \
         stmt;                                                           \
         GLenum _err = glGetError();                                     \
         if (_err != GL_NO_ERROR)                                        \
             fprintf(stderr, "GL 0x%04x  %s  [%s:%d]\n",               \
                     _err, #stmt, __FILE__, __LINE__);                  \
     } while (0)
#else
#  define GL_CHECK(stmt) stmt
#endif
