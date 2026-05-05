#include "scoped.hpp"

namespace imgui
{

    scoped_window::scoped_window(const char* id, ImGuiWindowFlags flags)
    {
        ImGui::Begin(id, nullptr, flags);
    }

    scoped_window::~scoped_window()
    {
        if(!ended)
        {
            ImGui::End();
        }
    }

    void scoped_window::end()
    {
        if(!ended)
        {
            ImGui::End();
            ended = true;
        }
    }

    scoped_table::scoped_table(const char* id, int columns, ImGuiTableFlags flags)
        : opened(ImGui::BeginTable(id, columns, flags))
    {
    }

    scoped_table::~scoped_table()
    {
        if(opened)
        {
            ImGui::EndTable();
        }
    }

    bool right_aligned_close_button(const char* id)
    {
        ImGui::SameLine();
        auto btn_w = ImGui::GetFrameHeight();
        auto avail = ImGui::GetContentRegionAvail().x;
        if(avail > btn_w)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - btn_w);
        }
        return ImGui::SmallButton(id);
    }

} // namespace imgui
