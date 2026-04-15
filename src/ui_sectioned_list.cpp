#include "ui_sectioned_list.hpp"

#include <cstdio>
#include <cstring>
#include <imgui.h>

namespace nasrbrowse
{
    const feature_section_def FEATURE_SECTIONS[] = {
        {"APT",   "AIRPORTS"},
        {"RWY",   "RUNWAYS"},
        {"NAV",   "NAVAIDS"},
        {"FIX",   "FIXES"},
        {"OBS",   "OBSTACLES"},
        {"AWOS",  "WEATHER STATIONS"},
        {"COM",   "COMM OUTLETS"},
        {"AWY",   "AIRWAYS"},
        {"MTR",   "MILITARY ROUTES"},
        {"CLS",   "CLASS AIRSPACE"},
        {"SUA",   "SPECIAL USE AIRSPACE"},
        {"MAA",   "MILITARY ALTITUDE AREAS"},
        {"PJA",   "PARACHUTE JUMP AREAS"},
        {"ADIZ",  "ADIZ"},
        {"ARTCC", "ARTCC"},
        {"FSS",   "FLIGHT SERVICE"},
    };
    const std::size_t FEATURE_SECTION_COUNT =
        sizeof(FEATURE_SECTIONS) / sizeof(FEATURE_SECTIONS[0]);

    int feature_section_index(const char* tag)
    {
        for(std::size_t i = 0; i < FEATURE_SECTION_COUNT; ++i)
            if(std::strcmp(FEATURE_SECTIONS[i].tag, tag) == 0)
                return static_cast<int>(i);
        return -1;
    }
    std::optional<std::pair<int, int>> draw_sectioned_selectable_list(
        const std::vector<ui_section>& sections)
    {
        std::optional<std::pair<int, int>> result;
        for(int s = 0; s < static_cast<int>(sections.size()); ++s)
        {
            const auto& sec = sections[s];
            if(sec.items.empty()) continue;

            ImGui::TextDisabled("%s", sec.header);
            for(int i = 0; i < static_cast<int>(sec.items.size()); ++i)
            {
                char id[32];
                std::snprintf(id, sizeof(id), "##sec_%d_%d", s, i);
                ImGui::PushID(id);
                if(ImGui::Selectable(sec.items[i].c_str(), false))
                    result = std::make_pair(s, i);
                ImGui::PopID();
            }
        }
        return result;
    }
} // namespace nasrbrowse
