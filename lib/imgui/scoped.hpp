#pragma once

#include <imgui.h>

namespace imgui
{

    // RAII wrapper for ImGui::Begin/End. End() runs in the destructor
    // unless the caller invoked end() first to close the window early.
    class scoped_window
    {
        bool ended = false;

    public:
        scoped_window(const char* id, ImGuiWindowFlags flags = 0);
        ~scoped_window();

        scoped_window(const scoped_window&) = delete;
        scoped_window& operator=(const scoped_window&) = delete;

        // Close the window before the destructor fires. After end(), no
        // further ImGui calls may target this window.
        void end();
    };

    // RAII wrapper for ImGui::BeginTable/EndTable. Use is_open() (or the
    // explicit bool conversion) to gate column/row calls.
    class scoped_table
    {
        bool opened;

    public:
        scoped_table(const char* id, int columns, ImGuiTableFlags flags = 0);
        ~scoped_table();

        scoped_table(const scoped_table&) = delete;
        scoped_table& operator=(const scoped_table&) = delete;

        bool is_open() const
        {
            return opened;
        }
        explicit operator bool() const
        {
            return opened;
        }
    };

    // Right-aligned small close button on the current line. Issues a
    // SameLine, advances the cursor to the right edge of the content
    // region, then draws a SmallButton. Returns true on click.
    bool right_aligned_close_button(const char* id);

} // namespace imgui
