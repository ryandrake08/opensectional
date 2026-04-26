#include "context.hpp"
#include <sdl/command_buffer.hpp>
#include <sdl/device.hpp>
#include <sdl/event.hpp>
#include <sdl/texture.hpp>
#include <sdl/window.hpp>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

namespace imgui
{

    struct context::impl
    {
        // ImGui auto-resize windows need two frames to stabilize layout
        int warmup_frames = 2;
    };

    context::context(sdl::device& dev, sdl::window& win)
        : pimpl(new impl())
    {
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui_ImplSDL3_InitForSDLGPU(win.get());

        ImGui_ImplSDLGPU3_InitInfo gpu_info = {};
        gpu_info.Device = dev.get();
        gpu_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev.get(), win.get());
        ImGui_ImplSDLGPU3_Init(&gpu_info);
    }

    context::~context()
    {
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }

    void context::process_event(const void* event)
    {
        ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(event));
    }

    void context::new_frame()
    {
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void context::end_frame()
    {
        ImGui::Render();
    }

    void context::render(sdl::command_buffer& cmd, sdl::texture& swapchain)
    {
        ImDrawData* draw_data = ImGui::GetDrawData();

        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd.get());

        SDL_GPUColorTargetInfo color_info = {};
        color_info.texture = swapchain.get();
        color_info.load_op = SDL_GPU_LOADOP_LOAD;
        color_info.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd.get(), &color_info, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd.get(), pass);
        SDL_EndGPURenderPass(pass);
    }

    bool context::wants_mouse() const
    {
        return ImGui::GetIO().WantCaptureMouse;
    }

    bool context::wants_keyboard() const
    {
        return ImGui::GetIO().WantCaptureKeyboard;
    }

    bool context::warming_up()
    {
        if(pimpl->warmup_frames <= 0)
        {
            return false;
        }

        pimpl->warmup_frames--;

        // Push a dummy event so SDL_WaitEvent returns immediately
        // for the next warmup frame
        if(pimpl->warmup_frames > 0)
        {
            sdl::event_manager::push_user_event();
        }

        return true;
    }

} // namespace imgui
