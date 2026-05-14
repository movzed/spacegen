# Decisions Needed from Federico

Agents write here when blocked. Continue with other tasks after writing.
User answers inline. Morning Coordinator reads this every day at 08:00 UTC.

---

[DECISION-001] Should the C++ engine target macOS only for Phase 1-3, or should CMake be set up for cross-platform (macOS + Windows) from day one? Cross-platform from the start adds ~10% overhead to every PR but avoids a painful port later.

[DECISION-002] Output resolution strategy: fixed 1920×1080 internal render target with output scaling, or match the window/monitor resolution dynamically? Fixed is simpler and more predictable for live use. Dynamic is more flexible.

[DECISION-003] For Phase 4 audio reactivity, should FFT bands drive uniforms automatically (always-on), or should the user explicitly map audio bands to parameters per layer (more control, more setup)?
