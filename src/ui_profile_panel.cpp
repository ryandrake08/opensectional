#include "ui_profile_panel.hpp"

#include "terrain_profile.hpp"
#include <imgui.h>
#include <algorithm>
#include <optional>
#include <string>

namespace osect
{
    namespace
    {
        constexpr float DRAWER_HEIGHT_PX = 220.0F;
        constexpr float MARGIN_PX = 12.0F;
        constexpr double CLEARANCE_FT = TERRAIN_PROFILE_REQUIRED_CLEARANCE_FT;

        std::optional<double> maximum_elevation(const terrain_profile& profile)
        {
            std::optional<double> maximum;
            for(const auto& sample : profile.samples)
            {
                for(const auto elevation : {sample.centreline_elevation_ft, sample.corridor_elevation_ft,
                                             sample.aircraft_altitude_ft})
                {
                    if(elevation)
                    {
                        maximum = maximum ? std::max(*maximum, *elevation) : *elevation;
                    }
                }
                if(sample.corridor_elevation_ft)
                {
                    maximum = maximum ? std::max(*maximum, *sample.corridor_elevation_ft + CLEARANCE_FT)
                                      : *sample.corridor_elevation_ft + CLEARANCE_FT;
                }
            }
            return maximum;
        }
    }

    bool ui_profile_panel::open() const
    {
        return open_;
    }

    void ui_profile_panel::open_drawer()
    {
        open_ = true;
    }

    void ui_profile_panel::toggle()
    {
        open_ = !open_;
    }

    void ui_profile_panel::draw(const terrain_profile* profile, std::optional<std::string> error, bool has_active_route,
                                bool terrain_available, bool surface_model) const
    {
        if(!open_)
        {
            return;
        }

        const auto& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.0F, io.DisplaySize.y), ImGuiCond_Always, ImVec2(0.0F, 1.0F));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, DRAWER_HEIGHT_PX), ImGuiCond_Always);
        if(!ImGui::Begin("Terrain profile", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                     ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        if(!has_active_route)
        {
            ImGui::TextUnformatted("No active route");
            ImGui::End();
            return;
        }
        if(!terrain_available)
        {
            ImGui::TextUnformatted("Terrain data is unavailable. Install a terrain tile tree to view a profile.");
            ImGui::End();
            return;
        }
        if(error)
        {
            ImGui::TextWrapped("Terrain profile could not be prepared: %s", error->c_str());
            ImGui::End();
            return;
        }
        if(!profile || profile->samples.empty())
        {
            ImGui::TextUnformatted("Terrain profile is being prepared.");
            ImGui::End();
            return;
        }

        const auto max_distance = profile->samples.back().distance_nm;
        const auto max_elevation = std::max(1.0, maximum_elevation(*profile).value_or(0.0));
        const auto draw_list = ImGui::GetWindowDrawList();
        const auto origin = ImGui::GetCursorScreenPos();
        const auto available = ImGui::GetContentRegionAvail();
        const auto plot_min = ImVec2(origin.x + MARGIN_PX, origin.y + MARGIN_PX);
        const auto plot_max = ImVec2(origin.x + available.x - MARGIN_PX, origin.y + available.y - 30.0F);
        const auto grid = IM_COL32(150, 150, 150, 100);
        const auto text = IM_COL32(210, 210, 210, 255);
        const auto terrain_fill = IM_COL32(110, 75, 40, 130);
        const auto corridor = IM_COL32(255, 185, 70, 255);
        const auto obstacle_corridor = IM_COL32(130, 130, 130, 255);
        const auto clearance = IM_COL32(255, 185, 70, 55);
        const auto obstacle_clearance = IM_COL32(130, 130, 130, 55);
        const auto centreline = IM_COL32(190, 135, 75, 255);
        const auto climb = IM_COL32(90, 210, 255, 255);
        const auto cruise = IM_COL32(255, 255, 255, 255);
        const auto descent = IM_COL32(255, 110, 220, 255);
        const auto point_at = [=](double distance_nm, double elevation_ft)
        {
            const auto x_fraction = max_distance > 0.0 ? static_cast<float>(distance_nm / max_distance) : 0.0F;
            const auto y_fraction = static_cast<float>(elevation_ft / max_elevation);
            return ImVec2(plot_min.x + (plot_max.x - plot_min.x) * x_fraction,
                          plot_max.y - (plot_max.y - plot_min.y) * y_fraction);
        };

        for(int i = 0; i <= 4; ++i)
        {
            const auto fraction = static_cast<float>(i) / 4.0F;
            const auto y = plot_max.y - (plot_max.y - plot_min.y) * fraction;
            draw_list->AddLine(ImVec2(plot_min.x, y), ImVec2(plot_max.x, y), grid);
            const auto label = std::to_string(static_cast<int>(max_elevation * fraction));
            draw_list->AddText(ImVec2(origin.x, y - ImGui::GetTextLineHeight() * 0.5F), text, label.c_str());
        }
        for(int i = 0; i <= 6; ++i)
        {
            const auto fraction = static_cast<float>(i) / 6.0F;
            const auto x = plot_min.x + (plot_max.x - plot_min.x) * fraction;
            draw_list->AddLine(ImVec2(x, plot_min.y), ImVec2(x, plot_max.y), grid);
            const auto label = std::to_string(static_cast<int>(max_distance * fraction));
            draw_list->AddText(ImVec2(x, plot_max.y + 3.0F), text, label.c_str());
        }
        draw_list->AddLine(ImVec2(plot_min.x, plot_max.y), plot_min, text);
        draw_list->AddLine(ImVec2(plot_min.x, plot_max.y), plot_max, text);

        // Each contiguous run is drawn independently so unavailable terrain
        // remains a visible gap rather than a fabricated elevation.
        ImVector<ImVec2> run;
        const auto flush_terrain = [&]()
        {
            if(run.empty())
            {
                return;
            }
            for(int i = 1; i < run.Size; ++i)
            {
                draw_list->AddQuadFilled(run[i - 1], run[i], ImVec2(run[i].x, plot_max.y),
                                         ImVec2(run[i - 1].x, plot_max.y), terrain_fill);
            }
            draw_list->AddPolyline(run.Data, run.Size, centreline, ImDrawFlags_None, 1.5F);
            run.clear();
        };
        for(const auto& sample : profile->samples)
        {
            if(sample.centreline_elevation_ft)
            {
                run.push_back(point_at(sample.distance_nm, *sample.centreline_elevation_ft));
            }
            else
            {
                flush_terrain();
            }
        }
        flush_terrain();

        for(std::size_t i = 1; i < profile->samples.size(); ++i)
        {
            const auto& previous = profile->samples[i - 1];
            const auto& current = profile->samples[i];
            if(!previous.corridor_elevation_ft || !current.corridor_elevation_ft)
            {
                continue;
            }
            const bool from_obstacle = previous.corridor_from_obstacle || current.corridor_from_obstacle;
            const auto line_color = from_obstacle ? obstacle_corridor : corridor;
            const auto band_color = from_obstacle ? obstacle_clearance : clearance;
            const auto previous_lower = point_at(previous.distance_nm, *previous.corridor_elevation_ft);
            const auto current_lower = point_at(current.distance_nm, *current.corridor_elevation_ft);
            draw_list->AddQuadFilled(previous_lower, current_lower,
                                     point_at(current.distance_nm, *current.corridor_elevation_ft + CLEARANCE_FT),
                                     point_at(previous.distance_nm, *previous.corridor_elevation_ft + CLEARANCE_FT),
                                     band_color);
            draw_list->AddLine(previous_lower, current_lower, line_color, 2.0F);
        }

        for(std::size_t i = 1; i < profile->samples.size(); ++i)
        {
            const auto& previous = profile->samples[i - 1];
            const auto& current = profile->samples[i];
            if(!previous.aircraft_altitude_ft || !current.aircraft_altitude_ft)
            {
                continue;
            }
            const auto difference = *current.aircraft_altitude_ft - *previous.aircraft_altitude_ft;
            const auto color = difference > 1e-6 ? climb : difference < -1e-6 ? descent : cruise;
            draw_list->AddLine(point_at(previous.distance_nm, *previous.aircraft_altitude_ft),
                               point_at(current.distance_nm, *current.aircraft_altitude_ft), color, 2.5F);
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, plot_max.y + ImGui::GetTextLineHeight() + 6.0F));
        ImGui::TextUnformatted(surface_model ? "Elevation source: DSM  |  terrain: brown  corridor: orange  obstacle corridor: gray  aircraft: blue/white/magenta"
                                             : "Elevation source: DTM  |  terrain: brown  corridor: orange  obstacle corridor: gray  aircraft: blue/white/magenta");
        ImGui::End();
    }
} // namespace osect
