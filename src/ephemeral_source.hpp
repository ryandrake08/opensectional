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

    // SDL event type number for "an ephemeral source's state
    // changed" — covers refresh start, end, and data commit. The
    // handler re-snapshots SOURCE_META, refreshes the data-status
    // panel, and invalidates cached projections (feature_builder
    // GPU buffers).
    std::uint32_t ephemeral_refresh_event_type();

    // Convenience: push an ephemeral-refresh event for `source` to
    // the main loop. Wakes SDL_WaitEvent and delivers `source` as
    // the event code to whatever handler the main thread installed
    // on ephemeral_refresh_event_type().
    void push_ephemeral_refresh(ephemeral_source source);
}
