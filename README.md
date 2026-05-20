# SpaceGen

Realtime 3D VFX engine. Renders effects through a virtual camera matched to a Blender-authored scene, outputs RGBA via Syphon (macOS) / NDI / capture into Resolume Arena.

Closest reference: **Notch Builder**.

## Status

In design — repo restarted **2026-05-20**.

The previous direction (generic VJ engine with fullscreen GLSL shader layers) is preserved at [movzed/spacegen-vj-archive](https://github.com/movzed/spacegen-vj-archive) for reference.

## Architecture

- **Blender** authors the master scene + the master camera. Optionally pre-renders video plates via Cycles/Eevee.
- **SpaceGen** loads the Blender camera and renders realtime 3D effects through it. Outputs an RGBA frame (transparent where no effect).
- **Resolume Arena** consumes SpaceGen's output via Syphon / NDI / capture card, composites it with the Blender plates on a timeline, projection-maps the composite onto the physical structure, and drives the projectors.

The Blender structure mesh is loaded into SpaceGen as an **invisible matte object** — rendered to depth only, so realtime effects occlude correctly against the structure's silhouette.

## Stack

| Component | Choice |
|---|---|
| Language | C++17 |
| Window/context | GLFW 3.4 |
| Graphics | OpenGL 4.1 Core (macOS ceiling) |
| Shaders | GLSL `#version 410` |
| GUI | Dear ImGui (docking branch) |
| Math | GLM |
| Mesh loading | tinygltf |
| Output | Syphon-Framework (macOS), NDI SDK |
| Build | CMake 3.25+ with FetchContent |

All deps via CMake FetchContent — no manual vendoring.

## First milestone

A Blender add-on (`tools/blender_export/`) writes `scene.json` (camera intrinsics + projection matrix) + `structure.glb` (holdout mesh) from any Blender scene. A minimal engine prototype loads that export and renders a single colored cube at scene origin that visually aligns with a Cycles `preview.png` rendered from the same Blender camera.

Once that alignment is correct, every realtime effect after it is just another shader/mesh in the same coordinate space.

## Repo layout (planned)

```
spacegen/
├── engine/                # C++ realtime engine
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp
│       ├── core/          # Window, BPMClock, ShaderProgram, RenderTarget
│       ├── scene/         # Camera, Mesh (glTF), Scene loader
│       ├── layers/        # ILayer, FullscreenLayer, MeshLayer, ParticleLayer, VolumetricLayer
│       ├── fx/            # Post FX node chain
│       ├── output/        # SyphonServer, NDISender
│       └── gui/           # ImGui scene tree + per-effect panels
├── tools/
│   └── blender_export/    # Blender add-on (Python)
├── shaders/               # GLSL .frag / .vert (hot-reloaded)
└── examples/              # Sample Blender scenes + reference exports
```
