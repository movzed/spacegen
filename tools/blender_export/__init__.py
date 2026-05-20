"""
SpaceGen Export — Blender add-on.

Exports a scene + camera into the data contract consumed by the SpaceGen
realtime engine.

Output (in the chosen folder):
  scene.json     — manifest with camera intrinsics, extrinsics, and the
                   projection matrix derived from Blender. Authoritative;
                   the engine consumes the projection matrix verbatim.
  structure.glb  — selected meshes exported as glTF 2.0 binary, in Blender's
                   Z-up coordinate system (export_yup=False) so meshes and
                   camera matrices share one convention. PBR materials
                   (baseColor, roughness, metallic, emissive) are embedded
                   from the Principled BSDF nodes of each mesh's material
                   and consumed by SpaceGen as a glTF MetallicRoughness
                   workflow.
  preview.png    — optional single-frame Cycles/Eevee render at the output
                   resolution, for visual diff against engine output.

Install via Edit > Preferences > Add-ons > Install... pointing at this folder
(or zip it first). UI appears in the 3D Viewport sidebar (N) under "SpaceGen".
"""

bl_info = {
    "name": "SpaceGen Export",
    "author": "movzed",
    "version": (0, 1, 0),
    "blender": (4, 0, 0),
    "location": "View3D > Sidebar > SpaceGen",
    "description": "Export scene + camera for the SpaceGen realtime engine",
    "category": "Import-Export",
}

import bpy
import json
import math
import time
from pathlib import Path
from mathutils import Vector


# ---------- Properties ----------

class SPACEGEN_PG_settings(bpy.types.PropertyGroup):
    output_dir: bpy.props.StringProperty(
        name="Output folder",
        description="Directory to write scene.json + structure.glb (created if missing)",
        subtype='DIR_PATH',
        default="//spacegen_export",
    )
    camera: bpy.props.PointerProperty(
        name="Camera",
        description="Camera to export. Falls back to the active scene camera",
        type=bpy.types.Object,
        poll=lambda self, obj: obj.type == 'CAMERA',
    )
    structure_collection: bpy.props.PointerProperty(
        name="Structure",
        description="Collection of meshes to export as the holdout structure. Falls back to all visible meshes",
        type=bpy.types.Collection,
    )
    render_preview: bpy.props.BoolProperty(
        name="Render preview frame",
        description="Render a still through the camera as a visual reference",
        default=True,
    )


# ---------- Helpers ----------

def matrix_to_list(m):
    return [list(row) for row in m]


def collect_meshes(collection):
    """All mesh objects from a collection (recursive) or all visible meshes."""
    if collection is None:
        return [o for o in bpy.context.scene.objects
                if o.type == 'MESH' and o.visible_get()]
    found = []
    def walk(c):
        for o in c.objects:
            if o.type == 'MESH':
                found.append(o)
        for child in c.children:
            walk(child)
    walk(collection)
    return found


def extract_camera_data(cam_obj, scene, depsgraph):
    cam_data = cam_obj.data
    if cam_data.type != 'PERSP':
        raise ValueError(f"Only PERSP cameras supported in v1 (got {cam_data.type})")

    projection = cam_data.calc_matrix_camera(
        depsgraph,
        x=scene.render.resolution_x,
        y=scene.render.resolution_y,
        scale_x=scene.render.pixel_aspect_x,
        scale_y=scene.render.pixel_aspect_y,
    )

    world = cam_obj.matrix_world.copy()
    view = world.inverted()

    return {
        "name": cam_obj.name,
        "type": cam_data.type,
        "focal_length_mm": cam_data.lens,
        "sensor_width_mm": cam_data.sensor_width,
        "sensor_height_mm": cam_data.sensor_height,
        "sensor_fit": cam_data.sensor_fit,
        "shift_x": cam_data.shift_x,
        "shift_y": cam_data.shift_y,
        "clip_start": cam_data.clip_start,
        "clip_end": cam_data.clip_end,
        "fov_x_rad": cam_data.angle_x,
        "fov_y_rad": cam_data.angle_y,
        "world_matrix": matrix_to_list(world),
        "view_matrix": matrix_to_list(view),
        "projection_matrix": matrix_to_list(projection),
    }


def compute_mesh_metadata(mesh_objects, depsgraph):
    object_metas = []
    total_v = 0
    total_t = 0
    bbox_min = Vector((float('inf'),) * 3)
    bbox_max = Vector((float('-inf'),) * 3)

    for obj in mesh_objects:
        eval_obj = obj.evaluated_get(depsgraph)
        mesh = eval_obj.to_mesh()
        try:
            mesh.calc_loop_triangles()
            vcount = len(mesh.vertices)
            tcount = len(mesh.loop_triangles)
            total_v += vcount
            total_t += tcount

            for corner in obj.bound_box:
                wp = obj.matrix_world @ Vector(corner)
                bbox_min.x = min(bbox_min.x, wp.x)
                bbox_min.y = min(bbox_min.y, wp.y)
                bbox_min.z = min(bbox_min.z, wp.z)
                bbox_max.x = max(bbox_max.x, wp.x)
                bbox_max.y = max(bbox_max.y, wp.y)
                bbox_max.z = max(bbox_max.z, wp.z)

            material = obj.active_material.name if obj.active_material else None
            object_metas.append({
                "name": obj.name,
                "vertex_count": vcount,
                "triangle_count": tcount,
                "material": material,
            })
        finally:
            eval_obj.to_mesh_clear()

    has_geometry = total_v > 0
    return {
        "objects": object_metas,
        "vertex_count": total_v,
        "triangle_count": total_t,
        "bbox_world_min": list(bbox_min) if has_geometry else [0.0, 0.0, 0.0],
        "bbox_world_max": list(bbox_max) if has_geometry else [0.0, 0.0, 0.0],
    }


def export_structure_glb(mesh_objects, glb_path, context):
    """Select the given objects, export to GLB, restore prior selection state."""
    prev_selected = list(context.selected_objects)
    prev_active = context.view_layer.objects.active
    try:
        bpy.ops.object.select_all(action='DESELECT')
        for o in mesh_objects:
            o.select_set(True)
        context.view_layer.objects.active = mesh_objects[0]

        bpy.ops.export_scene.gltf(
            filepath=str(glb_path),
            export_format='GLB',
            use_selection=True,
            export_apply=True,
            export_yup=False,                # Z-up — match camera matrices
            export_materials='EXPORT',       # PBR for realtime lighting in SpaceGen
            export_animations=False,
        )
    finally:
        bpy.ops.object.select_all(action='DESELECT')
        for o in prev_selected:
            try:
                o.select_set(True)
            except Exception:
                pass
        if prev_active:
            context.view_layer.objects.active = prev_active


def render_preview_image(cam_obj, scene, preview_path):
    prev_filepath = scene.render.filepath
    prev_format = scene.render.image_settings.file_format
    prev_camera = scene.camera
    try:
        scene.camera = cam_obj
        scene.render.filepath = str(preview_path)
        scene.render.image_settings.file_format = 'PNG'
        bpy.ops.render.render(write_still=True)
        return True
    finally:
        scene.render.filepath = prev_filepath
        scene.render.image_settings.file_format = prev_format
        scene.camera = prev_camera


# ---------- Export operator ----------

class SPACEGEN_OT_export(bpy.types.Operator):
    bl_idname = "spacegen.export"
    bl_label = "Export to SpaceGen"
    bl_options = {'REGISTER'}

    def execute(self, context):
        settings = context.scene.spacegen_settings
        scene = context.scene
        depsgraph = context.evaluated_depsgraph_get()

        out_str = bpy.path.abspath(settings.output_dir)
        if not out_str or out_str.endswith("//"):
            self.report({'ERROR'}, "Output folder unresolved. Save the .blend or pick an absolute path.")
            return {'CANCELLED'}
        out = Path(out_str)
        try:
            out.mkdir(parents=True, exist_ok=True)
        except Exception as e:
            self.report({'ERROR'}, f"Cannot create output directory: {e}")
            return {'CANCELLED'}

        cam = settings.camera or scene.camera
        if not cam or cam.type != 'CAMERA':
            self.report({'ERROR'}, "No camera selected and no active scene camera")
            return {'CANCELLED'}

        meshes = collect_meshes(settings.structure_collection)
        if not meshes:
            self.report({'WARNING'}, "No meshes resolved for structure export — scene.json will reference no glb")

        try:
            cam_data = extract_camera_data(cam, scene, depsgraph)
        except ValueError as e:
            self.report({'ERROR'}, str(e))
            return {'CANCELLED'}

        mesh_meta = compute_mesh_metadata(meshes, depsgraph)

        if meshes:
            try:
                export_structure_glb(meshes, out / "structure.glb", context)
            except Exception as e:
                self.report({'ERROR'}, f"glTF export failed: {e}")
                return {'CANCELLED'}

        preview_filename = None
        if settings.render_preview:
            try:
                if render_preview_image(cam, scene, out / "preview.png"):
                    preview_filename = "preview.png"
            except Exception as e:
                self.report({'WARNING'}, f"Preview render failed: {e}")

        manifest = {
            "schema_version": 1,
            "exported_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "blender_version": ".".join(str(v) for v in bpy.app.version),
            "source_blend": bpy.data.filepath or "<unsaved>",
            "output_resolution": [scene.render.resolution_x, scene.render.resolution_y],
            "camera": cam_data,
            "structure": {
                "file": "structure.glb" if meshes else None,
                "object_count": len(meshes),
                "vertex_count": mesh_meta["vertex_count"],
                "triangle_count": mesh_meta["triangle_count"],
                "bbox_world_min": mesh_meta["bbox_world_min"],
                "bbox_world_max": mesh_meta["bbox_world_max"],
                "objects": mesh_meta["objects"],
            },
            "preview": preview_filename,
        }

        (out / "scene.json").write_text(json.dumps(manifest, indent=2))

        print(f"[SpaceGen] Exported to {out}")
        print(f"[SpaceGen]   Camera: {cam.name}  "
              f"FOV: {math.degrees(cam_data['fov_x_rad']):.2f}° H x "
              f"{math.degrees(cam_data['fov_y_rad']):.2f}° V")
        print(f"[SpaceGen]   Structure: {len(meshes)} obj  "
              f"{mesh_meta['vertex_count']:,} v  "
              f"{mesh_meta['triangle_count']:,} t")
        if preview_filename:
            print(f"[SpaceGen]   Preview: {preview_filename}")

        self.report({'INFO'}, f"Exported to {out}")
        return {'FINISHED'}


# ---------- UI ----------

class SPACEGEN_PT_panel(bpy.types.Panel):
    bl_label = "SpaceGen Export"
    bl_idname = "SPACEGEN_PT_panel"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "SpaceGen"

    def draw(self, context):
        layout = self.layout
        settings = context.scene.spacegen_settings

        col = layout.column(align=True)
        col.label(text="Camera", icon='CAMERA_DATA')
        col.prop(settings, "camera", text="")
        if not settings.camera and context.scene.camera:
            col.label(text=f"-> scene cam: {context.scene.camera.name}", icon='INFO')

        col.separator()
        col.label(text="Structure (holdout)", icon='MESH_DATA')
        col.prop(settings, "structure_collection", text="")
        if not settings.structure_collection:
            col.label(text="-> all visible meshes", icon='INFO')

        col.separator()
        col.label(text="Output", icon='FILE_FOLDER')
        col.prop(settings, "output_dir", text="")

        col.separator()
        col.prop(settings, "render_preview")

        col.separator()
        col.operator("spacegen.export", icon='EXPORT')


# ---------- Register ----------

classes = (
    SPACEGEN_PG_settings,
    SPACEGEN_OT_export,
    SPACEGEN_PT_panel,
)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.spacegen_settings = bpy.props.PointerProperty(type=SPACEGEN_PG_settings)


def unregister():
    del bpy.types.Scene.spacegen_settings
    for c in reversed(classes):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
