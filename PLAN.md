# SpaceGen — Industry-Grade Generative Video Engine
## Master Plan v1.0

---

## Stack Decision

**Runtime**: C++ 17  
**Window/Context**: GLFW 3.4  
**Graphics**: OpenGL 4.1 Core Profile (macOS ceiling, also Linux/Windows compatible)  
**Shading**: GLSL 4.10  
**GUI**: Dear ImGui (docking branch)  
**Math**: GLM 0.9.9  
**Image**: stb_image (PNG/JPG for mask sequences)  
**File dialogs**: nativefiledialog-extended  
**Build**: CMake 3.25+ with FetchContent (no manual vendor management)  
**Audio (Phase 4)**: miniaudio + custom FFT  
**MIDI (Phase 4)**: RtMidi  
**Syphon (Phase 4)**: Syphon-C (macOS texture sharing)  
**NDI (Phase 5)**: NDI SDK  

Why this stack over any web/Electron approach:
- Zero runtime overhead between CPU and GPU
- Frame-perfect timing (critical for BPM sync on stage)
- Direct OpenGL state control — no abstraction taxes
- Dear ImGui is used inside Notch, game engines, audio tools — it IS the industry standard for operator interfaces
- Runs headless or multi-output natively
- Ships as a single binary

---

## Repository Structure

```
spacegen/
├── engine/                    # All C++ source
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp
│   │   ├── core/
│   │   │   ├── Window.h/cpp           # GLFW wrapper, fullscreen, multi-monitor
│   │   │   ├── Renderer.h/cpp         # OpenGL state machine, draw loop
│   │   │   ├── LayerManager.h/cpp     # Ordered layer stack, blend modes
│   │   │   ├── BPMClock.h/cpp         # High-precision BPM, tap, SMPTE-ready
│   │   │   ├── MaskSequence.h/cpp     # PNG sequence, streaming, GPU upload
│   │   │   ├── ShaderProgram.h/cpp    # Compile, link, hot-reload on file change
│   │   │   ├── RenderTarget.h/cpp     # FBO wrapper, ping-pong utility
│   │   │   ├── TexturePool.h/cpp      # LRU cache, async upload
│   │   │   └── PresetManager.h/cpp    # Save/load layer stacks as JSON
│   │   ├── layers/
│   │   │   ├── ILayer.h               # Pure virtual interface
│   │   │   ├── ShaderLayer.h/cpp      # Fullscreen quad + GLSL program
│   │   │   ├── ParticleLayer.h/cpp    # GPU particles via Transform Feedback
│   │   │   ├── GeometryLayer.h/cpp    # Instanced geometry
│   │   │   └── FeedbackLayer.h/cpp    # Ping-pong FBO feedback effects
│   │   └── gui/
│   │       ├── GUI.h/cpp              # Main ImGui host
│   │       ├── LayerPanel.h/cpp       # Per-layer controls
│   │       ├── BPMPanel.h/cpp         # BPM clock + tap tempo
│   │       ├── OutputPanel.h/cpp      # Resolution, fullscreen, Syphon
│   │       └── PresetPanel.h/cpp      # Save/load presets
│   └── shaders/
│       ├── common/
│       │   ├── fullscreen.vert        # Shared vertex shader for all layers
│       │   ├── noise.glsl             # Shared noise functions (include)
│       │   └── palette.glsl           # Cosine palette utility
│       └── layers/
│           ├── chaser.frag
│           ├── saber.frag
│           ├── plasma.frag
│           ├── domain_warp.frag
│           ├── kaleidoscope.frag
│           ├── voronoi.frag
│           ├── starfield.frag
│           ├── chroma_glitch.frag
│           └── ... (more as research delivers)
├── research/                  # Research team writes here
│   ├── REPORT.md              # Daily findings
│   ├── ENGINEERING_BRIEF.md   # Prioritized shader queue
│   └── MORNING_BRIEF.md       # Coordinator writes every morning for engineers
└── engineering/               # Engineering team writes here
    ├── STATUS.md              # What each engineer is working on
    ├── DECISIONS.md           # Blocked decisions — user reads and answers here
    └── PROGRESS.md            # Completed milestones log
```

---

## Team Structure

### Research Team (already running 24/7)

| Agent | Schedule | Responsibility |
|---|---|---|
| Research Updater | Every 2h | Web research → REPORT.md + ENGINEERING_BRIEF.md |
| Tools Survey | Every 3h | TouchDesigner, Resolume, Notch, ISF, VJ community news |
| Shaders Deep-Dive | Every 4h | GLSL algorithms, Shadertoy, LYGIA — full code detail |

**Output**: `research/ENGINEERING_BRIEF.md` is the contract between research and engineering. It contains a live priority queue with full GLSL algorithm descriptions, references, and implementation notes.

---

### Engineering Team (to be created)

| Agent | Schedule | Responsibility |
|---|---|---|
| **Morning Coordinator** | 08:00 UTC daily | Reads ENGINEERING_BRIEF.md + STATUS.md. Writes MORNING_BRIEF.md. Assigns top 3 tasks. Surfaces decisions needed from user. |
| **Lead Engineer** | 09:00 UTC daily | Core engine: Window, Renderer, LayerManager, RenderTarget, ShaderProgram. Architecture decisions. |
| **Shader Engineer** | 11:00 UTC daily | Reads MORNING_BRIEF, implements GLSL shaders. Ports from brief. One shader per run. |
| **Systems Engineer** | 14:00 UTC daily | BPM clock, mask sequence, texture pool, preset system, audio FFT (Phase 4). |
| **GUI Engineer** | 16:00 UTC daily | Dear ImGui panels. Layer controls, BPM panel, parameter binding, MIDI learn (Phase 4). |
| **QA / Build** | 19:00 UTC daily | Verifies CMake builds cleanly. Checks shader compilation. Fixes integration issues. Commits nightly build status. |

**Communication protocol**:
- All agents read `research/MORNING_BRIEF.md` before working
- All agents write their daily status to `engineering/STATUS.md`
- If an agent is blocked on a user decision: writes to `engineering/DECISIONS.md` and moves to the next task
- User reads `engineering/DECISIONS.md` each morning and writes answers inline
- Morning Coordinator picks up answers and forwards context to the right engineer

---

## Phases & Milestones

### PHASE 1 — Foundation (Weeks 1–2)
*Goal: A window opens. A shader renders. The GUI appears. Hot-reload works.*

- [ ] CMake project (GLFW + GLAD + ImGui via FetchContent)
- [ ] GLFW window, OpenGL 4.1 Core Profile, vsync
- [ ] Dear ImGui docking layout initialized
- [ ] ShaderProgram: compile, link, uniform set, hot-reload (inotify/kqueue file watcher)
- [ ] Fullscreen quad (VAO/VBO) shared by all ShaderLayers
- [ ] First shader live: PlasmaWave renders, GUI sliders control it
- [ ] BPMClock: high-precision (std::chrono), tap tempo, phase output

**Milestone**: Open app → see PlasmaWave → move sliders → tap tempo → shader changes with beat.

---

### PHASE 2 — Layer Engine (Weeks 3–4)
*Goal: Multiple layers composited correctly. Masks working. Basic GUI complete.*

- [ ] ILayer interface (pure virtual: `render()`, `renderGUI()`, `setMask()`, `setBlend()`, `setOpacity()`)
- [ ] LayerManager: ordered stack, add/remove/reorder
- [ ] Blend modes via OpenGL blend equation (Additive, Normal, Screen, Multiply)
- [ ] RenderTarget (FBO): each layer renders to its own texture, compositor blends
- [ ] MaskSequence: load PNG directory, stream frames at BPM rate, GPU texture upload
- [ ] TexturePool: LRU cache, async upload queue, max VRAM budget
- [ ] GUI: layer list (drag-to-reorder), per-layer blend/opacity/enable, mask loader

**Milestone**: 3 layers + masks + blend modes working in real time. GUI fully usable.

---

### PHASE 3 — Shader Library (Weeks 5–8)
*Goal: 15+ shaders. All quality. All BPM-reactive. All mask-aware.*

Port and improve all shaders from research brief in priority order:
- [ ] PlasmaWave (cosine palette, BPM pulse)
- [ ] DomainWarp (IQ two-layer warp, BPM strength spike)
- [ ] SaberEdge (Sobel edge detection + multi-pass glow)
- [ ] ChaserBeam (traveling band, texture, distortion)
- [ ] StarField (3-layer parallax, BPM flash)
- [ ] Kaleidoscope (N-fold UV fold, rotation)
- [ ] VoronoiCells (animated Worley noise)
- [ ] ChromaGlitch (RGB split, scanlines, BPM gate)
- [ ] OscilloscopeWave (Lissajous, BPM-phase lock)
- [ ] RaymarchTunnel (SDF infinite tunnel, BPM speed)
- [ ] ReactionDiffusion (Gray-Scott via ping-pong FBO)
- [ ] FeedbackEcho (zoom+rotate feedback loop)
- [ ] FluidSim (Jos Stam stable fluids, ping-pong)
- [ ] + whatever research team delivers

**Each shader must**:
- Compile without warnings
- Have minimum 5 GUI parameters
- React to BPM (beat trigger and/or phase-locked animation)
- Accept mask as gate or source image
- Run at 60fps at 1080p on integrated GPU

---

### PHASE 4 — Pro Features (Weeks 9–16)
*Goal: Usable on a real stage.*

- [ ] Audio FFT: miniaudio input → FFT → `uFFT` texture uniform (bass/mid/hi bands)
- [ ] MIDI learn: right-click any slider → assign CC
- [ ] OSC control: receive parameter changes over network (TouchOSC, Lemur)
- [ ] Preset system: save/load full layer stacks as JSON
- [ ] Syphon server: share output texture to MadMapper/VDMX/etc (macOS)
- [ ] Multi-output: second window on second monitor/LED processor
- [ ] GPU particle system: Transform Feedback (no readback, fully GPU-side)
- [ ] Shader hot-reload: file watcher triggers recompile without restart
- [ ] Performance overlay: GPU time per layer (via timer queries)

---

### PHASE 5 — Distribution (Weeks 17–24)
*Goal: Ships. Reliable. Fast.*

- [ ] NDI output: stream over network to LED walls/screens
- [ ] Windows port (GLFW + GLAD is cross-platform, ImGui is cross-platform)
- [ ] Signed macOS app bundle
- [ ] DMX input (ArtNet → parameter mapping)
- [ ] SMPTE timecode input (LTC audio)
- [ ] Headless mode: CLI-driven, no GUI, for dedicated hardware
- [ ] Auto-crash recovery: save state every 30s, restore on relaunch

---

## Quality Standards (non-negotiable for all agents)

**Performance**:
- Every shader must run at 60fps at 1920×1080 on Apple M-series integrated GPU
- GPU timer queries on every layer — no layer >8ms per frame
- Texture uploads async — never stall the render thread
- FBO ping-pong pre-allocated at startup — no runtime alloc

**Code quality**:
- C++17, no raw owning pointers (use `unique_ptr`, `shared_ptr`)
- Every OpenGL call wrapped with `GL_CHECK()` macro in debug builds
- Shader compilation errors reported with file + line number
- CMake: no hardcoded paths, FetchContent for all dependencies

**Usability**:
- Every parameter reachable without mouse (keyboard shortcuts)
- Every parameter MIDI-learnable (Phase 4)
- GUI state persists between sessions (ImGui .ini)
- File dialogs use native OS dialogs (nfd)

**Scalability**:
- Shaders loaded from `.frag` files at runtime — add a shader = drop a file
- Preset format is plain JSON — human-readable, version-controlled
- Layer system uses virtual interface — new layer types drop in without touching core

---

## Decision Log

*Engineering agents write here when blocked. User writes answers below each item.*

> Format: agents write `[DECISION-001] Question here` and continue with another task.
> User replies: `[DECISION-001 ANSWER] Answer here` — morning coordinator picks it up.

*(empty — no decisions yet)*
