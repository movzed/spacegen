---
name: layer-stack-architecture-spacegen
description: Semántica del stack de layers en SpaceGen — tipos (Generator/Effect/Group), estados (Disabled/Bypassed/Enabled, Mute/Solo), buses (Master/Preview/Aux), render order, propagación de alpha (premultiplied), blend modes. Úsala cuando añadas un nuevo tipo de layer, diseñes la jerarquía de la escena, implementes el render pipeline, escribas la UI del layer rack, o decidas cómo se compone una nueva feature.
---

# Layer Stack Architecture — el modelo de composición

## Por qué existe esta skill

SpaceGen es Resolume-class en composición. El operador piensa en términos de:

- "Esta layer genera partículas, la pongo encima del structure con blend Add."
- "Solo esta layer mientras debuggeo."
- "Mute las 3 capas de FX para ver el background limpio."
- "Esta layer la mando al Preview para chequearla sin ensuciar el Master."

Si el engine no abstrae esto como un stack de primera clase, terminamos con render-passes hardcoded en `main.cpp` y cero capacidad de improv en vivo. Esta skill define el modelo que el renderer + la UI + el OSC controller comparten.

## Tipos de Layer — exhaustivos

Toda layer es **uno** de tres tipos. La decisión es del autor de la layer, no runtime.

### Generator

Produce output a partir de nada (no consume el accumulator below).

- `StructureLayer` — renderiza `structure.glb` con PBR forward y lights activas.
- `ParticleLayer` — emisión + simulación + sprite/quad rendering.
- `VolumetricLayer` — raymarched fog / beams.
- `FullscreenLayer` — fullscreen MSL/HLSL shader (gradientes, noise, patterns generativos).
- `VideoLayer` — playback de archivo / NDI source / capturadora (v2; slot reservado).

### Effect

Toma como input el accumulator below + opcionalmente su propio mask, produce un accumulator modificado.

- `BloomEffect` — bright-pass + multi-tap blur + composite.
- `ColorGradeEffect` — LUT / curves / saturation / contrast.
- `GlitchEffect` — RGB split / scanlines / displacement.
- `BlurEffect` — gaussian / radial / motion.
- `DistortLensEffect` — barrel / pinch / spherize.

### Group

Container con su propio sub-stack. Renderiza el sub-stack a un FBO temporal aislado, luego se presenta al parent como UNA layer (con blend mode + opacity propios).

- Permite encapsular composiciones complejas.
- Permite aplicar un blend mode / opacity al resultado completo del sub-stack.
- Permite reordenar grupos enteros sin descomponer.
- Permite tener output de un sub-stack que NO contamina el bus principal hasta el composite final.

## Layer States

Cada layer tiene **un** estado primario:

- `Disabled` — fuera del grafo. No consume recursos. No se evalúa. Como si no existiera.
- `Bypassed` — está en el grafo, su `render()` se skipea; equivale a pass-through del accumulator below. Útil para debug A/B.
- `Enabled` — render normal.

Y **dos** modificadores ortogonales, audio-style:

- `Mute` — fuerza visual Disabled, pero **preserva el estado primario** (al un-mute vuelve a Enabled o Bypassed).
- `Solo` — si CUALQUIER layer del bus tiene Solo activo, todas las demás se renderizan como muted. Múltiples Solo simultáneos OK (todos los Solo se renderizan, el resto se silencia).

**Regla operativa**: `Mute` y `Solo` son toggles "live" del operador (rápidos, no destructivos). `Disabled / Bypassed / Enabled` es estado persistente del proyecto.

## Propiedades por Layer

```cpp
// engine/core/Layer.h
namespace spacegen {

enum class LayerKind   { Generator, Effect, Group };
enum class LayerState  { Enabled, Bypassed, Disabled };
enum class BlendMode   { Normal, Add, Multiply, Screen, Overlay, SoftLight, HardLight, Luma };

using LayerID = std::array<uint8_t, 16>; // UUID v4 stable across renames

class ILayer {
public:
    virtual ~ILayer() = default;

    // Identidad
    virtual LayerKind kind()       const = 0;
    virtual const std::string& typeName() const = 0;  // "ParticleLayer" para UI

    // Render
    virtual void render(RenderContext& ctx) = 0;

    // Estado primario
    LayerState  state = LayerState::Enabled;

    // Modificadores audio-style
    bool mute = false;
    bool solo = false;

    // Composición sobre el bus
    std::shared_ptr<Parameter> opacity;     // [0..1], obligatorio, registrado en ParamGraph
    BlendMode   blendMode = BlendMode::Normal;

    // Apariencia / UI
    std::string name;        // editable in UI
    std::string colorTag;    // hex, para grouping visual en el rack

    // Mask channel opcional — referencia a otra layer del mismo bus por ID
    std::optional<LayerID> maskSource;

    // ID estable (UUID) — sobrevive renames
    LayerID id;
};

} // namespace spacegen
```

`opacity` es **siempre** un `Parameter` (sigue la skill `parameter-graph-spacegen`). No es un float pelado.

## Buses

Un **Bus** es un punto de mezcla con su propia layer stack hacia un output.

### Master Bus

- Bus principal. Su output va a Syphon (Mac) / Spout (Win, fase 2) / NDI / capture.
- Toda layer "principal" del proyecto vive en su stack.
- 1 solo per proyecto.

### Preview Bus

- Output secundario, mostrado en la GUI preview window.
- Por default refleja el Master.
- Modo `Cued` (estilo Resolume "Preview before Master"): el operador asigna layers temporalmente al Preview antes de soltarlas al Master en vivo.
- Permite chequear una layer sin ensuciar el output al público.

### Aux Buses (N, configurables)

- Outputs adicionales (a otros Syphon clients, a otros NDI streams, a un segundo monitor físico).
- Sub-stacks independientes. Una layer puede vivir en master **y** en aux (referencia compartida), o sólo en aux.

```cpp
class Bus {
public:
    std::string name;
    std::vector<std::shared_ptr<ILayer>> layers;  // bottom-to-top
    RenderTarget output;                          // FBO RGBA16Float + Depth32Float
    std::shared_ptr<Parameter> masterOpacity;     // bus-level
    std::shared_ptr<Parameter> masterBlend;       // si bus se composita en otro (poco común)
};

class Scene {
public:
    Bus              master;
    Bus              preview;
    std::vector<Bus> aux;
};
```

## Render order y traversal

**Bottom-to-top dentro de un bus.** Layer en index 0 es la primera en pintar, layer en index `N-1` la última (queda visualmente encima).

Pseudocódigo del traversal:

```
for each Bus in scene:
    clear bus.output to (0,0,0,0)
    let any_solo = layers.any(l -> l.solo and l.state == Enabled)

    for each layer in bus.layers (index 0 → N-1):
        if layer.state == Disabled: continue
        if layer.mute:               continue
        if any_solo and !layer.solo: continue
        if layer.state == Bypassed:  continue   # pass-through (no render call)

        ctx.accumulator = bus.output
        ctx.blendMode   = layer.blendMode
        ctx.opacity     = layer.opacity.currentValue()
        ctx.maskTex     = resolveMask(layer.maskSource)

        switch (layer.kind):
            case Generator: layer.render(ctx)  // writes into accumulator with blend+opacity
            case Effect:    layer.render(ctx)  // reads accumulator, writes back
            case Group:     renderSubStack(group); compositeGroupOutputOntoAccumulator()

    bus.output now contains the final mix
```

**No hay DAG / no hay render graph dinámico en v1.** Stack lineal por bus. Suficiente para Resolume-class. Un render graph dinámico con dependencias arbitrarias se mete en v2 si una feature lo justifica explícitamente.

## Alpha contract

**Premultiplied alpha throughout.** El framebuffer interno de cada bus (`RGBA16Float`) almacena `(R·a, G·a, B·a, a)`.

Razones:

1. Composición correcta sin artefactos en bordes (no halos).
2. Blend modes simples y matemáticamente correctos.
3. Layers que generan luz directamente (StructureLayer con `alpha = light_contribution`) encajan naturalmente.
4. Output a Syphon/NDI: ambos esperan straight alpha en su API → conversión `(R/a, G/a, B/a, a)` al publishing. Si `a == 0`, el RGB se ignora.

### Color space

- **Framebuffer interno**: **linear** color, sRGB no aplicado.
- **Lectura de texturas sRGB**: gamma-decode al sample (idealmente vía sRGB texture format, no manual).
- **Output a Syphon/NDI**: gamma-encode al publish (media servers downstream esperan sRGB).

### Blend modes — referencia rápida

Detalles completos viven en `compositing-blend-modes-spacegen` (skill a redactar junto a M3). Las que se usarán desde día 1:

- **Normal (Over)**: `dst = src + dst * (1 − src.a)` — composición clásica premultiplied.
- **Add**: `dst.rgb = src.rgb + dst.rgb`, `dst.a = src.a + dst.a · (1 − src.a)` — para luces, partículas brillantes.
- **Multiply**: `dst.rgb = src.rgb · dst.rgb / max(dst.a, ε) · dst.a` (variante premul correcta) — para sombras / tintes.
- **Screen**: `dst.rgb = src.rgb + dst.rgb − src.rgb · dst.rgb / max(dst.a, ε) · dst.a` — para highlights.

## Cómo añadir un nuevo tipo de Layer

Pasos canónicos:

1. **Crear** `engine/core/layers/MyEffectLayer.h` y `.cpp`. Hereda de `ILayer`.
2. **Decidir** `kind()`: ¿Generator o Effect?
3. **Registrar parámetros** en el constructor vía `ParameterRegistry::global().create(...)`. Toda variable controlable es un Parameter (skill `parameter-graph-spacegen`).
4. **Implementar `render(RenderContext&)`**:
   - Generator: dibuja en el accumulator, respetando `ctx.opacity` y `ctx.blendMode`.
   - Effect: lee del accumulator, dibuja modificado de vuelta al mismo accumulator.
5. **Registrar el tipo** en `LayerFactory::registerType<MyEffectLayer>()` para que aparezca en el menú "Add Layer" de la UI.
6. **(Opcional)** `serialize()` / `deserialize()` adicionales si la layer tiene estado no-parámetro (p.ej., seed de un PRNG).

### Ejemplo mínimo

```cpp
// engine/core/layers/GlitchEffectLayer.h
class GlitchEffectLayer : public ILayer {
public:
    GlitchEffectLayer();
    LayerKind kind() const override { return LayerKind::Effect; }
    const std::string& typeName() const override {
        static const std::string n = "GlitchEffectLayer"; return n;
    }
    void render(RenderContext& ctx) override;

private:
    std::shared_ptr<Parameter> amount_;
    std::shared_ptr<Parameter> blockSize_;
};

// .cpp
GlitchEffectLayer::GlitchEffectLayer() {
    auto& reg = ParameterRegistry::global();
    amount_ = reg.create({
        .id = "fx.glitch." + uuidToHex(id) + ".amount",
        .oscPath = "/fx/glitch/" + uuidToHex(id) + "/amount",
        .displayName = "Amount",
        .group = "FX/Glitch",
        .range = {0.0f, 1.0f, 0.3f},
    });
    blockSize_ = reg.create({
        .id = "fx.glitch." + uuidToHex(id) + ".block_size",
        .oscPath = "/fx/glitch/" + uuidToHex(id) + "/block_size",
        .displayName = "Block Size",
        .group = "FX/Glitch",
        .range = {1.0f, 64.0f, 8.0f},
        .smoothingMs = 0.0f,  // discreto, sin smoothing
    });
}

void GlitchEffectLayer::render(RenderContext& ctx) {
    // ctx.accumulator es la textura con el resultado del stack debajo.
    // Hacemos un fullscreen pass que lee de ella y escribe back con la modificación.
    auto pipeline = ctx.renderer->getPipeline("glitch_effect");

    GlitchUniforms u {
        .amount    = amount_->currentValue() * ctx.opacity,
        .blockSize = blockSize_->currentValue(),
        .time      = ctx.elapsedSeconds,
    };

    ctx.renderer->beginEffectPass(ctx.accumulator);
    ctx.renderer->setPipeline(pipeline);
    ctx.renderer->setFragmentTexture(ctx.accumulator.color(), 0);
    ctx.renderer->setFragmentBytes(&u, sizeof(u), 0);
    ctx.renderer->setBlendMode(ctx.blendMode);
    ctx.renderer->drawFullscreenTriangle();
    ctx.renderer->endEffectPass();
}
```

## Conexión con `IRenderer`

`ILayer::render()` recibe un `RenderContext` que contiene un `IRenderer*`. **El layer hace todas las llamadas gráficas via el renderer, no via Metal-cpp / D3D12 directamente.** Esto asegura que la layer compila en ambos backends sin cambios.

```cpp
struct RenderContext {
    IRenderer*       renderer;        // abstracted graphics API
    RenderTarget*    accumulator;     // current bus FBO
    const Scene*     scene;           // acceso a lights / camera / structure
    const BPMClock*  bpm;             // beat / phase / elapsed beats
    float            dt;              // seconds since last frame
    double           elapsedSeconds;  // total session time
    int              frameIndex;

    // Per-layer state (set por el bus loop antes de llamar a render):
    float            opacity;
    BlendMode        blendMode;
    TextureHandle    maskTex;         // 1×1 white si no hay mask
};
```

## Anti-patrones

❌ Pintar directamente al framebuffer de la ventana desde una layer.
✅ Pintar al `RenderContext::accumulator`. El bus se encarga de presentar al final.

❌ Hardcodear el orden de layers en código (`Structure first, particles next, bloom last`).
✅ Layers se añaden y reordenan dinámicamente desde la UI / OSC. El render loop sólo itera la lista del bus.

❌ Layer que genera luz expone "RGB sin alpha" y queda opaca cuando no hay efecto.
✅ Layer respeta el alpha contract: cuando nada genera output, alpha = 0 (transparente).

❌ Layer llamando `MetalRenderer::*` o `D3D12Renderer::*` directamente.
✅ Layer llama `RenderContext::renderer->*` (interface abstracta).

❌ "Solo" implementado como boolean en el rendering loop fuera de la abstracción del bus.
✅ Solo es un toggle por layer; el bus consulta `any_solo_active()` y aplica la regla.

❌ `Mute` destruye estado primario (al un-mute la layer vuelve a Disabled).
✅ Mute es ortogonal: `Mute(Enabled) → muted`, `unmute → Enabled`. Nunca toca el estado primario.

❌ Group renderiza al framebuffer del parent directamente (sin aislar).
✅ Group renderiza a un FBO temporal y SÓLO la textura final del group se composita al parent. Aislamiento total.

❌ Blend mode aplicado en CPU comparando muchos casos (`if blendMode == Add then ... else if Multiply then ...`).
✅ Blend mode aplicado al estado del pipeline (`setBlendMode()`) — un solo draw call. El renderer traduce a Metal/D3D12 blend state.

❌ Effect layer modifica el accumulator destructivamente sin respetar `opacity`.
✅ El effect produce el resultado modificado y lo mezcla con el accumulator usando `opacity` como factor: `final = mix(accumulator, effected, opacity)`.

## UI implícita (referencia para `imgui-vj-workstation`)

El **Layer Rack** debe mostrar:

- Lista vertical, **bottom-to-top** (arriba en pantalla = encima visualmente; refleja el render order).
- Cada layer en una "card":
  - Enable / Bypass / Disable toggle (3-way)
  - Mute toggle (M)
  - Solo toggle (S)
  - Blend mode dropdown (Normal / Add / Multiply / Screen / Overlay / ...)
  - Opacity slider (compacto)
  - Name editable
  - Color tag (clickable swatch)
  - Drag-handle (reordering)
- Click sobre layer → la selecciona, sus parámetros aparecen en el Inspector panel.
- Drag para reorder dentro del bus. Drag entre buses para mover (Master ↔ Preview ↔ Aux).
- Right-click → menú: Delete, Duplicate, Group / Ungroup, Send to Preview, Send to Aux N.

Detalles UI viven en `imgui-vj-workstation` (a redactar tras la primera preview window funcional).

## Cuándo violar esta skill

Casi nunca. La excepción es **debug / hardcoded prototypes** — en M2 dibujamos el structure mesh directamente sin pasar por `ILayer` para validar el camera matching. En cuanto pasamos a M3 (lighting + primera FX layer), todo se reescribe encima de `ILayer`. No queda código fuera del modelo.
