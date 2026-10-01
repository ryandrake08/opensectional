#include "ui_overlay.hpp"
#include "feature_type.hpp"
#include "route_plan_options.hpp"
#include "ui_profile_panel.hpp"
#include "ui_sectioned_list.hpp"
#include <imgui.h>
#include <imgui/scoped.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <misc/cpp/imgui_stdlib.h>

namespace osect
{

    namespace
    {
        constexpr auto SEARCH_INPUT_WIDTH_PX = 240;

        std::string format_cruise_altitude(double altitude_ft)
        {
            auto text = std::to_string(altitude_ft);
            // Trim the trailing zeros from std::to_string's fixed format.
            text.erase(text.find_last_not_of('0') + 1);
            if(text.back() == '.')
            {
                text.pop_back();
            }
            return text;
        }
    }

    struct route_panel
    {
        std::uint64_t id = 0;
        std::string text_buf;
        std::string error;
        bool has_route = false;
        // True while a sigil-bearing route is being expanded on a
        // background thread for this panel. Drives a spinner shown
        // inside the tab's body. Stays anchored to the originating
        // tab even if the user switches tabs while in flight.
        bool planning = false;
        // Per-tab planner knobs.
        double max_leg_nm = default_max_leg_length_nm;
        bool use_airways = default_use_airways;
        bool avoid_terrain = default_avoid_terrain;
        // NM of planned legs whose terrain was not checked; 0 for none.
        double terrain_unchecked_nm = 0.0;
        std::string cruise_altitude_text;
        std::optional<double> cruise_altitude_ft;
    };

    struct ui_overlay::impl
    {
        layer_visibility vis;
        std::string search_buf;
        // Last search_buf value emitted via ui_overlay_result::search_query.
        // Used to suppress redundant per-frame emissions while the box text
        // is unchanged, so callers don't re-run the FTS query at frame rate.
        std::string last_search_query;
        std::vector<search_hit> hits;
        terrain_shading terrain_mode = terrain_shading::hypsometric;
        ui_profile_panel profile_panel;

        // Route panel tabs. Always at least one — closing the last
        // re-creates a fresh empty panel so the panel UI never
        // disappears.
        std::vector<route_panel> panels;
        std::uint64_t next_panel_id = 1;
        std::size_t active_panel_index = 0;
        // Last value of the active panel's id reported through
        // result.active_tab_changed; used to detect transitions.
        std::uint64_t last_reported_active_id = 0;

        // True until the first draw completes. Used to force-select
        // the active tab on the first frame so its body renders and
        // the auto-resized panel snaps to its full width — without
        // this, no tab is selected on frame 1 and the window briefly
        // appears ~10 px wide before it draws content on frame 2.
        bool needs_initial_tab_selection = true;

        // Data-status panel state. Empty until set_data_sources is
        // called; the panel renders nothing in that case.
        std::vector<data_source> sources;

        std::size_t find_panel(std::uint64_t id) const
        {
            for(std::size_t i = 0; i < panels.size(); ++i)
            {
                if(panels[i].id == id)
                {
                    return i;
                }
            }
            return panels.size();
        }

        route_panel make_panel()
        {
            route_panel p;
            p.id = next_panel_id++;
            return p;
        }
    };

    ui_overlay::ui_overlay() : pimpl(std::make_unique<impl>())
    {
        pimpl->panels.push_back(pimpl->make_panel());
        // last_reported_active_id stays 0 so the first draw emits an
        // active_tab_changed event for the initial panel — gives the
        // caller a chance to register the tab id before any other
        // events reference it.
    }
    ui_overlay::~ui_overlay() = default;

    std::uint64_t ui_overlay::add_route_tab(const flight_route& route, std::optional<double> cruise_altitude_ft)
    {
        auto& d = *pimpl;
        // Absorb the pristine starter panel rather than appending a
        // sibling, so loading N saved routes produces N tabs with no
        // leading empty.
        if(d.panels.size() == 1 && !d.panels.front().has_route && !d.panels.front().planning &&
           d.panels.front().text_buf.empty())
        {
            auto& p = d.panels.front();
            p.has_route = true;
            p.error.clear();
            p.text_buf = route.to_text();
            p.cruise_altitude_ft = cruise_altitude_ft;
            p.cruise_altitude_text = cruise_altitude_ft ? format_cruise_altitude(*cruise_altitude_ft) : "";
            return p.id;
        }
        auto p = d.make_panel();
        p.has_route = true;
        p.text_buf = route.to_text();
        p.cruise_altitude_ft = cruise_altitude_ft;
        p.cruise_altitude_text = cruise_altitude_ft ? format_cruise_altitude(*cruise_altitude_ft) : "";
        auto id = p.id;
        d.panels.push_back(std::move(p));
        return id;
    }

    void ui_overlay::set_route_state(std::uint64_t tab_id, const flight_route& route)
    {
        auto i = pimpl->find_panel(tab_id);
        if(i >= pimpl->panels.size())
        {
            return;
        }
        auto& p = pimpl->panels[i];
        p.error.clear();
        p.terrain_unchecked_nm = 0.0;
        p.has_route = true;
        // Snap the input buffer to the canonical shorthand so
        // auto-corrected entry/exit points are reflected.
        p.text_buf = route.to_text();
    }

    void ui_overlay::clear_route_state(std::uint64_t tab_id, const std::string& error)
    {
        auto i = pimpl->find_panel(tab_id);
        if(i >= pimpl->panels.size())
        {
            return;
        }
        auto& p = pimpl->panels[i];
        p.has_route = false;
        p.error = error;
        p.terrain_unchecked_nm = 0.0;
    }

    void ui_overlay::set_route_terrain_unchecked(std::uint64_t tab_id, double unchecked_nm)
    {
        auto i = pimpl->find_panel(tab_id);
        if(i >= pimpl->panels.size())
        {
            return;
        }
        pimpl->panels[i].terrain_unchecked_nm = unchecked_nm;
    }

    void ui_overlay::set_route_planning(std::uint64_t tab_id, bool pending)
    {
        auto i = pimpl->find_panel(tab_id);
        if(i >= pimpl->panels.size())
        {
            return;
        }
        pimpl->panels[i].planning = pending;
    }

    void ui_overlay::close_tab(std::uint64_t tab_id)
    {
        auto i = pimpl->find_panel(tab_id);
        if(i >= pimpl->panels.size())
        {
            return;
        }
        pimpl->panels.erase(pimpl->panels.begin() + static_cast<std::ptrdiff_t>(i));
        if(pimpl->panels.empty())
        {
            pimpl->panels.push_back(pimpl->make_panel());
        }
        if(pimpl->active_panel_index >= pimpl->panels.size())
        {
            pimpl->active_panel_index = pimpl->panels.size() - 1;
        }
    }

    bool ui_overlay::has_tab(std::uint64_t tab_id) const
    {
        return pimpl->find_panel(tab_id) < pimpl->panels.size();
    }

    void ui_overlay::set_active_tab(std::uint64_t tab_id)
    {
        auto i = pimpl->find_panel(tab_id);
        if(i >= pimpl->panels.size())
        {
            return;
        }
        pimpl->active_panel_index = i;
        // Force the next draw to override ImGui's internal tab
        // selection so the visible tab matches our intent.
        pimpl->needs_initial_tab_selection = true;
        // The caller already handled the side effects of the active
        // change (set_active_route / select_route on map_widget), so
        // suppress the redundant active_tab_changed event next draw.
        pimpl->last_reported_active_id = tab_id;
    }

    void ui_overlay::set_route_planner_defaults(double max_leg_nm, bool use_airways, bool avoid_terrain)
    {
        // Seed the initial panel created at construction so the app
        // starts with the user's configured defaults. Only the
        // pristine starter panel is touched; thereafter each tab's
        // knobs are its own and new tabs inherit from the active tab.
        if(pimpl->panels.size() == 1 && !pimpl->panels.front().has_route && !pimpl->panels.front().planning &&
           pimpl->panels.front().text_buf.empty())
        {
            pimpl->panels.front().max_leg_nm = max_leg_nm;
            pimpl->panels.front().use_airways = use_airways;
            pimpl->panels.front().avoid_terrain = avoid_terrain;
        }
    }

    std::optional<double> ui_overlay::cruise_altitude_ft(std::uint64_t tab_id) const
    {
        const auto i = pimpl->find_panel(tab_id);
        if(i >= pimpl->panels.size())
        {
            return std::nullopt;
        }
        return pimpl->panels[i].cruise_altitude_ft;
    }

    void ui_overlay::set_terrain_shading(terrain_shading mode)
    {
        pimpl->terrain_mode = mode;
    }

    void ui_overlay::open_terrain_profile()
    {
        pimpl->profile_panel.open_drawer();
    }

    bool ui_overlay::terrain_profile_open() const
    {
        return pimpl->profile_panel.open();
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

    ui_overlay_result ui_overlay::draw(float last_render_ms,
                                       const std::vector<std::unique_ptr<feature_type>>& feature_types,
                                       bool terrain_available, const terrain_profile* profile, bool has_active_route,
                                       std::optional<std::string> profile_error, bool terrain_surface_model)
    {
        auto& io = ImGui::GetIO();
        auto result = ui_overlay_result{};
        auto& d = *pimpl;

        // The profile drawer owns the entire bottom edge while open.
        if(!d.profile_panel.open())
        {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x, io.DisplaySize.y), ImGuiCond_Always, ImVec2(1.0F, 1.0F));
            ImGui::SetNextWindowBgAlpha(0.4F);
            imgui::scoped_window window("##fps", ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                     ImGuiWindowFlags_NoInputs);
            auto fps = (last_render_ms > 0.0F) ? 1000.0F / last_render_ms : 0.0F;
            ImGui::Text("%6.1f FPS (%5.2f ms)", fps, last_render_ms);
        }

        // Layer checkboxes in the top-right corner
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x, 0), ImGuiCond_Always, ImVec2(1.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.6F);
        float layers_height = 0.0F;
        {
            imgui::scoped_window window("##layers", ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

            // Row = (label, layer_id). Basemap sits at the top, then each
            // feature_type in registration order.
            struct row
            {
                const char* label;
                int id;
                bool available;
            };
            std::vector<row> rows;
            rows.reserve(feature_types.size() + 2);
            rows.push_back({"Basemap", layer_basemap, true});
            rows.push_back({"Terrain", layer_terrain, terrain_available});
            for(const auto& obj : feature_types)
            {
                rows.push_back({obj->label(), obj->layer_id(), true});
            }

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
                ImGui::BeginDisabled(!r.available);
                if(ImGui::Checkbox("", &d.vis[r.id]))
                {
                    changed = true;
                }
                ImGui::EndDisabled();
                if(!r.available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("Terrain data is unavailable");
                }
                ImGui::PopID();
            }

            layers_height = ImGui::GetWindowSize().y;
        }

        // Altitude band filter beneath the layer checkboxes.
        const auto altitude_y = layers_height + 16.0F;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x, altitude_y), ImGuiCond_Always, ImVec2(1.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.6F);
        float altitude_height = 0.0F;
        {
            imgui::scoped_window window("Altitude", ImGuiWindowFlags_AlwaysAutoResize |
                                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
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
            altitude_height = ImGui::GetWindowSize().y;
        }

        // Terrain shading control beneath the altitude window.
        const auto terrain_shading_y = altitude_y + altitude_height + 16.0F;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x, terrain_shading_y), ImGuiCond_Always, ImVec2(1.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.6F);
        float terrain_shading_height = 0.0F;
        {
            imgui::scoped_window window("Terrain shading",
                                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

            ImGui::BeginDisabled(!terrain_available);
            if(ImGui::RadioButton("Hillshade", d.terrain_mode == terrain_shading::hillshade))
            {
                d.terrain_mode = terrain_shading::hillshade;
                result.terrain_shading_changed = d.terrain_mode;
            }
            if(ImGui::RadioButton("Hypsometric", d.terrain_mode == terrain_shading::hypsometric))
            {
                d.terrain_mode = terrain_shading::hypsometric;
                result.terrain_shading_changed = d.terrain_mode;
            }
            if(ImGui::RadioButton("Cruise-relative", d.terrain_mode == terrain_shading::cruise_relative))
            {
                d.terrain_mode = terrain_shading::cruise_relative;
                result.terrain_shading_changed = d.terrain_mode;
            }
            ImGui::EndDisabled();
            terrain_shading_height = ImGui::GetWindowSize().y;
        }

        // Chart-type filter beneath the terrain-shading window.
        const auto chart_y = terrain_shading_y + terrain_shading_height + 16.0F;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x, chart_y), ImGuiCond_Always, ImVec2(1.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.6F);
        {
            imgui::scoped_window window("Chart", ImGuiWindowFlags_AlwaysAutoResize |
                                                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                                     ImGuiWindowFlags_NoSavedSettings);

            bool& changed = result.visibility_changed;
            if(ImGui::RadioButton("Sectional", d.vis.chart == chart_type::sectional))
            {
                d.vis.chart = chart_type::sectional;
                changed = true;
            }
            if(ImGui::RadioButton("IFR Low", d.vis.chart == chart_type::ifr_low))
            {
                d.vis.chart = chart_type::ifr_low;
                changed = true;
            }
            if(ImGui::RadioButton("IFR High", d.vis.chart == chart_type::ifr_high))
            {
                d.vis.chart = chart_type::ifr_high;
                changed = true;
            }
        }

        // Search box in the top-left corner.
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always, ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.85F);
        {
            imgui::scoped_window window("##search", ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                                        ImGuiWindowFlags_NoSavedSettings);

            ImGui::SetNextItemWidth(static_cast<float>(SEARCH_INPUT_WIDTH_PX));
            ImGui::InputTextWithHint("##search_input", "Search", &d.search_buf);
            auto input_focused = ImGui::IsItemFocused();

            auto enter_pressed = input_focused && ImGui::IsKeyPressed(ImGuiKey_Enter, false);

            // Group hits by entity_type into the shared feature section list.
            std::vector<ui_section> sections(FEATURE_SECTION_COUNT);
            std::vector<std::vector<int>> section_hit_index(FEATURE_SECTION_COUNT);
            for(std::size_t s = 0; s < FEATURE_SECTION_COUNT; ++s)
            {
                sections.at(s).header = FEATURE_SECTIONS.at(s).header;
            }

            for(int i = 0; i < static_cast<int>(d.hits.size()); ++i)
            {
                const auto& h = d.hits[i];
                auto s = feature_section_index(h.entity_type.c_str());
                if(s < 0)
                {
                    continue;
                }

                std::string label;
                if(!h.ids.empty() && !h.name.empty())
                {
                    label = h.ids + "  " + h.name;
                }
                else if(!h.ids.empty())
                {
                    label = h.ids;
                }
                else
                {
                    label = h.name;
                }

                sections[s].items.push_back(std::move(label));
                section_hit_index[s].push_back(i);
            }

            auto picked = draw_sectioned_selectable_list(sections);

            // Enter accepts the first visible hit across all sections.
            auto selected_flat = std::optional<int>();
            if(picked)
            {
                selected_flat = section_hit_index[picked->first][picked->second];
            }
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
                // Selection clears the box; sync last_search_query to "" so
                // the now-empty buffer doesn't fire a redundant search-query
                // emission on this frame or the next. The caller's selection
                // handler is responsible for clearing displayed results.
                d.last_search_query.clear();
            }
            else if(d.search_buf != d.last_search_query)
            {
                d.last_search_query = d.search_buf;
                result.search_query = d.search_buf;
            }
        }

        // Route panel, top-center. Tab strip lets the user keep
        // multiple routes open and switch between them; trailing "+"
        // creates a new empty panel.
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5F, 0), ImGuiCond_Always, ImVec2(0.5F, 0.0F));
        ImGui::SetNextWindowBgAlpha(0.85F);
        {
            imgui::scoped_window window("Route", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

            std::optional<std::size_t> close_index;

            // Captured before BeginTabBar's body. ImGui's SetSelected
            // only queues the focus change for next frame's
            // TabBarLayout, so this frame's BeginTabItem still
            // returns true for the prior visible tab and clobbers
            // d.active_panel_index inside the loop. Using a captured
            // index keeps the SetSelected request aligned with our
            // intended tab regardless of iteration order, and lets
            // us restore d.active_panel_index after the loop so the
            // active_id comparison doesn't fire a spurious
            // active_tab_changed event.
            auto desired_active = d.active_panel_index;
            auto force_select = d.needs_initial_tab_selection;

            if(ImGui::BeginTabBar("##route_tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs))
            {
                for(std::size_t i = 0; i < d.panels.size(); ++i)
                {
                    auto& p = d.panels[i];
                    bool tab_open = true;
                    auto label = "Route " + std::to_string(i + 1) + "###tab" + std::to_string(p.id);
                    ImGuiTabItemFlags tab_flags = ImGuiTabItemFlags_None;
                    if(force_select && i == desired_active)
                    {
                        tab_flags |= ImGuiTabItemFlags_SetSelected;
                    }
                    if(ImGui::BeginTabItem(label.c_str(), &tab_open, tab_flags))
                    {
                        d.active_panel_index = i;

                        ImGui::BeginDisabled(p.planning);
                        ImGui::SetNextItemWidth(360.0F);
                        ImGui::PushID(static_cast<int>(p.id));
                        auto submit =
                            ImGui::InputText("##route_input", &p.text_buf, ImGuiInputTextFlags_EnterReturnsTrue);
                        ImGui::SameLine();
                        if(ImGui::Button("Set"))
                        {
                            submit = true;
                        }

                        ImGui::Checkbox("Use airways", &p.use_airways);
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(90.0F);
                        ImGui::InputDouble("Max leg (nm)", &p.max_leg_nm, 0.0, 0.0, "%.0f");
                        // Without terrain data the option is off and
                        // greyed, with the reason beside it.
                        ImGui::BeginDisabled(!terrain_available);
                        bool avoid_terrain = terrain_available && p.avoid_terrain;
                        if(ImGui::Checkbox("Avoid terrain", &avoid_terrain))
                        {
                            p.avoid_terrain = avoid_terrain;
                        }
                        ImGui::EndDisabled();
                        if(!terrain_available)
                        {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(no terrain data installed)");
                        }
                        ImGui::SetNextItemWidth(120.0F);
                        const auto altitude_text_before = p.cruise_altitude_text;
                        if(ImGui::InputText("Cruise altitude (ft)", &p.cruise_altitude_text))
                        {
                            if(p.cruise_altitude_text.empty())
                            {
                                p.cruise_altitude_ft.reset();
                            }
                            else
                            {
                                try
                                {
                                    std::size_t consumed = 0;
                                    const auto altitude = std::stod(p.cruise_altitude_text, &consumed);
                                    if(consumed != p.cruise_altitude_text.size() || !std::isfinite(altitude) ||
                                       altitude < 0.0)
                                    {
                                        throw std::invalid_argument("invalid cruise altitude");
                                    }
                                    p.cruise_altitude_ft = altitude;
                                }
                                catch(const std::exception&)
                                {
                                    p.cruise_altitude_text = altitude_text_before;
                                }
                            }
                            result.cruise_altitude_changed = std::pair{p.id, p.cruise_altitude_ft};
                        }
                        ImGui::EndDisabled();

                        if(submit && !p.planning)
                        {
                            ui_overlay_result::route_submit_request req;
                            req.tab_id = p.id;
                            req.text = p.text_buf;
                            req.max_leg_nm = p.max_leg_nm;
                            req.use_airways = p.use_airways;
                            req.avoid_terrain = terrain_available && p.avoid_terrain;
                            result.route_submit = std::move(req);
                        }

                        if(p.planning)
                        {
                            static const std::array<const char*, 3> dots = {"Planning route.", "Planning route..",
                                                                            "Planning route..."};
                            auto bucket = static_cast<std::size_t>(ImGui::GetTime() * 3.0) % dots.size();
                            ImGui::TextUnformatted(dots.at(bucket));
                        }
                        else if(!p.error.empty())
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
                            ImGui::TextWrapped("%s", p.error.c_str());
                            ImGui::PopStyleColor();
                        }
                        else if(p.terrain_unchecked_nm > 0.0)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 60, 255));
                            ImGui::TextWrapped("Terrain not checked along %.0f nm of planned legs: no terrain data "
                                               "there.",
                                               std::ceil(p.terrain_unchecked_nm));
                            ImGui::PopStyleColor();
                        }

                        if(p.has_route && !p.planning)
                        {
                            const bool incomplete_profile =
                                profile &&
                                std::any_of(
                                    profile->samples.begin(), profile->samples.end(), [](const auto& sample)
                                    { return !sample.centreline_elevation_ft || !sample.corridor_elevation_ft; });
                            const bool has_violations = profile && !profile->clearance_spans.empty();
                            const char* label = "Terrain profile";
                            ImU32 color = IM_COL32(255, 255, 255, 255);
                            if(!terrain_available || profile_error || incomplete_profile)
                            {
                                label = "Terrain unavailable";
                                color = IM_COL32(150, 150, 150, 255);
                            }
                            else if(!profile)
                            {
                                label = "Calculating terrain";
                            }
                            else if(has_violations)
                            {
                                color = IM_COL32(255, 80, 80, 255);
                            }
                            else
                            {
                                color = IM_COL32(80, 220, 100, 255);
                            }
                            ImGui::PushStyleColor(ImGuiCol_Text, color);
                            if(ImGui::Button(label))
                            {
                                d.profile_panel.toggle();
                                result.terrain_profile_open_changed = true;
                            }
                            ImGui::PopStyleColor();
                            ImGui::SameLine();
                            if(ImGui::Button("Clear"))
                            {
                                p.text_buf.clear();
                                ui_overlay_result::route_submit_request req;
                                req.tab_id = p.id;
                                req.max_leg_nm = p.max_leg_nm;
                                req.use_airways = p.use_airways;
                                req.avoid_terrain = false;
                                result.route_submit = std::move(req);
                            }
                        }
                        ImGui::PopID();
                        ImGui::EndTabItem();
                    }
                    if(!tab_open)
                    {
                        close_index = i;
                    }
                }

                if(ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
                {
                    auto p = d.make_panel();
                    // New tabs inherit the active tab's planner knobs
                    // rather than ini defaults, so opening a series of
                    // tabs carries the user's last-used settings.
                    p.max_leg_nm = d.panels[d.active_panel_index].max_leg_nm;
                    p.use_airways = d.panels[d.active_panel_index].use_airways;
                    p.avoid_terrain = d.panels[d.active_panel_index].avoid_terrain;
                    p.cruise_altitude_text = d.panels[d.active_panel_index].cruise_altitude_text;
                    p.cruise_altitude_ft = d.panels[d.active_panel_index].cruise_altitude_ft;
                    d.panels.push_back(std::move(p));
                    // BeginTabBar's AutoSelectNewTabs flag will switch
                    // focus to the new panel on the next frame; record
                    // the new index now so the change is observed.
                    d.active_panel_index = d.panels.size() - 1;
                }

                ImGui::EndTabBar();
            }

            if(close_index)
            {
                auto closed_id = d.panels.at(*close_index).id;
                d.panels.erase(d.panels.begin() + static_cast<std::ptrdiff_t>(*close_index));
                if(d.panels.empty())
                {
                    d.panels.push_back(d.make_panel());
                }
                if(d.active_panel_index >= d.panels.size())
                {
                    d.active_panel_index = d.panels.size() - 1;
                }
                result.tab_closed = closed_id;
            }

            // Restore the intended active index after the loop —
            // see the captured-desired_active comment above.
            if(force_select && desired_active < d.panels.size())
            {
                d.active_panel_index = desired_active;
            }

            auto active_id = d.panels.at(d.active_panel_index).id;
            if(active_id != d.last_reported_active_id)
            {
                result.active_tab_changed = active_id;
                d.last_reported_active_id = active_id;
            }
            d.needs_initial_tab_selection = false;
        }

        // Data status panel, pinned to the bottom-left corner.
        if(!d.profile_panel.open() && !d.sources.empty())
        {
            ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y), ImGuiCond_Always, ImVec2(0.0F, 1.0F));
            ImGui::SetNextWindowBgAlpha(0.6F);
            imgui::scoped_window window(
                "Data status", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

            for(const auto& s : d.sources)
            {
                // Status colors: yellow=updating, green=fresh,
                // red=expired, gray=unknown. UPD takes priority over
                // status() so an in-flight refresh is visible no matter
                // what the underlying data state is.
                ImU32 color = IM_COL32(180, 180, 180, 255);
                const char* tag = "UNK";
                if(s.updating)
                {
                    color = IM_COL32(220, 200, 70, 255);
                    tag = "UPD";
                }
                else
                {
                    switch(s.status())
                    {
                    case data_source_status::fresh:
                        color = IM_COL32(80, 200, 80, 255);
                        tag = "CUR";
                        break;
                    case data_source_status::expired:
                        color = IM_COL32(230, 90, 90, 255);
                        tag = "EXP";
                        break;
                    case data_source_status::unknown:
                        break;
                    }
                }
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::Text("[%s]", tag);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::Text("%-7s %s", s.name.c_str(), s.info.c_str());
            }
        }

        d.profile_panel.draw(profile, std::move(profile_error), has_active_route, terrain_available,
                             terrain_surface_model);

        return result;
    }

} // namespace osect
