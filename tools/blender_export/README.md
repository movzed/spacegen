# SpaceGen Export — Blender add-on

Exports a Blender scene + camera into the data contract consumed by the SpaceGen realtime engine.

## Install

Two options:

**As a folder symlink** (recommended during development — edits to the source are picked up on Blender restart):
```bash
ln -s /Users/fpf/Desktop/spacegen/tools/blender_export \
      ~/Library/Application\ Support/Blender/4.5/scripts/addons/spacegen_export
```
Then in Blender: `Edit > Preferences > Add-ons`, search "SpaceGen", enable.

**As a zip** (for distribution):
```bash
cd tools && zip -r spacegen_export.zip blender_export
```
Then in Blender: `Edit > Preferences > Add-ons > Install...` pointing at the zip.

## Use

1. Open the .blend with your scene. Save it (the default output path is relative to the .blend).
2. Make sure the scene has a camera positioned to match the projector pose.
3. Press `N` in the 3D Viewport to open the sidebar. Click the **SpaceGen** tab.
4. *Camera*: leave blank to use the active scene camera, or pick one.
5. *Structure*: leave blank to export all visible meshes, or pick a collection.
6. *Output folder*: where to write the export.
7. *Render preview frame*: optional Cycles/Eevee still through the camera (visual reference).
8. Click **Export to SpaceGen**.

## Output

In the chosen folder:

| File | Purpose |
|---|---|
| `scene.json` | Manifest. Camera intrinsics + view matrix + projection matrix + structure metadata. Authoritative — the engine consumes `projection_matrix` verbatim. |
| `structure.glb` | Selected meshes as glTF 2.0 binary in Blender's Z-up convention (`export_yup=False`). Includes PBR materials (baseColor, roughness, metallic, emissive) read from each mesh's Principled BSDF and exported as glTF MetallicRoughness. Loaded by the engine as a real, visibly lit surface — illuminated by SpaceGen's realtime lights and also serving as the depth occluder for other effects. |
| `preview.png` | Optional — single-frame render through the camera at output resolution. For visual diff against engine output. |

## Schema versioning

`scene.json` carries `schema_version` (currently `1`). Engine refuses to load unknown versions. Additive fields are non-breaking; renames or removals require a version bump.

## Limitations (v0.1)

- Only `PERSP` cameras (panoramic + orthographic not supported)
- Static camera only — no animation export. (Resolume + Blender's animated camera sync is out of scope; SpaceGen renders a single fixed pose.)
- PBR materials only — Principled BSDF nodes export cleanly as glTF MetallicRoughness; non-Principled shaders fall back to a glTF default (mid-gray, roughness 1, non-metal).
- Texture maps (base color / normal / roughness textures) export via glTF if they're connected to the Principled BSDF in Blender. Procedural Blender materials baked to texture are out of scope (bake manually in Blender first).
- No lights export — SpaceGen owns the lights. Lights live inside SpaceGen so they can be animated realtime (BPM-driven, operator-controlled, MIDI-mapped). Blender lights in the scene are ignored by this exporter.
