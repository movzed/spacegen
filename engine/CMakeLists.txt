cmake_minimum_required(VERSION 3.25)
project(spacegen LANGUAGES CXX OBJCXX)

# ---- Compiler standard ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 17)

# ---- Platform gate (v1 is macOS only) ----
if(NOT APPLE)
    message(FATAL_ERROR
        "SpaceGen v1 targets macOS (Apple Silicon) only. "
        "Windows DX12 backend is planned for fase 2. "
        "See CLAUDE.md for the per-platform stack decision.")
endif()

# ---- Default build type ----
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

include(FetchContent)

# ============================================================
# Dependencies
# ============================================================

# ---- GLFW 3.4 (windowing only; we attach Metal ourselves) ----
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glfw)

# ---- Metal-cpp (Apple's official C++ bindings to Metal) ----
# Apple distributes Metal-cpp as a zip download. We fetch it at configure time.
# If the URL ever breaks: download manually from https://developer.apple.com/metal/cpp/
# and extract to ${METAL_CPP_DIR}/metal-cpp.
set(METAL_CPP_DIR "${CMAKE_BINARY_DIR}/metal-cpp-extracted")
if(NOT EXISTS "${METAL_CPP_DIR}/metal-cpp")
    set(METAL_CPP_URL "https://developer.apple.com/metal/cpp/files/metal-cpp_macOS15_iOS18.zip")
    set(METAL_CPP_ZIP "${CMAKE_BINARY_DIR}/metal-cpp.zip")
    message(STATUS "Downloading Metal-cpp from ${METAL_CPP_URL}")
    file(DOWNLOAD "${METAL_CPP_URL}" "${METAL_CPP_ZIP}"
        SHOW_PROGRESS
        STATUS DL_STATUS
        TLS_VERIFY ON
    )
    list(GET DL_STATUS 0 DL_CODE)
    if(NOT DL_CODE EQUAL 0)
        message(FATAL_ERROR
            "Failed to download Metal-cpp from ${METAL_CPP_URL}.\n"
            "HTTP/download status: ${DL_STATUS}\n"
            "Fix: download manually from https://developer.apple.com/metal/cpp/ and "
            "extract the contents to ${METAL_CPP_DIR}/metal-cpp")
    endif()
    message(STATUS "Extracting Metal-cpp to ${METAL_CPP_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${METAL_CPP_ZIP}" DESTINATION "${METAL_CPP_DIR}")
endif()

add_library(metal-cpp INTERFACE)
target_include_directories(metal-cpp INTERFACE "${METAL_CPP_DIR}/metal-cpp")

# ============================================================
# Engine executable
# ============================================================

add_executable(spacegen
    main.cpp
    metal_cpp_impl.cpp
    platform_metal_glfw.mm
)

target_link_libraries(spacegen PRIVATE
    glfw
    metal-cpp
    "-framework Foundation"
    "-framework QuartzCore"
    "-framework Metal"
    "-framework AppKit"
)

target_compile_options(spacegen PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# Useful at config time
message(STATUS "spacegen build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "spacegen target:     macOS (Apple Silicon)")
