#include "ui_popup_manager.hpp"
#include "feature_type.hpp"
#include "flight_route.hpp"
#include "map_view.hpp"
#include "ui_sectioned_list.hpp"
#include <imgui.h>
#include <imgui/scoped.hpp>
#include <string>

namespace osect
{

    namespace
    {
        // Padding between the click anchor and the popup, in pixels.
        constexpr auto POPUP_ANCHOR_PADDING = 12.0F;

        // Max width of the value column in the info popup; anything
        // longer wraps.
        constexpr auto INFO_VALUE_WRAP_PX = 280.0F;

        struct pick_state
        {
            bool open = false;
            int session_id = 0;
            std::string window_id;
            int warmup_frames = 0;
            double click_lon = 0.0;
            double click_lat = 0.0;
            std::vector<feature> features;
        };

        struct info_state
        {
            bool open = false;
            int session_id = 0;
            std::string window_id;
            int warmup_frames = 0;
            double anchor_lon = 0.0;
            double anchor_lat = 0.0;
            feature payload{airport{}};
        };

        struct route_state
        {
            bool open = false;
            int session_id = 0;
            std::string window_id;
            int warmup_frames = 0;
            double anchor_lon = 0.0;
            double anchor_lat = 0.0;
        };

        // Compute popup placement for a world-space lon/lat anchor.
        // Returns false when the anchor is off-screen — caller should
        // dismiss the popup.
        bool compute_anchor(const map_view& view, double lon, double lat, ImVec2& pos, ImVec2& pivot)
        {
            auto anchor = view.world_to_pixel(lon, lat);
            if(anchor.x < 0 || anchor.x > view.viewport_width || anchor.y < 0 || anchor.y > view.viewport_height)
            {
                return false;
            }

            auto right = anchor.x >= view.viewport_width * 0.5;
            auto bottom = anchor.y >= view.viewport_height * 0.5;

            pos.x = static_cast<float>(anchor.x + (right ? -POPUP_ANCHOR_PADDING : POPUP_ANCHOR_PADDING));
            pos.y = static_cast<float>(anchor.y + (bottom ? -POPUP_ANCHOR_PADDING : POPUP_ANCHOR_PADDING));
            pivot = ImVec2(right ? 1.0F : 0.0F, bottom ? 1.0F : 0.0F);
            return true;
        }
    } // namespace

    struct popup_manager::impl
    {
        pick_state pick;
        info_state info;
        route_state route;
    };

    popup_manager::popup_manager() : pimpl(std::make_unique<impl>())
    {
    }
    popup_manager::~popup_manager() = default;

    void popup_manager::open_pick(std::vector<feature> features, double click_lon, double click_lat)
    {
        auto& p = pimpl->pick;
        p.open = true;
        ++p.session_id;
        p.window_id = "##pick_selector_" + std::to_string(p.session_id);
        p.warmup_frames = 2;
        p.click_lon = click_lon;
        p.click_lat = click_lat;
        p.features = std::move(features);
    }

    void popup_manager::close_pick()
    {
        pimpl->pick.open = false;
        pimpl->pick.features.clear();
    }

    void popup_manager::open_info(const feature& f, double anchor_lon, double anchor_lat)
    {
        auto& p = pimpl->info;
        p.open = true;
        ++p.session_id;
        p.window_id = "##info_popup_" + std::to_string(p.session_id);
        p.warmup_frames = 2;
        p.anchor_lon = anchor_lon;
        p.anchor_lat = anchor_lat;
        p.payload = f;
    }

    void popup_manager::close_info()
    {
        pimpl->info.open = false;
    }

    void popup_manager::open_route(double anchor_lon, double anchor_lat)
    {
        auto& p = pimpl->route;
        p.anchor_lon = anchor_lon;
        p.anchor_lat = anchor_lat;
        if(!p.open)
        {
            p.open = true;
            ++p.session_id;
            p.window_id = "##route_info_popup_" + std::to_string(p.session_id);
            p.warmup_frames = 2;
        }
    }

    void popup_manager::close_route()
    {
        pimpl->route.open = false;
    }

    bool popup_manager::pick_open() const
    {
        return pimpl->pick.open;
    }
    bool popup_manager::info_open() const
    {
        return pimpl->info.open;
    }
    bool popup_manager::route_open() const
    {
        return pimpl->route.open;
    }

    namespace
    {
        // Draw the pick selector. Returns the warmup-still-running flag.
        // Dismissal/selection are reported through `out`.
        bool draw_pick(pick_state& p, popup_manager::actions& out, const map_view& view,
                       const std::vector<std::unique_ptr<feature_type>>& feature_types)
        {
            if(!p.open)
            {
                return false;
            }

            ImVec2 pos;
            ImVec2 pivot;
            if(!compute_anchor(view, p.click_lon, p.click_lat, pos, pivot))
            {
                p.open = false;
                p.features.clear();
                out.pick_dismissed = true;
                return false;
            }

            ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
            ImGui::SetNextWindowBgAlpha(0.9F);
            ImGui::SetNextWindowSizeConstraints(ImVec2(220, 0), ImVec2(FLT_MAX, view.viewport_height * 0.8F));
            imgui::scoped_window window(p.window_id.c_str(),
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);

            // Header: lat/long left, [X] right
            ImGui::Text("Lat, Long: %.5f, %.5f", p.click_lat, p.click_lon);
            if(imgui::right_aligned_close_button("X##pick_close"))
            {
                p.open = false;
                p.features.clear();
                out.pick_dismissed = true;
                return false;
            }

            if(!p.features.empty())
            {
                ImGui::Separator();

                // Group pick features by canonical feature-section tag.
                std::vector<ui_section> sections(FEATURE_SECTION_COUNT);
                std::vector<std::vector<int>> section_feature_index(FEATURE_SECTION_COUNT);
                for(std::size_t s = 0; s < FEATURE_SECTION_COUNT; ++s)
                {
                    sections.at(s).header = FEATURE_SECTIONS.at(s).header;
                }

                for(int i = 0; i < static_cast<int>(p.features.size()); ++i)
                {
                    const auto& f = p.features[i];
                    const auto& t = find_feature_type(feature_types, f);
                    auto s = feature_section_index(t.section_tag());
                    if(s < 0)
                    {
                        continue;
                    }
                    sections[s].items.push_back(t.summary(f));
                    section_feature_index[s].push_back(i);
                }

                auto picked = draw_sectioned_selectable_list(sections);
                if(picked)
                {
                    auto fi = section_feature_index[picked->first][picked->second];
                    out.pick_selected = popup_manager::pick_selection{p.features[fi], p.click_lon, p.click_lat};
                    p.open = false;
                    p.features.clear();
                }
            }

            auto need_more = p.warmup_frames > 0;
            if(need_more)
            {
                --p.warmup_frames;
            }
            return need_more;
        }

        bool draw_info(info_state& p, popup_manager::actions& out, const map_view& view,
                       const std::vector<std::unique_ptr<feature_type>>& feature_types)
        {
            if(!p.open)
            {
                return false;
            }

            ImVec2 pos;
            ImVec2 pivot;
            if(!compute_anchor(view, p.anchor_lon, p.anchor_lat, pos, pivot))
            {
                p.open = false;
                out.info_dismissed = true;
                return false;
            }

            ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
            ImGui::SetNextWindowBgAlpha(0.9F);
            ImGui::SetNextWindowSizeConstraints(ImVec2(220, 0), ImVec2(FLT_MAX, view.viewport_height * 0.9F));
            imgui::scoped_window window(p.window_id.c_str(),
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);

            // Title row: feature summary left, [X] right.
            const auto& L = find_feature_type(feature_types, p.payload);
            auto title = L.summary(p.payload);
            ImGui::TextUnformatted(title.c_str());
            if(imgui::right_aligned_close_button("X##info_close"))
            {
                p.open = false;
                out.info_dismissed = true;
                return false;
            }
            ImGui::Separator();

            // Two-column layout: fixed-width keys (auto-fit) + fixed-width
            // wrapped values. An ImGui table gives per-cell wrapping while
            // keeping the key column aligned.
            auto rows = L.info_kv(p.payload);
            const ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
            if(imgui::scoped_table table("info_kv", 2, flags); table)
            {
                ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthFixed, INFO_VALUE_WRAP_PX);

                auto line_h = ImGui::GetTextLineHeight();
                for(const auto& [key, value] : rows)
                {
                    if(value.empty())
                    {
                        continue;
                    }
                    ImGui::TableNextRow();

                    auto val_size = ImGui::CalcTextSize(value.c_str(), nullptr, false, INFO_VALUE_WRAP_PX);
                    auto row_h = val_size.y > line_h ? val_size.y : line_h;

                    ImGui::TableSetColumnIndex(0);
                    auto y_offset = (row_h - line_h) * 0.5F;
                    if(y_offset > 0.0F)
                    {
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + y_offset);
                    }
                    ImGui::TextUnformatted(key);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + INFO_VALUE_WRAP_PX);
                    auto val_w = val_size.x;
                    if(val_w < INFO_VALUE_WRAP_PX)
                    {
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (INFO_VALUE_WRAP_PX - val_w));
                    }
                    ImGui::TextUnformatted(value.c_str());
                    ImGui::PopTextWrapPos();
                }
            }

            auto need_more = p.warmup_frames > 0;
            if(need_more)
            {
                --p.warmup_frames;
            }
            return need_more;
        }

        bool draw_route(route_state& p, popup_manager::actions& out, const map_view& view, const flight_route& route)
        {
            if(!p.open)
            {
                return false;
            }

            ImVec2 pos;
            ImVec2 pivot;
            if(!compute_anchor(view, p.anchor_lon, p.anchor_lat, pos, pivot))
            {
                p.open = false;
                out.route_dismissed = true;
                return false;
            }

            ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
            ImGui::SetNextWindowBgAlpha(0.9F);
            ImGui::SetNextWindowSizeConstraints(ImVec2(220, 0), ImVec2(FLT_MAX, view.viewport_height * 0.9F));
            imgui::scoped_window window(p.window_id.c_str(),
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);

            // Title row: "Flight route" left, [X] right.
            ImGui::TextUnformatted("Flight route");
            if(imgui::right_aligned_close_button("X##route_info_close"))
            {
                p.open = false;
                out.route_dismissed = true;
                return false;
            }
            ImGui::Separator();

            auto legs = route.compute_legs();
            const auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit;
            if(imgui::scoped_table table("route_info_legs", 4, flags); table)
            {
                ImGui::TableSetupColumn("From");
                ImGui::TableSetupColumn("To");
                ImGui::TableSetupColumn("Dist (nm)");
                ImGui::TableSetupColumn("Course (T)");
                ImGui::TableHeadersRow();
                for(const auto& leg : legs)
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
                }
            }
            double total_nm = 0.0;
            for(const auto& l : legs)
            {
                total_nm += l.distance_nm;
            }
            ImGui::Text("Total: %.1f nm", total_nm);

            if(ImGui::Button("Delete route"))
            {
                p.open = false;
                out.route_delete = true;
                return false;
            }

            auto need_more = p.warmup_frames > 0;
            if(need_more)
            {
                --p.warmup_frames;
            }
            return need_more;
        }
    } // namespace

    void draw_route_drag_rubber_band(const map_view& view, const flight_route& route, bool is_segment_drag,
                                     std::size_t index)
    {
        const auto& wps = route.waypoints;
        auto cursor = ImGui::GetMousePos();
        auto* dl = ImGui::GetForegroundDrawList();
        auto col = IM_COL32(255, 255, 255, 220);

        auto draw_from = [&](std::size_t idx)
        {
            auto p = view.world_to_pixel(waypoint_lon(wps[idx]), waypoint_lat(wps[idx]));
            dl->AddLine(ImVec2(static_cast<float>(p.x), static_cast<float>(p.y)), cursor, col, 2.0F);
        };

        if(is_segment_drag)
        {
            draw_from(index);
            draw_from(index + 1);
        }
        else // waypoint: anchor to prev + next neighbors (if any)
        {
            if(index > 0)
            {
                draw_from(index - 1);
            }
            if(index + 1 < wps.size())
            {
                draw_from(index + 1);
            }
        }
    }

    popup_manager::actions popup_manager::draw(const map_view& view,
                                               const std::vector<std::unique_ptr<feature_type>>& feature_types,
                                               const flight_route* route)
    {
        actions out{};

        // Auto-close the route popup when the route disappears.
        if(!route && pimpl->route.open)
        {
            pimpl->route.open = false;
        }

        auto a = draw_pick(pimpl->pick, out, view, feature_types);
        auto b = draw_info(pimpl->info, out, view, feature_types);
        auto c = route ? draw_route(pimpl->route, out, view, *route) : false;

        out.needs_more_frames = a || b || c;
        return out;
    }

} // namespace osect
