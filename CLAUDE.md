# SpaceGen — orientation for AI assistants

Realtime 3D VFX engine, **native per-platform**. Output to Resolume Arena via Syphon (macOS) / Spout (Windows) / NDI. Notch-class content generator.

**Target platforms (v1)**: macOS on Apple Silicon (M1/M2/M3/M4) — primary, built first. Windows 10/11 x64 on NVIDIA RTX 30+ — fase 2 port. Linux is not a v1 target.

## What this project IS

A realtime 3D rendering engine that produces video content through a virtual camera precisely matched to a Blender-authored scene. The output is an RGBA texture stream (Syphon on macOS, NDI cross-platform) consumed by Resolume Arena, which handles timeline playback, projection mapping onto a physical structure, and projector output.

The closest commercial reference is **Notch Builder** — realtime 3D VFX engine for media servers (disguise, Hippotizer, Resolume).

## What this project is NOT

- **Not a projection-mapping tool.** Resolume Arena handles all surface warping/calibration downstream. Do not propose corner-pinning, homography UIs, or G-buffer baking — those solve problems we don't have.
- **Not a generic VJ engine.** The earlier direction (preserved at https://github.com/movzed/spacegen-vj-archive) was layered fullscreen GLSL shaders for stock VJ content. We are not rebuilding that.
- **Not a 2D-only fullscreen-shader pipeline.** Notch-class content requires real triangles, real depth, real perspective cameras.

## Architecture in one paragraph

The user authors a scene in Blender (the physical structure modeled with PBR materials, a camera positioned to match the projector pose, optionally pre-rendered video plates from Cycles). A Blender add-on (`tools/blender_export/`) exports `scene.json` (camera + projection matrix + structure metadata) + `structure.glb` (mesh + PBR materials). SpaceGen loads these on startup. **The structure mesh is rendered as a real, visibly lit surface** — SpaceGen's lights (Spot / Point / Directional, defined inside SpaceGen, BPM-driven, operator-controlled) illuminate it in realtime with a PBR shading model (GGX + Lambert + Schlick). The same pass writes depth, so additional effects (volumetric beams, GPU particles, geometry deformation, post FX) render in front with depth-test against the structure for correct occlusion. The output is a single RGBA framebuffer where alpha tracks light intensity — **unlit areas stay transparent** so Resolume can composite Blender plates underneath. Resolume Arena consumes the output via Syphon (macOS) / NDI (network), composites with Blender's pre-rendered plates on its timeline, projection-maps the composite onto the physical structure, and drives the projectors.

## Decisions locked

- **Composition**: hybrid. Blender pre-renders some plates; SpaceGen renders the structure with realtime lighting + overlay effects; Resolume composites both.
- **Camera mode**: static. Single fixed pose matching Blender's render camera. No animation, no live operator camera moves in v1.
- **Structure rendering**: the structure mesh is a **real, visibly lit surface** in SpaceGen — rendered every frame with PBR shading driven by SpaceGen's realtime lights. The depth-occlusion role for other effects is a byproduct of the same pass (depth-write on), not a separate "matte" object.
- **Lighting model (v1)**: PBR-light — GGX specular (Trowbridge-Reitz microfacet distribution + Smith geometry), Lambert diffuse, Schlick Fresnel. Metallic-roughness workflow (matches glTF 2.0). Forward shading, ~16 active lights max in one fragment loop.
- **Light types**: Spot (position, direction, cone inner/outer angle, color, intensity, falloff), Point (position, color, intensity, falloff), Directional (direction, color, intensity). All animatable from BPM clock / MIDI / GUI.
- **Materials**: per-mesh PBR from glTF — `baseColorFactor`, `roughnessFactor`, `metallicFactor`, `emissiveFactor`. Read directly from `structure.glb` via tinygltf (Blender's Principled BSDF exports as glTF MetallicRoughness).
- **Coordinates**: Blender's Z-up right-handed all the way through. No matrix fix-ups.
- **Projection matrix**: derived in Blender via `cam.calc_matrix_camera(...)` and exported in `scene.json`. Engine consumes verbatim — never recompute.
- **Output format**: RGBA. Alpha = clamped light contribution. Unlit structure pixels stay transparent; Blender plates show through underneath.

## Stack

**Architectural decision (2026-05-20)**: native graphics API per platform. No cross-platform graphics abstraction (no bgfx, no Vulkan-via-MoltenVK, no OpenGL). Each platform gets its native max-performance API. Reason: the user explicitly rejected a "disappointing common denominator" and is willing to invest in two backends to extract platform peak.

### Common (both platforms)

- **Language**: C++17 (pure C++ — use **Metal-cpp** on Mac, not Objective-C++)
- **Windowing**: GLFW 3.4 (with `GLFW_CLIENT_API = GLFW_NO_API` so GLFW just makes a native window; we attach Metal / D3D12 swapchains ourselves)
- **GUI**: Dear ImGui (docking branch) — uses `imgui_impl_metal` on Mac, `imgui_impl_dx12` on Windows, `imgui_impl_glfw` for input on both
- **Math**: GLM (header-only, fine on both)
- **glTF**: tinygltf (header-only) — produces CPU-side vertex/index/material data that each backend uploads its own way
- **JSON**: nlohmann_json (`scene.json` parsing)
- **Build**: CMake 3.25+ with FetchContent, platform branches via `if(APPLE)` / `if(WIN32)`

### macOS (v1, built first)

- **Render API**: **Metal 3** via **Metal-cpp** (Apple's official C++ headers, no Objective-C/ObjC++ mixing required)
- **Shading language**: **MSL** (Metal Shading Language — C++14-like) in `.metal` files
- **Min target**: macOS 13 Ventura (when Metal-cpp stabilized) — likely bump to 14 Sonoma
- **Texture sharing**: Syphon — uses IOSurface, native to Metal (no GL-to-IOSurface dance)
- **Profiling**: Xcode GPU Frame Capture, Metal Performance HUD, Instruments

### Windows (fase 2, port from Metal)

- **Render API**: **DirectX 12** via the official D3D12 headers (`d3d12.h`, `dxgi1_6.h`, `d3dx12.h`)
- **Shading language**: **HLSL** in `.hlsl` files, compiled with **DXC** (the modern DXIL compiler) at build time
- **Min target**: Windows 10 21H2 + DX12-feature-level 11.0 hardware (RTX 30+ has 12_2 — we have plenty of headroom)
- **Texture sharing**: Spout 2 (D3D12 shared resources)
- **Profiling**: PIX on Windows, NVIDIA Nsight Graphics, RenderDoc

### Shaders — translation strategy

Open decision: write shaders **twice** (`.metal` for Mac + `.hlsl` for Windows, hand-tuned per platform) **or** write once in HLSL and translate to MSL via DXC + SPIRV-Cross. Both are real options; will be decided when we start writing the first shader. Default leaning: **hand-write per platform** for v1 since the shader surface is small (~5-10 shaders for Notch-class core) and the perf control is worth the duplication. Reconsider for v2 if shader count grows.

## Engine pieces

Code is split into **platform-agnostic** (`engine/core/`, `engine/fx/`, `engine/gui/`) and **per-backend** (`engine/backends/metal/`, future `engine/backends/dx12/`). Everything platform-agnostic talks to graphics through an `IRenderer` interface; backends implement it.

- **Reused conceptually from archive** (will be re-implemented on Metal — the GL versions are reference only): BPMClock (drift-free + tap tempo), shader hot-reload pattern, RenderTarget pattern, ImGui knob/fader widgets.
- **Platform-agnostic (`engine/core/`)**:
  - `BPMClock` — pure C++ time math
  - `Window` — thin GLFW wrapper, no graphics API
  - `Scene` — parses `scene.json`, owns Cameras/Meshes/Lights/Materials as CPU-side data
  - `Camera` — projection + view matrices loaded from JSON
  - `Mesh` (CPU-side) — vertex/index buffers from tinygltf, no GPU handles
  - `Material` — PBR struct (baseColor, roughness, metallic, emissive)
  - `Light` — Spot / Point / Directional with animatable params + BPM hooks
  - `ILayer.h` — interface for FX layers
- **Render interface (`engine/render/`)**:
  - `IRenderer.h` — abstract: `init(window)`, `createMesh(MeshData)`, `createPipeline(spec)`, `beginFrame()`, `drawStructure(...)`, `drawLayer(...)`, `endFrame()`, `getSharedTexture()`
  - `RenderTypes.h` — platform-agnostic enums, structs (`PipelineSpec`, `MeshHandle`, `TextureHandle`, etc.)
- **Metal backend (`engine/backends/metal/`)**:
  - `MetalRenderer` — implements IRenderer using Metal-cpp
  - `MetalMesh`, `MetalPipeline`, `MetalCommandBuffer` — wrappers
  - `engine/backends/metal/shaders/` — `.metal` MSL files
  - `StructurePass.metal` — forward PBR shader (GGX + Lambert + Schlick)
- **DX12 backend (`engine/backends/dx12/`, fase 2)**:
  - Mirror structure with `D3D12Renderer`, etc.
  - `engine/backends/dx12/shaders/` — `.hlsl` files
- **Output (`engine/output/`)**:
  - `IOutputSharing.h` — `publish(TextureHandle, w, h, timestamp)`
  - `SyphonOutput.cpp` — Mac, native IOSurface from Metal texture
  - `SpoutOutput.cpp` — Win (fase 2), D3D12 shared resource
  - `NDIOutput.cpp` — universal, GPU→CPU readback + NDI send
- **FX (`engine/fx/`)**: post-FX, particles, volumetrics — each `ILayer` impl that calls into `IRenderer`
- **GUI (`engine/gui/`)**: ImGui panels, platform-agnostic; backend init lives in the renderer impl

## Conventions

- **No raw owning pointers** — `unique_ptr` / `shared_ptr` only (Metal-cpp uses `NS::SharedPtr<T>` for Metal objects; treat the same way).
- **Shader hot-reload** — edit any `.metal` / `.hlsl` and changes appear next frame. Implementation: file watcher → re-compile → swap pipeline state. Compile errors surfaced in ImGui status bar with line numbers.
- **Validation always on in debug** — Metal validation layer (`MTL_DEBUG_LAYER=1`) on Mac; D3D12 debug layer on Windows. Every API call validated; performance overhead taken willingly in debug builds.
- **Single output framebuffer** is `RGBA16Float` with `Depth32Float` attachment.
- **Static camera matrices** are bound once at scene load to a per-frame uniform buffer, not updated per frame.
- **Platform-agnostic code stays in `engine/core/`** — no Metal/DX12 types leak there. Backend code stays under `engine/backends/`.
- **Naming**: backend-specific files prefixed by API (`MetalMesh`, `D3D12Mesh`) and live in their backend folder. Common types (`Mesh`, the CPU-side struct) live in `core/`.

## Repo layout

```
spacegen/
├── engine/
│   ├── CMakeLists.txt                # platform branches
│   ├── main.cpp
│   ├── core/                         # platform-agnostic C++17
│   │   ├── BPMClock.{h,cpp}
│   │   ├── Window.{h,cpp}            # GLFW, no graphics API
│   │   ├── Scene.{h,cpp}             # parses scene.json
│   │   ├── Camera.{h,cpp}
│   │   ├── Mesh.{h,cpp}              # CPU-side glTF data
│   │   ├── Material.{h,cpp}          # PBR struct
│   │   ├── Light.{h,cpp}             # Spot/Point/Directional
│   │   └── ILayer.h                  # FX layer interface
│   ├── render/
│   │   ├── IRenderer.h               # abstract graphics interface
│   │   └── RenderTypes.h             # handles, enums, specs
│   ├── backends/
│   │   ├── metal/                    # v1
│   │   │   ├── MetalRenderer.{h,cpp}
│   │   │   ├── MetalMesh.{h,cpp}
│   │   │   ├── MetalPipeline.{h,cpp}
│   │   │   └── shaders/
│   │   │       ├── structure.metal
│   │   │       ├── particles.metal
│   │   │       └── ...
│   │   └── dx12/                     # fase 2
│   │       ├── D3D12Renderer.{h,cpp}
│   │       └── shaders/*.hlsl
│   ├── output/
│   │   ├── IOutputSharing.h
│   │   ├── SyphonOutput.cpp          # macOS
│   │   ├── SpoutOutput.cpp           # Windows (fase 2)
│   │   └── NDIOutput.cpp             # universal
│   ├── fx/                           # post-FX nodes, particles, volumetrics
│   └── gui/                          # ImGui panels
├── tools/
│   └── blender_export/               # Blender add-on (Python, platform-agnostic)
└── examples/                         # Sample Blender exports for testing
```

Note: no top-level `shaders/` folder anymore — shaders live with their backend since they're API-specific.

## First deliverable (done — `a5a4bef`)

Blender add-on `tools/blender_export/` writes `scene.json` + `structure.glb` (with PBR materials embedded in glTF) + optional `preview.png` from a Blender scene with an active camera and selected meshes. Establishes the data contract.

## Next milestone (Metal-only, macOS)

A minimal Metal-backed engine that:
1. CMake project with FetchContent for GLFW, GLM, tinygltf, nlohmann_json, ImGui (and Metal-cpp downloaded from Apple)
2. GLFW window with `GLFW_NO_API` + `CAMetalLayer` attached
3. Loads `scene.json` (nlohmann_json) → uploads projection + view matrices to a uniform buffer
4. Loads `structure.glb` (tinygltf) → uploads to Metal vertex/index buffers, parses PBR material factors
5. Renders the structure via a forward PBR shader (MSL) + one hard-coded directional "test light"
6. Presents to the swapchain (Syphon / NDI come after)

**Success criterion**: side-by-side comparison with `preview.png` from the same export shows correct camera alignment (same vanishing points, same silhouettes) and recognizable PBR shading on the structure. We're validating geometry + camera + Metal pipeline plumbing, not light parity with Cycles.

## Phases

- **Phase 1 (current)**: Metal/macOS only. Full engine, all effects, Syphon + NDI output. Until Notch-class on Mac.
- **Phase 2**: DX12/Windows port. Implement `D3D12Renderer` behind same `IRenderer` interface. Spout output. Reuse all `engine/core/`, `engine/fx/`, `engine/gui/` code unchanged.
- **Phase 3 (optional, deferred)**: any post-launch feature work, possibly Linux via Vulkan if there's demand.
