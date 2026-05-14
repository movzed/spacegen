cmake_minimum_required(VERSION 3.25)
project(spacegen VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "")
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ── GLFW ───────────────────────────────────────────────────────────────────
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ── Dear ImGui (docking branch) ────────────────────────────────────────────
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC glfw)

# ── GLM ────────────────────────────────────────────────────────────────────
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        0.9.9.8
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(glm)

# ── nlohmann/json (presets) ────────────────────────────────────────────────
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

# ── stb (image loading) ────────────────────────────────────────────────────
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(stb)
add_library(stb_image INTERFACE)
target_include_directories(stb_image INTERFACE ${stb_SOURCE_DIR})

# ── System OpenGL ──────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

# ── Sources ────────────────────────────────────────────────────────────────
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(spacegen ${SOURCES})

target_include_directories(spacegen PRIVATE
    src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

target_link_libraries(spacegen PRIVATE
    glfw
    imgui_lib
    glm::glm
    nlohmann_json::nlohmann_json
    stb_image
    OpenGL::GL)

if(APPLE)
    target_link_libraries(spacegen PRIVATE
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreFoundation")
endif()

# ── Copy shaders to build dir (hot-reload works from build/) ───────────────
add_custom_target(shaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders
        ${CMAKE_CURRENT_BINARY_DIR}/shaders
    COMMENT "Syncing shaders → build/shaders")
add_dependencies(spacegen shaders)

# ── Compiler flags ─────────────────────────────────────────────────────────
target_compile_options(spacegen PRIVATE
    $<$<CONFIG:Debug>:   -g -O0 -Wall -Wextra>
    $<$<CONFIG:Release>: -O3 -DNDEBUG -march=native>)
