#pragma once

namespace osect
{
    // Wake the main-loop event wait. Background producers
    // (tile_loader, feature_builder, tfr_source, route_submitter)
    // call this from their worker thread after publishing a result so
    // dispatch_events() returns promptly without spinning on a short
    // timeout. Implemented in terms of sdl::event_manager::push_user_event;
    // safe to call from any thread.
    void wake_main_thread();
}
