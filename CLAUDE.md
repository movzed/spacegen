# SpaceGen — orientation for AI assistants

Realtime 3D VFX engine. Output to Resolume Arena via Syphon/NDI. Notch-class content generator.

## What this project IS

A realtime 3D rendering engine that produces video content through a virtual camera precisely matched to a Blender-authored scene. The output is an RGBA texture stream (Syphon on macOS, NDI cross-platform) consumed by Resolume Arena, which handles timeline playback, projection mapping onto a physical structure, and projector output.

The closest commercial reference is **Notch Builder** — realtime 3D VFX engine for media servers (disguise, Hippotizer, Resolume).

## What this project is NOT

- **Not a projection-mapping tool.** Resolume Arena handles all surface warping/calibration downstream. Do not propose corner-pinning, homography UIs, or G-buffer baking — those solve problems we don't have.
- **Not a generic VJ engine.** The earlier direction (preserved at https://github.com/movzed/spacegen-vj-archive) was layered fullscreen GLSL shaders for stock VJ content. We are not rebuilding that.
- **Not a 2D-only fullscreen-shader pipeline.** Notch-class content requires real triangles, real depth, real perspective cameras.

## Architecture in one paragraph

The user authors a scene in Blender (the physical structure modeled, a camera positioned to match the projector pose, optionally pre-rendered video plates from Cycles). A Blender add-on (`tools/blender_export/`) exports `scene.json` (camera + projection matrix) + `structure.glb` (mesh). SpaceGen loads these on startup. The structure mesh is rendered to depth only — invisible matte object. All realtime effects (volumetric beams, GPU particles, geometry deformation, post FX) render through the same projection/view matrices Blender used. Output goes to a single RGBA framebuffer shared via Syphon (macOS) and NDI (network). Resolume Arena consumes that output, composites it with Blender's pre-rendered plates on its timeline, projection-maps the composite onto the physical structure, and drives the projectors.

## Decisions locked

- **Composition**: hybrid. Blender pre-renders some plates; SpaceGen renders live overlays; Resolume composites.
- **Camera mode**: static. Single fixed pose matching Blender's render camera. No animation, no live operator camera moves in v1.
- **Occlusion**: structure mesh loaded as invisible depth holdout. Effects occlude against it correctly.
- **Coordinates**: Blender's Z-up right-handed all the way through. No matrix fix-ups.
- **Projection matrix**: derived in Blender via `cam.calc_matrix_camera(...)` and exported in `scene.json`. Engine consumes verbatim — never recompute.
- **Output format**: RGBA. Transparent where no effect, opaque where effect.

## Stack

C++17 · GLFW 3.4 · OpenGL 4.1 Core · GLSL 410 · Dear ImGui (docking) · GLM · tinygltf · Syphon-Framework · NDI SDK · CMake 3.25 + FetchContent.

OpenGL 4.1 is the macOS ceiling. Sufficient for everything described unless we hit specific limits (millions of GPU particles, image load/store, advanced compute). Re-evaluate Metal or bgfx only on demonstrated need.

## Engine pieces (planned)

- **Reused from archive**: BPMClock (drift-free + tap tempo), ShaderProgram (hot-reload), RenderTarget pattern, ImGui knob/fader widgets, fullscreen-triangle vertex shader.
- **New**: Scene graph (transforms, lights, cameras, meshes), Camera class (loads projection + view from `scene.json`), Mesh class (glTF via tinygltf, VAO/VBO/EBO), HoldoutPass (depth-only render), layer types (`MeshLayer`, `ParticleLayer` via Transform Feedback, `VolumetricLayer` via raymarching), SyphonServer, NDISender, post-FX node chain.

## Conventions

- No raw owning pointers — `unique_ptr` / `shared_ptr` only.
- Shader hot-reload — edit any `.frag` and changes appear next frame.
- Every OpenGL call wrapped in `GL_CHECK()` in debug builds.
- Single output framebuffer is `RGBA16F` with depth attachment.
- Static camera matrices are uniforms set once at scene load, not updated per frame.

## Repo layout

```
spacegen/
├── engine/                # C++ realtime engine
│   ├── CMakeLists.txt
│   └── src/
│       ├── main.cpp
│       ├── core/          # Window, BPMClock, ShaderProgram (hot-reload), RenderTarget
│       ├── scene/         # Camera, Mesh (glTF), Scene (scene.json + structure.glb loader)
│       ├── layers/        # ILayer, FullscreenLayer, MeshLayer, ParticleLayer, VolumetricLayer
│       ├── fx/            # Post FX chain
│       ├── output/        # SyphonServer, NDISender
│       └── gui/           # ImGui scene tree, per-effect panels
├── tools/
│   └── blender_export/    # Blender add-on (Python)
├── shaders/               # GLSL .frag / .vert files (hot-reloaded)
└── examples/              # Sample Blender scenes + reference exports
```

## First deliverable (in progress)

Blender add-on `tools/blender_export/` — writes `scene.json` + `structure.glb` from a Blender scene with an active camera and selected meshes. Establishes the data contract. See in-conversation design notes (schema, camera math, UI flow) — code not yet written, awaiting user review of the schema.
