#include "ui_overlay.hpp"
#include "feature_type.hpp"
#include "ui_sectioned_list.hpp"

#include <array>
#include <memory>
#include <string>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace nasrbrowse
{

    namespace
    {
        constexpr int SEARCH_INPUT_WIDTH_PX = 240;
    }

    struct ui_overlay::impl
    {
        layer_visibility vis;
        std::string search_buf;
        std::vector<search_hit> hits;

        // Route panel state: input buffer + cached display data refreshed
        // via set_route_state() after the caller parses a submission.
        std::string route_buf;
        bool has_route = false;
        std::string route_shorthand;
        std::vector<route_leg> route_legs;
        std::string route_error;
    };

    ui_overlay::ui_overlay() : pimpl(std::make_unique<impl>()) {}
    ui_overlay::~ui_overlay() = default;

    void ui_overlay::set_route_state(const flight_route& route)
    {
        pimpl->route_error.clear();
        pimpl->has_route = true;
        pimpl->route_shorthand = route.to_text();
        pimpl->route_legs = compute_legs(route);
        // Snap the input buffer to the canonical shorthand so
        // auto-corrected entry/exit points are reflected.
        pimpl->route_buf = pimpl->route_shorthand;
    }

    void ui_overlay::clear_route_state(const std::string& error)
    {
        pimpl->has_route = false;
        pimpl->route_shorthand.clear();
        pimpl->route_legs.clear();
        pimpl->route_error = error;
    }

    void ui_overlay::set_search_results(std::vector<search_hit> hits)
    {
        pimpl->hits = std::move(hits);
    }

    const std::vector<search_hit>& ui_overlay::search_results() const
    {
        return pimpl->hits;
    }

    const layer_visibility& ui_overlay::visibility() const
    {
        return pimpl->vis;
    }

    ui_overlay_result ui_overlay::draw(
        float last_render_ms, double zoom_level,
        const std::vector<std::unique_ptr<feature_type>>& feature_types)
    {
        ImGuiIO& io = ImGui::GetIO();
        ui_overlay_result result;
        auto& d = *pimpl;

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

        // Row = (label, layer_id). Basemap sits at the top, then each
        // feature_type in registration order.
        struct row { const char* label; int id; };
        std::vector<row> rows;
        rows.reserve(feature_types.size() + 1);
        rows.push_back({"Basemap", layer_basemap});
        for(const auto& obj : feature_types)
            rows.push_back({obj->label(), obj->layer_id()});

        float max_label_w = 0;
        for(const auto& r : rows)
        {
            float w = ImGui::CalcTextSize(r.label).x;
            if(w > max_label_w) max_label_w = w;
        }

        float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        bool& changed = result.visibility_changed;

        for(const auto& r : rows)
        {
            float label_w = ImGui::CalcTextSize(r.label).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + max_label_w - label_w);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", r.label);
            ImGui::SameLine(0, spacing);

            ImGui::PushID(r.id);
            if(ImGui::Checkbox("", &d.vis[r.id]))
                changed = true;
            ImGui::PopID();
        }

        ImGui::End();

        // Altitude band filter beneath the layer checkboxes.
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x, ImGui::GetFrameHeightWithSpacing() * static_cast<float>(rows.size() + 1) + 16.0F),
            ImGuiCond_Always,
            ImVec2(1.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.6F);
        ImGui::Begin("Altitude", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

        if(ImGui::Checkbox("Below 18,000 ft", &d.vis.altitude.show_low))
            changed = true;
        if(ImGui::Checkbox("18,000 ft and above", &d.vis.altitude.show_high))
            changed = true;

        ImGui::End();

        // Search box in the top-left corner.
        ImGui::SetNextWindowPos(
            ImVec2(0, 0), ImGuiCond_Always, ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.85F);
        ImGui::Begin("##search", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings);

        ImGui::SetNextItemWidth(static_cast<float>(SEARCH_INPUT_WIDTH_PX));
        ImGui::InputTextWithHint("##search_input", "Search", &d.search_buf);
        bool input_focused = ImGui::IsItemFocused();
        result.search_query = d.search_buf;

        bool enter_pressed = input_focused &&
                             ImGui::IsKeyPressed(ImGuiKey_Enter, false);

        // Group hits by entity_type into the shared feature section list.
        std::vector<ui_section> sections(FEATURE_SECTION_COUNT);
        std::vector<std::vector<int>> section_hit_index(FEATURE_SECTION_COUNT);
        for(std::size_t s = 0; s < FEATURE_SECTION_COUNT; ++s)
            sections[s].header = FEATURE_SECTIONS[s].header;

        for(int i = 0; i < static_cast<int>(d.hits.size()); ++i)
        {
            const auto& h = d.hits[i];
            int s = feature_section_index(h.entity_type.c_str());
            if(s < 0) continue;

            std::string label;
            if(!h.ids.empty() && !h.name.empty())
                label = h.ids + "  " + h.name;
            else if(!h.ids.empty())
                label = h.ids;
            else
                label = h.name;

            sections[s].items.push_back(std::move(label));
            section_hit_index[s].push_back(i);
        }

        auto picked = draw_sectioned_selectable_list(sections);

        // Enter accepts the first visible hit across all sections.
        std::optional<int> selected_flat;
        if(picked)
            selected_flat = section_hit_index[picked->first][picked->second];
        else if(enter_pressed && !d.hits.empty())
        {
            for(std::size_t s = 0; s < FEATURE_SECTION_COUNT; ++s)
            {
                if(!section_hit_index[s].empty())
                {
                    selected_flat = section_hit_index[s][0];
                    break;
                }
            }
        }

        if(selected_flat)
        {
            result.selected_hit_index = *selected_flat;
            d.search_buf.clear();
            result.search_query.clear();
        }

        ImGui::End();

        // Route panel, top-center.
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5F, 0),
            ImGuiCond_Always,
            ImVec2(0.5F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.85F);
        ImGui::Begin("Route", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);

        ImGui::SetNextItemWidth(360.0F);
        bool submit = ImGui::InputText("##route_input",
            &d.route_buf,
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if(ImGui::Button("Set")) submit = true;
        if(submit)
            result.submit_route_text = d.route_buf;

        if(!d.route_error.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
            ImGui::TextWrapped("%s", d.route_error.c_str());
            ImGui::PopStyleColor();
        }

        if(d.has_route)
        {
            if(ImGui::Button("Clear"))
            {
                result.clear_route = true;
                d.route_buf.clear();
            }

            double total = 0;
            const ImGuiTableFlags flags =
                ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit;
            if(ImGui::BeginTable("route_legs", 4, flags))
            {
                ImGui::TableSetupColumn("From");
                ImGui::TableSetupColumn("To");
                ImGui::TableSetupColumn("Dist (nm)");
                ImGui::TableSetupColumn("Course (T)");
                ImGui::TableHeadersRow();
                for(const auto& leg : d.route_legs)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(leg.from_id.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(leg.to_id.c_str());
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.1f", leg.distance_nm);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%03.0f", leg.true_course_deg);
                    total += leg.distance_nm;
                }
                ImGui::EndTable();
            }
            ImGui::Text("Total: %.1f nm", total);
        }

        ImGui::End();
        return result;
    }

} // namespace nasrbrowse
