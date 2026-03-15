#include "ui_overlay.hpp"
#include <cstdio>
#include <imgui.h>

namespace nasrbrowse
{

    ui_overlay::ui_overlay() = default;

    bool ui_overlay::draw(float last_render_ms, double zoom_level)
    {
        ImGuiIO& io = ImGui::GetIO();

        // Zoom level overlay in the bottom-left corner
        ImGui::SetNextWindowPos(
            ImVec2(0, io.DisplaySize.y),
            ImGuiCond_Always,
            ImVec2(0.0F, 1.0F));
        ImGui::SetNextWindowBgAlpha(0.4F);
        ImGui::Begin("##zoom", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoInputs);
        ImGui::Text("z%.1f", zoom_level);
        ImGui::End();

        // FPS overlay in the bottom-right corner
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x, io.DisplaySize.y),
            ImGuiCond_Always,
            ImVec2(1.0F, 1.0F));
        ImGui::SetNextWindowBgAlpha(0.4F);
        ImGui::Begin("##fps", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoInputs);
        float fps = (last_render_ms > 0.0F) ? 1000.0F / last_render_ms : 0.0F;
        ImGui::Text("%6.1f FPS (%5.2f ms)", fps, last_render_ms);
        ImGui::End();

        // Layer checkboxes in the top-right corner
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x, 0),
            ImGuiCond_Always,
            ImVec2(1.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.6F);
        ImGui::Begin("##layers", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings);

        struct entry { const char* label; bool* value; };
        entry entries[] = {
            {"Basemap",   &vis.basemap},
            {"Airports",  &vis.airports},
            {"Runways",   &vis.runways},
            {"Navaids",   &vis.navaids},
            {"Fixes",     &vis.fixes},
            {"Airways",   &vis.airways},
            {"Airspace",  &vis.airspace},
            {"SUA",       &vis.sua},
            {"Obstacles", &vis.obstacles},
        };

        // Measure the widest label
        float max_label_w = 0;
        for(const auto& e : entries)
        {
            float w = ImGui::CalcTextSize(e.label).x;
            if(w > max_label_w) max_label_w = w;
        }

        float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        bool changed = false;

        for(const auto& e : entries)
        {
            float label_w = ImGui::CalcTextSize(e.label).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + max_label_w - label_w);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", e.label);
            ImGui::SameLine(0, spacing);

            char id[32];
            snprintf(id, sizeof(id), "##%s", e.label);
            if(ImGui::Checkbox(id, e.value))
            {
                changed = true;
            }
        }

        ImGui::End();
        return changed;
    }

    const layer_visibility& ui_overlay::visibility() const
    {
        return vis;
    }

} // namespace nasrbrowse
