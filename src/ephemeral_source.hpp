#pragma once

#include <cstdint>

namespace osect
{
    // Identifier for an ephemeral data source. Used as the `code`
    // payload of osect::ephemeral_refresh_event_type events pushed
    // by refreshers after they commit fresh data. A single SDL
    // event type carries all ephemeral-refresh signals; the
    // identifier here distinguishes which source updated. Adding a
    // new source (NOTAMs, weather, etc.) means adding a value here,
    // not a new event type.
    //
    // Numeric values are stable across runs only for the lifetime of
    // the build — refreshers and handlers always reference enumerators
    // by name, never the raw integer.
    enum class ephemeral_source : int
    {
        tfr = 1,
    };

    // SDL event type number for "an ephemeral data source has new
    // data on disk and any cached projections (e.g. feature_builder
    // GPU buffers) should be invalidated." Allocated on first call
    // and stable for the process lifetime; calling from any thread
    // is safe.
    std::uint32_t ephemeral_refresh_event_type();

    // Convenience: push an ephemeral-refresh event for `source` to
    // the main loop. Wakes SDL_WaitEvent and delivers `source` as
    // the event code to whatever handler the main thread installed
    // on ephemeral_refresh_event_type().
    void push_ephemeral_refresh(ephemeral_source source);
}
