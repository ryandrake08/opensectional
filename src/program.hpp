#pragma once
#include <memory>
#include <string>
#include <vector>

namespace osect
{
    // Top-level application orchestrator. Constructed from a tokenized
    // command line, owns every subsystem (SDL, ImGui, the map widget,
    // the route planner, the ephemeral data facade, the ui overlay)
    // and runs the main loop.
    //
    // The constructor parses options, resolves bundled assets, and
    // initializes every subsystem, throwing on failure. `-h`/`--help`
    // prints usage to stdout and calls std::exit(EXIT_SUCCESS).
    class program
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        // Thrown by the constructor on -h/--help, after usage has
        // been printed to stdout. main converts this to a successful
        // exit. Does not derive from std::exception so it bypasses
        // the FATAL ERROR catch-all.
        struct help_requested
        {
        };

        explicit program(const std::vector<std::string>& cmdline);
        ~program();

        program(const program&) = delete;
        program& operator=(const program&) = delete;

        // Run the main loop. Returns when the SDL event loop sees a
        // quit event (user closed the window, signal handler pushed
        // a quit, etc).
        void run();
    };

    // Wake the main-loop event wait. Background producers
    // (tile_loader, feature_builder, tfr_source, route_submitter)
    // call this from their worker thread after publishing a result so
    // dispatch_events() returns promptly without spinning on a short
    // timeout. Implemented in terms of sdl::event_manager::push_user_event;
    // safe to call from any thread.
    void wake_main_thread();
}
