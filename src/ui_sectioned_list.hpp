#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace osect
{
    struct ui_section
    {
        const char* header;
        std::vector<std::string> items; // display text, already formatted
    };

    // Canonical feature sections shared by the pick popup and the search
    // dropdown. Ordered for display. `tag` matches the entity_type code
    // stored in search_fts where applicable; pick-only types (OBS, MAA,
    // RWY) have no corresponding FTS rows.
    struct feature_section_def
    {
        const char* tag;
        const char* header;
    };

    constexpr std::size_t FEATURE_SECTION_COUNT = 17;
    extern const std::array<feature_section_def, FEATURE_SECTION_COUNT> FEATURE_SECTIONS;

    // Returns the index into FEATURE_SECTIONS whose tag matches `tag`, or
    // -1 if unknown.
    int feature_section_index(const char* tag);

    // Renders a sectioned selectable list inside the current ImGui window:
    // for each non-empty section, a dimmed header row followed by a
    // Selectable per item. Returns {section_index, item_index} of the row
    // the user clicked on this frame, or nullopt.
    //
    // Empty sections are skipped. The helper owns no state; callers map the
    // returned indices back to their own data.
    std::optional<std::pair<int, int>> draw_sectioned_selectable_list(const std::vector<ui_section>& sections);

} // namespace osect
