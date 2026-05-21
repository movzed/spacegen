---
name: data-contract-blender-engine
description: Contrato formal versionado de la pipeline Blender → SpaceGen engine. Schema de scene.json (camera + structure metadata), estructura de structure.glb, convenciones PBR (Principled BSDF ↔ glTF MetallicRoughness), reglas de versionado, validación al cargar, alineación cámara Z-up. Úsala cuando modifiques el add-on, cambies el schema, añadas un campo al contrato, implementes el loader del engine, o debuggees misalignment cámara/geometría.
---

# Data Contract Blender ↔ Engine — el contrato de import

## Por qué existe esta skill

El add-on de Blender (`tools/blender_export/`) y el engine son dos programas distintos en lenguajes distintos, mantenidos potencialmente en momentos distintos. Sin un contrato formal:

- El add-on añade un campo, el engine se rompe silenciosamente al cargar.
- El engine ignora un campo nuevo sin notificar, el operador piensa que la feature funciona.
- La matriz de cámara se calcula en ambos lados y diverge en sutilezas de aspect ratio / pixel aspect / sensor shift.
- Z-up vs Y-up se aplica fix-up en un sitio y se olvida en otro.

Esta skill define el contrato canónico. Add-on y engine ambos apuntan aquí. Cuando cambia, bumpea schema_version.

## File layout del export

Un export es **una carpeta** con archivos relacionados:

```
<output_dir>/
├── scene.json          # manifest. Obligatorio.
├── structure.glb       # mesh + materiales PBR. Obligatorio si scene.json.structure.file != null.
└── preview.png         # Cycles single-frame. Opcional.
```

Una carpeta = una unidad de import. El engine carga la carpeta entera. Re-exportar desde Blender sobreescribe.

## `scene.json` — schema v1

```jsonc
{
  // ---- IDENTIDAD DEL EXPORT ----
  "schema_version": 1,                                        // REQUIRED — engine valida primero
  "exported_at": "2026-05-20T14:32:18Z",                      // ISO 8601 UTC
  "blender_version": "4.5.0",                                 // diagnóstico
  "source_blend": "/abs/path/to/scene.blend",                 // diagnóstico; "<unsaved>" si no se guardó
  "output_resolution": [1920, 1080],                          // [w, h] píxeles — window del engine arranca aquí

  // ---- CÁMARA ----
  "camera": {
    "name": "ProjectionCam",
    "type": "PERSP",                                          // "PERSP" | "ORTHO" | "PANO" — sólo PERSP v1

    // ---- Raw Blender params (DIAGNÓSTICO; engine no recomputa) ----
    "focal_length_mm": 50.0,
    "sensor_width_mm": 36.0,
    "sensor_height_mm": 24.0,
    "sensor_fit": "AUTO",                                     // "AUTO" | "HORIZONTAL" | "VERTICAL"
    "shift_x": 0.0,
    "shift_y": 0.0,
    "clip_start": 0.1,
    "clip_end": 1000.0,
    "fov_x_rad": 0.69111,                                     // == cam.angle_x
    "fov_y_rad": 0.40907,                                     // == cam.angle_y

    // ---- AUTHORITATIVE — engine consume directo ----
    "world_matrix":      [[r00,r01,r02,tx], ... ],            // 4×4 row-major, Blender camera-to-world (Z-up RH)
    "view_matrix":       [[ ... ]],                           // inversa de world_matrix (precomputed by add-on)
    "projection_matrix": [[ ... ]]                            // cam.calc_matrix_camera(depsgraph, x, y, sx, sy) — GL-style clip
  },

  // ---- STRUCTURE (mesh + materiales) ----
  "structure": {
    "file": "structure.glb",                                  // null si no hay structure exportado
    "object_count": 12,
    "vertex_count": 24580,
    "triangle_count": 48916,
    "bbox_world_min": [-5.0, 0.0, -5.0],
    "bbox_world_max": [ 5.0, 10.0, 5.0],
    "objects": [
      {
        "name": "Wall_North",
        "vertex_count": 4082,
        "triangle_count": 7244,
        "material": "M_Concrete"                              // glTF material name; valores PBR viven en el .glb
      }
    ]
  },

  // ---- PREVIEW ----
  "preview": "preview.png"                                     // o null si no se renderizó
}
```

## Field semantics — qué hace el engine con cada uno

| Campo | Engine lo usa para | Engine valida |
|---|---|---|
| `schema_version` | Decidir si carga. Si > major conocido, refuse + log error. | ✅ primero |
| `exported_at` | Log only | ❌ |
| `blender_version` | Log only | ❌ |
| `source_blend` | Log only | ❌ |
| `output_resolution` | Tamaño inicial de ventana + FBO master | ✅ [w,h] enteros > 0 |
| `camera.type` | Sólo PERSP procesado v1 | ✅ refuse si != "PERSP" |
| `camera.projection_matrix` | Uniform de proyección — **consume verbatim** | ✅ 4×4 floats |
| `camera.view_matrix` | Uniform de vista — **consume verbatim** | ✅ 4×4 floats |
| `camera.world_matrix` | Diagnóstico (view_matrix es lo que se usa al render) | ❌ |
| Demás campos cámara (focal, sensor, fov_*, etc.) | Diagnóstico / UI tooltip | ❌ |
| `structure.file` | Path al .glb relativo a la carpeta. null = sin structure | ✅ archivo debe existir si != null |
| `structure.bbox_world_*` | Initial camera framing en UI, debug overlay | ❌ |
| `structure.objects[*]` | Listing en UI Layer/Material panel | ❌ |

## Reglas de versionado

`schema_version` es un entero. Reglas:

- **Añadir campo opcional** (con default si missing) → **NO bump**. Engine ignora campos desconocidos sin error.
- **Renombrar un campo** → **BUMP**. Engine rechaza versiones no entendidas y exige re-export.
- **Eliminar un campo existente** → **BUMP**.
- **Cambiar la semántica de un campo existente** (e.g., units, coordinate system) → **BUMP**.
- **Cambiar el tipo de un campo** (string → int, float → array) → **BUMP**.

Engine carga así:

```cpp
auto j = nlohmann::json::parse(file);
int v = j.at("schema_version").get<int>();

if (v > KNOWN_SCHEMA_VERSION) {
    LOG_ERROR("scene.json schema_version=%d exceeds known=%d. "
              "Re-export with newer engine, or downgrade add-on.", v, KNOWN_SCHEMA_VERSION);
    return std::nullopt;
}

if (v < KNOWN_SCHEMA_VERSION) {
    LOG_WARN("scene.json schema_version=%d is older than known=%d. "
             "Fields may be missing; using defaults.", v, KNOWN_SCHEMA_VERSION);
    // No fail — sigue cargando, fields faltantes toman default.
}
```

Engine soporta `[KNOWN_SCHEMA_VERSION, KNOWN_SCHEMA_VERSION - 1]` (un step atrás). Más viejo → refuse + log.

## Camera math — el contrato más estricto

### Tres reglas inviolables

**1. `projection_matrix` es authoritative.**
El engine NO recomputa la matriz de proyección desde focal/sensor/aspect. La consume verbatim del JSON. Esto evita drift por diferencias sutiles en cómo Blender vs el engine interpretan `sensor_fit = AUTO`, pixel aspect ratio, anamorphic squeeze, etc.

**2. `view_matrix` es authoritative.**
El engine NO invierte `world_matrix` para obtener `view_matrix`. El add-on ya hizo la inversa con la precisión interna de Blender (que es double). Engine consume `view_matrix` directo. `world_matrix` se exporta sólo para diagnóstico.

**3. Coordenadas: Z-up, right-handed, todo.**
El add-on exporta meshes con `export_yup=False`. La cámara está en Z-up. **Toda la matemática del engine asume Z-up RH. Cero matrix fix-ups en el engine.** Si en Blender la cámara mira hacia el norte, en el engine también.

### Cómo el add-on calcula la proyección

```python
# tools/blender_export/__init__.py
depsgraph = context.evaluated_depsgraph_get()
proj = cam_data.calc_matrix_camera(
    depsgraph,
    x       = scene.render.resolution_x,
    y       = scene.render.resolution_y,
    scale_x = scene.render.pixel_aspect_x,
    scale_y = scene.render.pixel_aspect_y,
)
```

Este método de Blender produce una matriz **OpenGL-style** (clip Z en `[-1, 1]`, right-handed). Metal y DX12 usan clip Z en `[0, 1]` por default — el engine **convierte en carga** (no toca el archivo).

### Conversión GL clip → Metal/DX12 clip

```cpp
// engine/scene/Camera.cpp
glm::mat4 convertGLProjToD3DRange(const glm::mat4& glProj) {
    // GL: z_clip in [-w, w]   →   D3D/Metal: z_clip in [0, w]
    // Multiplicar por (1+z)/2 en post — equivalente a esta matriz:
    glm::mat4 fix(1.0f);
    fix[2][2] = 0.5f;
    fix[3][2] = 0.5f;
    return fix * glProj;
}
```

Esto vive en el engine. El add-on siempre exporta GL-style → canónico.

## Mesh contract — `structure.glb`

- **Format**: glTF 2.0 binary (`.glb`), single-file.
- **Coordinate system**: Z-up, right-handed (Blender's native). Add-on usa `export_yup=False`.
- **Triangulated**: glTF requiere triangles. Add-on triangula al exportar.
- **Apply modifiers**: add-on usa `export_apply=True` — modificadores se aplican antes del export.
- **Normals**: incluidos (`export_normals=True`).
- **No animations**: add-on usa `export_animations=False` (static camera contract).
- **No skins / no morph**: same.
- **Materials**: incluidos como glTF MetallicRoughness (siguiente sección).
- **Textures**: si las materials de Blender usan Image Textures conectadas al Principled BSDF, se embebmen en el `.glb`. Materiales procedurales NO se exportan (operador debe bakear antes — fuera del scope del add-on).

### Mesh validation en el engine

```cpp
// engine/scene/StructureLoader.cpp
tinygltf::Model model;
std::string err, warn;

if (!loader.LoadBinaryFromFile(&model, &err, &warn, glbPath)) {
    LOG_ERROR("Failed loading %s: %s", glbPath.c_str(), err.c_str());
    return std::nullopt;
}

for (auto& mesh : model.meshes) {
    for (auto& prim : mesh.primitives) {
        if (prim.mode != TINYGLTF_MODE_TRIANGLES) {
            LOG_WARN("Mesh '%s' prim uses mode %d (not triangles). Skipping.",
                     mesh.name.c_str(), prim.mode);
            continue;
        }
        if (prim.attributes.find("NORMAL") == prim.attributes.end()) {
            LOG_WARN("Mesh '%s' has no NORMAL attribute. Lighting will be flat.",
                     mesh.name.c_str());
        }
        // ... upload to backend (Metal/D3D12) via IRenderer
    }
}
```

## Material contract — PBR via glTF MetallicRoughness

Blender's Principled BSDF mapea **directamente** al glTF MetallicRoughness:

| Blender Principled BSDF | glTF 2.0 `pbrMetallicRoughness` | Engine PBR shader input |
|---|---|---|
| **Base Color** | `baseColorFactor` (vec4) and/or `baseColorTexture` | albedo (linear) |
| **Metallic** | `metallicFactor` (scalar) and/or `metallicRoughnessTexture.B` | metallic 0..1 |
| **Roughness** | `roughnessFactor` (scalar) and/or `metallicRoughnessTexture.G` | roughness 0..1 |
| **Emission Color × Emission Strength** | `emissiveFactor` (vec3) + opt `emissiveTexture` | emission radiance |
| **Normal Map** (via Normal Map node) | `normalTexture` + scale | normal map |
| **Alpha** (input + Alpha Blend mode) | `alphaMode` + `alphaCutoff` | (no relevant for opaque structure v1) |

**El engine NO inventa defaults inteligentes para material parameters faltantes.** Si una material no especifica un campo, se usa el default de glTF spec:
- baseColor = [1, 1, 1, 1] (white)
- metallic = 1.0
- roughness = 1.0
- emissive = [0, 0, 0]

Estos defaults son raramente lo que el operador quiere → log a WARN. **La responsabilidad de configurar PBR razonable está en Blender.**

### Limitaciones v1

NO se exportan:

- Subsurface scattering, Sheen, Clearcoat, Specular, IOR, Transmission (todos del Principled BSDF avanzado — no mapean a glTF Core; KHR_materials_* extensions están fuera del scope v1).
- Animaciones de materials.
- Procedural textures de Blender (bake primero, then export).

## Validación al cargar — checklist canónico

El engine, al cargar un export folder:

1. ✅ Existe `scene.json` en la carpeta. Si no → fail.
2. ✅ `scene.json` parseable como JSON. Si no → fail with file:line.
3. ✅ Campo `schema_version` presente. Si no → fail.
4. ✅ `schema_version` ≤ `KNOWN_SCHEMA_VERSION`. Si no → fail (refuse newer).
5. ✅ `camera.type == "PERSP"`. Si no → fail (v1 limitation).
6. ✅ `camera.projection_matrix` y `camera.view_matrix` son arrays 4×4 de floats. Si no → fail.
7. ⚠️ `structure.file` referenciado existe. Si no → warning + sigue (export sin structure ES válido — useful for camera-only sanity tests).
8. ✅ El `.glb` parsea con tinygltf. Si no → fail.
9. ⚠️ Cada mesh tiene NORMAL attribute. Si no → warning + sigue (lighting será flat).
10. ⚠️ Materiales claramente sin configurar (todo default) → warning (operador no configuró PBR en Blender).

Códigos: ✅ = fail loud, ⚠️ = warn pero sigue.

## Roundtrip testing — receta canónica

Para verificar que el contrato funciona end-to-end:

1. **Escena de test conocida** (`examples/calibration.blend`):
   - Cámara a `(5, -5, 3)` mirando al origin.
   - Una caja unit en el origin con material Principled (baseColor rojo, roughness 0.5, metallic 0).
   - Una flecha unit en la dirección +X (verde, roughness 1).
   - Una flecha unit en la dirección +Y (azul, roughness 1).
   - Una flecha unit en la dirección +Z (blanco, roughness 1).
2. **Export con add-on** → `examples/calibration_export/scene.json` + `structure.glb` + `preview.png`.
3. **Engine carga** `examples/calibration_export/` y renderiza con UN solo directional light apuntando desde camera direction (head-light).
4. **Comparar visualmente** con `examples/calibration_export/preview.png`:
   - Silueta de la caja en el mismo lugar (camera alignment OK).
   - Aspect ratio idéntico (projection matrix OK).
   - Las 3 flechas apuntan a las direcciones correctas (coordinate system OK).
   - Roughness y baseColor reconocibles (material loading OK).
   - **NO buscamos paridad de iluminación con Cycles** — sólo geometría + cámara + materials.

Cuando este test pasa, el contrato funciona. Cuando algo diverge, este test detecta cuál capa rompió (engine vs add-on vs schema).

## Anti-patrones

❌ Recomputar la projection matrix en el engine desde focal/sensor/aspect.
✅ Consume `projection_matrix` del JSON verbatim.

❌ Convertir matrices Z-up → Y-up "porque GLM espera Y-up."
✅ GLM funciona en cualquier convention. Usar Z-up consistentemente. La convention es del mundo, no de la lib.

❌ Ignorar `schema_version` y "try our best" al cargar.
✅ Validar primero. Refuse si major version desconocido.

❌ Hardcoded defaults inteligentes en el engine para material params faltantes.
✅ Defaults son los de glTF spec. Warning en log si claramente inadecuados; nunca asumir intent.

❌ Cambiar un campo de `scene.json` sin bumpear `schema_version`.
✅ Cada cambio que rompe back-compat → bump. Cada add no-breaking → no bump.

❌ Asumir que `preview.png` existe.
✅ Es opcional. Si missing, UI no muestra preview reference. No error.

❌ Inicializar window size con valor hardcodeado (1920×1080) si scene.json no lo dice.
✅ Window size = `scene.json["output_resolution"]`. Si missing (schema viejo), fallback a 1920×1080 con warning.

❌ Engine corrige errores del add-on silenciosamente ("oh, no hay proj matrix, la calculo yo").
✅ Engine refuse y reporta. El error vive en el add-on, no se enmascara.

❌ Asumir que el .glb usa convention default de glTF (Y-up).
✅ El add-on exporta con `export_yup=False`. El loader del engine respeta lo que viene en el archivo.

## Cuándo bumpear el schema — casos reales esperados

- **v2**: añadir export de animation tracks de la cámara (cambia el contrato "static camera").
- **v2**: añadir export de lights de Blender (decisión actual: NO se exportan; podría cambiar si el operador prefiere autoring en Blender).
- **v3**: añadir LOD meshes para structure (geometría adicional vs. la principal).
- **v3**: añadir layer presets exportados de Blender directamente.

Cada bump major. Engine en versión N entiende `[N, N-1]` (compat un step), refuse cualquier cosa más vieja o más nueva.

## Cuándo violar esta skill

Nunca silenciosamente. Si vas a romper el contrato:

1. Documentado en esta skill (actualízala primero).
2. Bump major del `schema_version`.
3. Update del add-on **y** del engine en el mismo commit.
4. Tests del roundtrip (`examples/calibration_export/`) pasan.
5. Mención en `CHANGELOG.md` con qué bumpó y por qué.

Si rompes el contrato sin pasar por estos 5 pasos, has introducido un bug latente que va a aparecer en producción cuando alguien re-exporte una escena vieja.
