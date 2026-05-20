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

C++17 · GLFW 3.4 · OpenGL 4.1 Core · GLSL 410 · Dear ImGui (docking) · GLM · tinygltf · Syphon-Framework · NDI SDK · CMake 3.25 + FetchContent.

OpenGL 4.1 is the macOS ceiling. Sufficient for everything described unless we hit specific limits (millions of GPU particles, image load/store, advanced compute). Re-evaluate Metal or bgfx only on demonstrated need.

## Engine pieces (planned)

- **Reused from archive**: BPMClock (drift-free + tap tempo), ShaderProgram (hot-reload), RenderTarget pattern, ImGui knob/fader widgets, fullscreen-triangle vertex shader.
- **New**:
  - Scene graph (transforms, lights, cameras, meshes)
  - Camera class — loads projection + view from `scene.json`
  - Mesh class — glTF + PBR materials via tinygltf, VAO/VBO/EBO
  - Light types — Spot / Point / Directional, with realtime animatable params
  - **StructurePass** — forward PBR rendering of the structure mesh, lit by active lights, writes color + alpha + depth
  - Layer types: `MeshLayer` (additional 3D geometry), `ParticleLayer` (Transform Feedback), `VolumetricLayer` (raymarched beams/fog), `FullscreenLayer` (post-process / 2D shaders)
  - SyphonServer (macOS), NDISender, post-FX node chain

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

## First deliverable (done — `a5a4bef`)

Blender add-on `tools/blender_export/` writes `scene.json` + `structure.glb` (with PBR materials embedded in glTF) + optional `preview.png` from a Blender scene with an active camera and selected meshes. Establishes the data contract.

## Next milestone

A minimal engine that:
1. Opens a window at the export's output resolution
2. Loads `scene.json` → uploads projection + view matrices
3. Loads `structure.glb` via tinygltf — meshes, normals, PBR materials
4. Renders the structure with a PBR forward shader + one hard-coded directional "test light"
5. Outputs to default framebuffer (Syphon / NDI come after)

**Success criterion**: side-by-side comparison with `preview.png` from the same export shows correct camera alignment (same vanishing points, same silhouettes) and recognizable PBR shading on the structure (even if the test light doesn't match Cycles exactly — we're verifying geometry + camera, not light parity).
