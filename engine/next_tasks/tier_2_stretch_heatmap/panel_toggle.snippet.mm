// =============================================================================
// Tier 2 — Stretch Heatmap panel controls
// =============================================================================
//
// Insert into drawUvAnalysisPanel(...) in Workstation.mm, AFTER the cache
// status block and BEFORE the "xatlas regenerate" block.
//
// Requires:
//   - Two static panel-local flags (add next to gUvSharpPreCut):
//       static bool  gUvShowStretchHeatmap = false;
//       static int   gUvStretchMetric      = 0;   // 0 = ratio, 1 = symDir
//   - These flags are forwarded into the StructureLayer (which builds the
//     Uniforms struct in MetalRenderer). Two new bools propagate through
//     StructureLayer.cpp → MetalRenderer::renderStructureMeshes → Uniforms
//     fields `modeFlags.y` and `modeFlags.z`.

ImGui::Separator();
ImGui::TextDisabled("Stretch heatmap diagnostic");
ImGui::Checkbox("Show stretch heatmap##sh", &gUvShowStretchHeatmap);
if (gUvShowStretchHeatmap) {
    static const char* kMetricNames[] = {
        "Stretch ratio (max σ_i/σ_j) — ideal 1.0",
        "Symmetric Dirichlet energy — ideal 4.0",
    };
    ImGui::Combo("Metric##shm", &gUvStretchMetric, kMetricNames, 2);

    // Legend bar — a horizontal gradient strip + tick marks for the
    // operator to interpret the colors.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2  p0 = ImGui::GetCursorScreenPos();
    float   w  = ImGui::GetContentRegionAvail().x - 8.0f;
    float   h  = 14.0f;
    const ImU32 colors[] = {
        IM_COL32(255, 255, 255, 255),
        IM_COL32( 51, 217, 242, 255),
        IM_COL32( 77, 217,  77, 255),
        IM_COL32(242,  77,  51, 255),
    };
    // 3 horizontal gradient segments using the 4 stops.
    float seg = w / 3.0f;
    for (int i = 0; i < 3; ++i) {
        dl->AddRectFilledMultiColor(
            ImVec2(p0.x + i*seg,     p0.y),
            ImVec2(p0.x + (i+1)*seg, p0.y + h),
            colors[i], colors[i+1], colors[i+1], colors[i]);
    }
    ImGui::Dummy(ImVec2(w, h));
    ImGui::TextDisabled("white = ideal     cyan ~ minor     "
                        "green ~ moderate     red = broken");
}
