---
name: parameter-graph-spacegen
description: Modelo unificado de parámetros para SpaceGen. Úsala cuando definas un parámetro controlable, expongas algo via MIDI/OSC/audio-reactive/automation, registres un slider en ImGui, serialices proyectos, o diseñes la interfaz de cualquier subsistema con parámetros (luces, FX, layers, post-FX, BPM clock). Si alguien escribe `float intensity` como miembro pelado de una clase de feature, está violando esta skill.
---

# Parameter Graph — el contrato de control de SpaceGen

## Por qué existe esta skill

SpaceGen es una herramienta VJ-class: el operador en GUI mueve un knob, un MIDI controller mapea CC al mismo knob, OSC desde tablet también, un audio-reactive driver modula el mismo knob según FFT band, una automation envelope lo anima entre cues. Si cada subsistema (ImGui / OSC / MIDI / audio / automation) habla con el engine usando su propio modelo de parámetros, terminamos con:

- 5 representaciones distintas del mismo valor (drift entre subsistemas).
- "MIDI learn" que no funciona para parámetros expuestos sólo a ImGui.
- Audio-reactive que sólo puede modular cosas que el dev acordó cablear a mano.
- Serialización de proyectos que recuerda algunos parámetros y olvida otros.

**Esta skill define EL parámetro como objeto de primera clase**, registrado en un grafo global. Todo subsistema lo conoce. Cualquier param es automáticamente MIDI-learnable, OSC-receivable, audio-reactive-bindable, animatable, serializable, y aparece en ImGui — sin código extra por subsistema.

## El descriptor

Todo param se define con un `ParamDescriptor`:

```cpp
// engine/core/Parameter.h
namespace spacegen {

enum class ParamType {
    Float,
    Int,
    Bool,
    Color,        // RGB (vec3) o RGBA (vec4)
    Vec3,
    Enum,
};

enum class ParamUnit {
    None,
    Normalized,   // 0..1
    Percent,      // 0..100
    Decibel,
    Hertz,
    BPM,
    Beat,         // 0..1 phase
    Degrees,
    Radians,
    Seconds,
    Milliseconds,
    Meters,
};

struct ParamRange {
    float min          = 0.0f;
    float max          = 1.0f;
    float defaultValue = 0.0f;
};

struct ParamDescriptor {
    // Identidad (estables entre versiones del proyecto)
    std::string id;          // p.ej. "light.spot_1.intensity"
    std::string oscPath;     // canonical, p.ej. "/light/spot_1/intensity"

    // UI
    std::string displayName; // "Intensity"
    std::string tooltip;     // "Multiplies the spot's luminous flux"
    std::string group;       // "Lights/Spot 1"
    std::string colorTag;    // opcional; hex color para visual grouping

    // Semántica
    ParamType   type   = ParamType::Float;
    ParamUnit   unit   = ParamUnit::None;
    ParamRange  range;
    std::vector<std::string> enumLabels; // sólo si type == Enum

    // Capabilities
    bool        animatable    = true;   // ¿LFO / envelope / audio pueden modular?
    bool        midiLearnable = true;
    bool        oscReceivable = true;

    // Suavizado (anti-stepping en automation y MIDI)
    float       smoothingMs   = 30.0f;  // 0 = sin smoothing
};

} // namespace spacegen
```

## El parámetro

```cpp
class Parameter {
public:
    explicit Parameter(ParamDescriptor desc);

    const ParamDescriptor& descriptor() const { return desc_; }

    // ---- ESCRITURAS (orden de prioridad: operator > MIDI > OSC > modulator) ----
    void setFromOperator(float v);    // GUI knob
    void setFromMIDI(float v);        // hardware CC tras learn
    void setFromOSC(float v);         // red
    void setFromModulator(float v);   // LFO / envelope / audio-reactive

    // ---- LECTURA ----
    float currentValue() const;       // smoothed value, listo para consumer

    // ---- LIFECYCLE ----
    void tick(float dt);              // suaviza hacia el target cada frame

private:
    ParamDescriptor desc_;
    float   targetValue_         = 0.0f;
    float   currentValue_        = 0.0f;
    uint8_t lastWriterPriority_  = 0;
    double  lastOperatorWriteAt_ = 0.0;
    // ...
};
```

## La prioridad de escritura

```
1. Operator (GUI)  — gana siempre, override por ~200ms post-touch (anti-glitch contra modulators)
2. MIDI CC         — gana sobre OSC y modulator
3. OSC             — gana sobre modulator
4. Modulator       — sólo si nada de lo anterior escribió este frame
   (LFO / envelope / audio FFT)
```

La regla práctica: **mientras el operador esté tocando el knob (last operator write < 200ms), todo lo demás se ignora**. Cuando suelta, MIDI/OSC/modulator vuelven a tener control. Esto evita la pesadilla de "muevo el knob y un modulator me lo arranca de las manos."

## El registro global

```cpp
class ParameterRegistry {
public:
    static ParameterRegistry& global();

    std::shared_ptr<Parameter> create(ParamDescriptor desc);
    std::shared_ptr<Parameter> findById(const std::string& id);
    std::shared_ptr<Parameter> findByOSCPath(const std::string& oscPath);

    std::vector<std::shared_ptr<Parameter>> all() const;
    std::vector<std::shared_ptr<Parameter>> byGroup(const std::string& group) const;

    // Serialización
    nlohmann::json snapshot() const;
    void restore(const nlohmann::json& snap);

    void tickAll(float dt);
};
```

**Toda subsistema obtiene sus parámetros llamando `create()` en el registry.** No hay `float intensity_;` pelado en ninguna clase de feature.

## El patrón de registro

```cpp
// engine/core/SpotLight.cpp
SpotLight::SpotLight(std::string name) : name_(std::move(name)) {
    auto& reg = ParameterRegistry::global();

    intensity_ = reg.create({
        .id          = "light." + name_ + ".intensity",
        .oscPath     = "/light/" + name_ + "/intensity",
        .displayName = "Intensity",
        .tooltip     = "Luminous flux multiplier",
        .group       = "Lights/" + name_,
        .type        = ParamType::Float,
        .unit        = ParamUnit::Normalized,
        .range       = {0.0f, 1.0f, 0.5f},
    });

    coneInner_ = reg.create({
        .id          = "light." + name_ + ".cone_inner",
        .oscPath     = "/light/" + name_ + "/cone/inner",
        .displayName = "Inner Cone",
        .tooltip     = "Angle (degrees) where intensity is full",
        .group       = "Lights/" + name_,
        .unit        = ParamUnit::Degrees,
        .range       = {0.0f, 90.0f, 15.0f},
    });

    color_ = reg.create({
        .id          = "light." + name_ + ".color",
        .oscPath     = "/light/" + name_ + "/color",
        .displayName = "Color",
        .group       = "Lights/" + name_,
        .type        = ParamType::Color,
    });
}

// En el render path:
void SpotLight::pack(GPULightData& out) const {
    out.intensity = intensity_->currentValue();
    out.coneInnerCos = std::cos(glm::radians(coneInner_->currentValue()));
    out.color = readColor(*color_);
}
```

## Binding helpers — cada subsistema usa los params sin saber del otro

### ImGui

```cpp
// engine/gui/ParamWidgets.h
void DrawParamSlider(Parameter& p);  // detecta tipo y dibuja widget apropiado
void DrawParamKnob(Parameter& p);
void DrawParamColor(Parameter& p);

// Convención: right-click sobre cualquier widget → context menu con:
//   - "MIDI Learn"            (sólo si midiLearnable)
//   - "Bind to FFT band..."   (sólo si animatable)
//   - "Bind to LFO..."        (sólo si animatable)
//   - "Bind to Envelope..."   (sólo si animatable)
//   - "Reset to default"
//   - "Show OSC path"         (copia al clipboard)
```

El widget lee `Parameter::currentValue()` y llama `Parameter::setFromOperator()` al cambiar. Trivial.

### OSC server — un dispatcher único

```cpp
// engine/control/OSCServer.cpp
void OSCServer::onMessage(const std::string& path, float value) {
    auto p = ParameterRegistry::global().findByOSCPath(path);
    if (p && p->descriptor().oscReceivable) {
        p->setFromOSC(value);
    }
}
```

Cero código por param nuevo. Cualquier param con `oscReceivable=true` aparece automáticamente en la red.

### MIDI learn

```cpp
// engine/control/MIDIMapper.cpp
void MIDIMapper::learn(int ccNumber, std::shared_ptr<Parameter> target) {
    if (!target || !target->descriptor().midiLearnable) return;
    mappings_[ccNumber] = target;
}

void MIDIMapper::onCC(int cc, float normalizedValue) {
    auto it = mappings_.find(cc);
    if (it != mappings_.end()) {
        it->second->setFromMIDI(normalizedValue);
    }
}
```

### Audio-reactive

```cpp
// engine/audio/AudioReactiveBinding.cpp
struct AudioBinding {
    std::shared_ptr<Parameter> target;
    int   fftBand;
    float floor;     // mínimo (silencio)
    float ceiling;   // máximo (pico)
    float attack;
    float release;
};

void AudioReactiveDriver::onFrame(const FFTAnalysis& fft) {
    for (auto& bind : bindings_) {
        float raw = fft.bands[bind.fftBand];
        float v   = remap(raw, 0.0f, 1.0f, bind.floor, bind.ceiling);
        bind.target->setFromModulator(v);
    }
}
```

## Serialización de proyecto

```cpp
// Save:
auto snap = ParameterRegistry::global().snapshot();
file.write(snap.dump(2));

// Load:
auto snap = nlohmann::json::parse(file.readAll());
ParameterRegistry::global().restore(snap);
```

`snapshot()` produce:

```json
{
  "schema_version": 1,
  "params": {
    "light.spot_1.intensity": 0.75,
    "light.spot_1.cone_inner": 22.0,
    "light.spot_1.color": [1.0, 0.6, 0.2]
  }
}
```

**Compat behavior**:
- Params en el snapshot que no existen en la versión actual → ignorar con warning.
- Params en la versión actual que no aparecen en el snapshot → usar `defaultValue` del descriptor.
- Nunca fallar el load del proyecto entero por un param desconocido o faltante.

## Convenciones de naming

- **IDs en dot notation**, todo lowercase, snake_case por segmento.
  - ✅ `light.spot_1.intensity`, `fx.bloom.threshold`, `master.opacity`
  - ❌ `Light.SpotOne.Intensity`, `spot1Intensity`, `bloom-threshold`
- **OSC paths en slash notation**, mismo lowercase snake_case.
  - ID `light.spot_1.intensity` → OSC `/light/spot_1/intensity`
  - **Default automatizable**: si no se especifica `oscPath`, derivar de `id` con `.` → `/` y prefix `/`.
- **Group strings** usan `/` como separador para anidamiento de panel.
  - `"Lights/Spot 1"`, `"FX/Bloom"`, `"Master/Output"`
- **Display names** en title case, inglés (UI bilingüe se decide más tarde).

## Anti-patrones

❌ `float intensity_ = 0.5f;` como miembro pelado de una clase de feature.
✅ `std::shared_ptr<Parameter> intensity_;` vía `ParameterRegistry::global().create(...)`.

❌ `ImGui::SliderFloat("Intensity", &intensity_, 0, 1);` directo sobre un float.
✅ `DrawParamSlider(*intensity_);` — el widget habla con `Parameter`.

❌ OSC server con `if (path == "/light/spot1/intensity") spotLight.setIntensity(value);` — uno por param.
✅ Dispatcher único que busca en el registry.

❌ Definir el rango (`0..1`) sólo en el slider ImGui.
✅ El rango vive en el `ParamDescriptor`. ImGui, OSC, MIDI, audio-reactive lo consultan desde ahí.

❌ Cablear "audio FFT bass → bloom intensity" en código duro.
✅ Operador en UI elige "Bind to FFT band 0" sobre el slider de bloom intensity.

❌ Param sin smoothing para algo que se anima (stepping audible/visible).
✅ Todo param numérico arranca con `smoothingMs = 30.0f` salvo justificación (enum / bool / discretos).

❌ Cambiar el `id` de un param entre versiones del proyecto sin migration.
✅ IDs son contrato. Si renombras, agregar regla en `restore()` que mapee viejo → nuevo.

❌ Bypassar el grafo "porque es interno y no se va a animar."
✅ Si el operador lo va a tocar EN ALGÚN ESCENARIO, registrarlo. Si es realmente interno (epsilon de un solver, valor calculado, no exposed) puede ser miembro normal.

## Ejemplo end-to-end: añadir "Roughness multiplier" al StructurePass

```cpp
// engine/passes/StructurePass.h
class StructurePass {
public:
    StructurePass();
    void render(RenderContext& ctx);
private:
    std::shared_ptr<Parameter> roughnessMul_;
};

// engine/passes/StructurePass.cpp
StructurePass::StructurePass() {
    roughnessMul_ = ParameterRegistry::global().create({
        .id          = "structure.roughness_mul",
        .oscPath     = "/structure/roughness_mul",
        .displayName = "Roughness ×",
        .tooltip     = "Multiplica el roughness de cada material PBR del structure",
        .group       = "Structure",
        .range       = {0.0f, 4.0f, 1.0f},
        .smoothingMs = 50.0f,
    });
}

void StructurePass::render(RenderContext& ctx) {
    float rmul = roughnessMul_->currentValue();
    ctx.renderer->setFragmentBytes(&rmul, sizeof(rmul), kRoughnessMulBuffer);
    // ... draw call
}
```

**Resultado automático** (sin tocar más código):
- Aparece en ImGui under "Structure" group, slider 0..4 default 1, tooltip visible.
- Right-click → "MIDI Learn" funciona.
- OSC `/structure/roughness_mul 2.5` funciona.
- "Bind to FFT band" / "Bind to LFO" funcionan.
- Se serializa al guardar proyecto, se restaura al cargar.

## Cuándo violar esta skill

Casi nunca. Excepción legítima: parámetros internos de algoritmo que el operador no toca y no se animan (e.g., epsilon de un solver, constantes de calibración). Esos pueden ser `constexpr` y no necesitan grafo.

Si dudas, regístralo. El coste son ~8 líneas de descriptor, el beneficio es todo el control automático.
