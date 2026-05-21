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

# ---- GLM (header-only math) ----
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# ---- nlohmann_json (header-only JSON) ----
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- tinygltf (header-only glTF 2.0 loader; we compile its impl in
#                tinygltf_impl.cpp). Bundles stb_image. ----
set(TINYGLTF_HEADER_ONLY ON  CACHE INTERNAL "" FORCE)
set(TINYGLTF_INSTALL     OFF CACHE INTERNAL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE INTERNAL "" FORCE)
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        v2.9.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tinygltf)
# Some tinygltf tags don't expose a proper target; expose include dir ourselves.
if(NOT TARGET tinygltf)
    add_library(tinygltf INTERFACE)
endif()
target_include_directories(tinygltf INTERFACE "${tinygltf_SOURCE_DIR}")

# ---- Dear ImGui (docking branch). Compiled into the spacegen binary
#      directly; no library target. Uses GLFW + Metal backends. ----
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
)

# ============================================================
# Engine executable
# ============================================================

add_executable(spacegen
    main.cpp
    metal_cpp_impl.cpp
    tinygltf_impl.cpp
    platform_metal_glfw.mm
    core/Scene.cpp
    core/Layer.cpp
    core/StructureLayer.cpp
    core/BeamLayer.cpp
    core/DirectionalLightLayer.cpp
    core/AmbientLightLayer.cpp
    backends/metal/MetalRenderer.cpp
    gui/Workstation.mm
    ${IMGUI_SOURCES}
)

target_include_directories(spacegen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

target_link_libraries(spacegen PRIVATE
    glfw
    metal-cpp
    glm
    nlohmann_json::nlohmann_json
    tinygltf
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
