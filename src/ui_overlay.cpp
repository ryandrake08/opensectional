#include "ui_overlay.hpp"
#include "feature_type.hpp"
#include "ui_sectioned_list.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <imgui.h>
#include <imgui/scoped.hpp>
#include <misc/cpp/imgui_stdlib.h>

namespace osect
{

    namespace
    {
        constexpr auto SEARCH_INPUT_WIDTH_PX = 240;
    }

    struct ui_overlay::impl
    {
        layer_visibility vis;
        std::string search_buf;
        std::vector<search_hit> hits;

        // Route panel state: input buffer + whether a route is active.
        // Refreshed via set_route_state() / clear_route_state().
        std::string route_buf;
        bool has_route = false;
        std::string route_error;
        // True while a sigil-bearing route is being expanded on a
        // background thread. Drives a spinner-and-label indicator in
        // the route panel.
        bool planning = false;
        // Planner knobs surfaced in the route panel.
        float max_leg_nm = 80.0F;  // ImGui::InputFloat takes float
        bool use_airways = false;

        // Data-status panel state. Empty until set_data_sources is
        // called; the panel renders nothing in that case.
        std::vector<data_source> sources;
    };

    ui_overlay::ui_overlay() : pimpl(std::make_unique<impl>()) {}
    ui_overlay::~ui_overlay() = default;

    void ui_overlay::set_route_state(const flight_route& route)
    {
        pimpl->route_error.clear();
        pimpl->has_route = true;
        // Snap the input buffer to the canonical shorthand so
        // auto-corrected entry/exit points are reflected.
        pimpl->route_buf = route.to_text();
    }

    void ui_overlay::clear_route_state(const std::string& error)
    {
        pimpl->has_route = false;
        pimpl->route_error = error;
    }

    void ui_overlay::set_route_planning(bool pending)
    {
        pimpl->planning = pending;
    }

    void ui_overlay::set_route_planner_defaults(double max_leg_nm,
                                                  bool use_airways)
    {
        pimpl->max_leg_nm = static_cast<float>(max_leg_nm);
        pimpl->use_airways = use_airways;
    }

    void ui_overlay::set_data_sources(std::vector<data_source> sources)
    {
        pimpl->sources = std::move(sources);
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
        float last_render_ms,
        const std::vector<std::unique_ptr<feature_type>>& feature_types)
    {
        auto& io = ImGui::GetIO();
        auto result = ui_overlay_result{};
        auto& d = *pimpl;

        // FPS overlay in the bottom-right corner
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x, io.DisplaySize.y),
            ImGuiCond_Always,
            ImVec2(1.0F, 1.0F));
        ImGui::SetNextWindowBgAlpha(0.4F);
        {
            imgui::scoped_window window("##fps",
                ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoInputs);
            auto fps = (last_render_ms > 0.0F) ? 1000.0F / last_render_ms : 0.0F;
            ImGui::Text("%6.1f FPS (%5.2f ms)", fps, last_render_ms);
        }

        // Layer checkboxes in the top-right corner
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x, 0),
            ImGuiCond_Always,
            ImVec2(1.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.6F);
        std::size_t row_count = 0;
        {
            imgui::scoped_window window("##layers",
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

            auto max_label_w = 0.0F;
            for(const auto& r : rows)
            {
                auto w = ImGui::CalcTextSize(r.label).x;
                max_label_w = std::max(w, max_label_w);
            }

            auto spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            bool& changed = result.visibility_changed;

            for(const auto& r : rows)
            {
                auto label_w = ImGui::CalcTextSize(r.label).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + max_label_w - label_w);
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", r.label);
                ImGui::SameLine(0, spacing);

                ImGui::PushID(r.id);
                if(ImGui::Checkbox("", &d.vis[r.id]))
                    changed = true;
                ImGui::PopID();
            }

            row_count = rows.size();
        }

        // Altitude band filter beneath the layer checkboxes.
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x, ImGui::GetFrameHeightWithSpacing() * static_cast<float>(row_count + 1) + 16.0F),
            ImGuiCond_Always,
            ImVec2(1.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.6F);
        {
            imgui::scoped_window window("Altitude",
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings);

            bool& changed = result.visibility_changed;
            if(ImGui::RadioButton("Below 18,000 ft", d.vis.altitude.show_low))
            {
                d.vis.altitude.show_low = true;
                d.vis.altitude.show_high = false;
                d.vis.altitude.show_unlimited = false;
                changed = true;
            }
            if(ImGui::RadioButton("18,000 ft and above", d.vis.altitude.show_high))
            {
                d.vis.altitude.show_low = false;
                d.vis.altitude.show_high = true;
                d.vis.altitude.show_unlimited = false;
                changed = true;
            }
            if(ImGui::RadioButton("Unlimited", d.vis.altitude.show_unlimited))
            {
                d.vis.altitude.show_low = false;
                d.vis.altitude.show_high = false;
                d.vis.altitude.show_unlimited = true;
                changed = true;
            }
        }

        // Search box in the top-left corner.
        ImGui::SetNextWindowPos(
            ImVec2(0, 0), ImGuiCond_Always, ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.85F);
        {
            imgui::scoped_window window("##search",
                ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings);

            ImGui::SetNextItemWidth(static_cast<float>(SEARCH_INPUT_WIDTH_PX));
            ImGui::InputTextWithHint("##search_input", "Search", &d.search_buf);
            auto input_focused = ImGui::IsItemFocused();
            result.search_query = d.search_buf;

            auto enter_pressed = input_focused &&
                                 ImGui::IsKeyPressed(ImGuiKey_Enter, false);

            // Group hits by entity_type into the shared feature section list.
            std::vector<ui_section> sections(FEATURE_SECTION_COUNT);
            std::vector<std::vector<int>> section_hit_index(FEATURE_SECTION_COUNT);
            for(std::size_t s = 0; s < FEATURE_SECTION_COUNT; ++s)
                sections.at(s).header = FEATURE_SECTIONS.at(s).header;

            for(int i = 0; i < static_cast<int>(d.hits.size()); ++i)
            {
                const auto& h = d.hits[i];
                auto s = feature_section_index(h.entity_type.c_str());
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
            auto selected_flat = std::optional<int>();
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
        }

        // Route panel, top-center.
        ImGui::SetNextWindowPos(
            ImVec2(io.DisplaySize.x * 0.5F, 0),
            ImGuiCond_Always,
            ImVec2(0.5F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.85F);
        {
            imgui::scoped_window window("Route",
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings);

            ImGui::BeginDisabled(d.planning);
            ImGui::SetNextItemWidth(360.0F);
            auto submit = ImGui::InputText("##route_input",
                &d.route_buf,
                ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if(ImGui::Button("Set")) submit = true;

            // Planner knobs. Always rendered so the user can change
            // them between submissions; disabled while a plan is
            // running so a mid-plan change can't desync the UI vs.
            // the in-flight options.
            ImGui::Checkbox("Use airways", &d.use_airways);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0F);
            ImGui::InputFloat("Max leg (nm)", &d.max_leg_nm, 0.0F, 0.0F, "%.0f");
            d.max_leg_nm = std::max(d.max_leg_nm, 1.0F);
            ImGui::EndDisabled();
            if(submit && !d.planning)
                result.submit_route_text = d.route_buf;
            result.route_max_leg_nm = d.max_leg_nm;
            result.route_use_airways = d.use_airways;

            if(d.planning)
            {
                // Simple animated ellipsis: one, two, or three dots
                // depending on which frame bucket we're in. Keeps the
                // indicator active-looking without a custom widget.
                static const std::array<const char*, 3> dots = {
                    "Planning route.",
                    "Planning route..",
                    "Planning route..."};
                auto bucket = static_cast<std::size_t>(
                    ImGui::GetTime() * 3.0) % dots.size();
                ImGui::TextUnformatted(dots.at(bucket));
            }
            else if(!d.route_error.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
                ImGui::TextWrapped("%s", d.route_error.c_str());
                ImGui::PopStyleColor();
            }

            if(d.has_route && !d.planning)
            {
                if(ImGui::Button("Clear"))
                {
                    result.clear_route = true;
                    d.route_buf.clear();
                }
            }
        }

        // Data status panel, pinned to the bottom-left corner.
        if(!d.sources.empty())
        {
            ImGui::SetNextWindowPos(
                ImVec2(0, io.DisplaySize.y),
                ImGuiCond_Always,
                ImVec2(0.0F, 1.0F));
            ImGui::SetNextWindowBgAlpha(0.6F);
            imgui::scoped_window window("Data status",
                ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings);

            for(const auto& s : d.sources)
            {
                // Status colors: green=fresh, red=expired, gray=unknown.
                ImU32 color = IM_COL32(180, 180, 180, 255);
                const char* tag = "?";
                switch(s.status())
                {
                case data_source_status::fresh:
                    color = IM_COL32(80, 200, 80, 255);  tag = "OK";  break;
                case data_source_status::expired:
                    color = IM_COL32(230, 90, 90, 255);  tag = "EXP"; break;
                case data_source_status::unknown:
                    break;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::Text("[%-5s]", tag);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::Text("%-6s %s", s.name.c_str(), s.info.c_str());
            }
        }

        return result;
    }

} // namespace osect
