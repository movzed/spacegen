# SpaceGen

Realtime 3D VFX engine, **native per-platform**. Renders effects through a virtual camera matched to a Blender-authored scene, outputs RGBA via Syphon (macOS) / Spout (Windows) / NDI into Resolume Arena.

Closest reference: **Notch Builder**.

**Platforms (v1)**: macOS on Apple Silicon (M1/M2/M3/M4) — built first on Metal. Windows 10/11 x64 on NVIDIA RTX 30+ — fase 2 port on DirectX 12.

## Status

In design — repo restarted **2026-05-20**.

The previous direction (generic VJ engine with fullscreen GLSL shader layers) is preserved at [movzed/spacegen-vj-archive](https://github.com/movzed/spacegen-vj-archive) for reference.

## Architecture

- **Blender** authors the master scene with PBR materials + the master camera. Optionally pre-renders video plates via Cycles/Eevee.
- **SpaceGen** loads the Blender camera and renders the structure as a **real, visibly lit surface** illuminated by SpaceGen's realtime lights (PBR forward shading — GGX + Lambert + Schlick), plus overlay effects (particles, volumetric beams, etc.) in front of it with correct depth occlusion. Outputs an RGBA frame where alpha tracks light contribution.
- **Resolume Arena** consumes SpaceGen's output via Syphon / Spout / NDI / capture card, composites it with the Blender pre-rendered plates on a timeline, projection-maps the composite onto the physical structure, and drives the projectors.

## Stack

Native graphics API per platform — no cross-platform graphics abstraction. Each platform gets its peak-performance API.

### Common (both platforms)

| Component | Choice |
|---|---|
| Language | C++17 (pure C++ — Metal-cpp on Mac, not Objective-C++) |
| Windowing | GLFW 3.4 (`GLFW_NO_API` mode — we attach native swapchains) |
| GUI | Dear ImGui (docking) with platform backends |
| Math | GLM |
| Mesh loading | tinygltf |
| JSON | nlohmann_json |
| Output | NDI SDK (universal) + platform-native texture sharing |
| Build | CMake 3.25+ with FetchContent |

### macOS (v1)

| Component | Choice |
|---|---|
| Graphics | Metal 3 via Metal-cpp |
| Shaders | MSL (Metal Shading Language) in `.metal` files |
| Texture sharing | Syphon (native IOSurface from Metal) |
| Min target | macOS 13 Ventura (or higher) |
| Profiling | Xcode GPU Frame Capture, Metal Performance HUD |

### Windows (fase 2)

| Component | Choice |
|---|---|
| Graphics | DirectX 12 |
| Shaders | HLSL compiled with DXC |
| Texture sharing | Spout 2 (D3D12 shared resources) |
| Min target | Windows 10 21H2, DX12 feature-level 11.0+ (RTX 30+ has 12_2) |
| Profiling | PIX on Windows, NVIDIA Nsight Graphics |

All deps via CMake FetchContent — no manual vendoring (except NDI SDK, which has a restrictive license and must be installed by the user).

## Milestones

**M1 — Blender add-on (done — `a5a4bef`)**: `tools/blender_export/` writes `scene.json` (camera + projection matrix) + `structure.glb` (mesh + PBR materials via Principled BSDF → glTF MetallicRoughness) + optional `preview.png` from any Blender scene.

**M2 — Metal engine bootstrap (next, macOS)**: CMake + Metal-cpp + GLFW window with `CAMetalLayer`. Load `scene.json` + `structure.glb`. Forward PBR shader (MSL) renders the structure lit by one hard-coded directional test light. Success criterion: visual alignment against `preview.png` from the same export.

**M3 — Lights + FX library (Metal)**: Spot / Point / Directional lights with realtime params + BPM hooks. Particle system (Metal compute shaders). Volumetric beams. Post-FX chain.

**M4 — Syphon + NDI output (Metal)**: native IOSurface texture share via Syphon. NDI fallback.

**M5 — Windows port (DX12)**: implement `D3D12Renderer` behind the same `IRenderer` interface. Re-translate shaders to HLSL. Spout output. All `engine/core/` and `engine/fx/` code reused unchanged.

## Repo layout (planned)

```
spacegen/
├── engine/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── core/                         # platform-agnostic
│   │   ├── BPMClock, Window (GLFW), Scene (scene.json), Camera,
│   │   │  Mesh (CPU-side glTF), Material (PBR), Light (Spot/Point/Dir),
│   │   │  ILayer.h
│   ├── render/
│   │   ├── IRenderer.h               # abstract graphics interface
│   │   └── RenderTypes.h
│   ├── backends/
│   │   ├── metal/                    # v1 — MetalRenderer + *.metal MSL
│   │   └── dx12/                     # fase 2 — D3D12Renderer + *.hlsl
│   ├── output/                       # SyphonOutput, SpoutOutput, NDIOutput
│   ├── fx/                           # post-FX, particles, volumetrics
│   └── gui/                          # ImGui panels
├── tools/
│   └── blender_export/               # Blender add-on (Python)
└── examples/                         # Sample Blender exports for testing
```
