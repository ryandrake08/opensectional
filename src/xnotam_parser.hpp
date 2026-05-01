#pragma once

#include "tfr.hpp"
#include <optional>
#include <stdexcept>
#include <string>

namespace osect
{
    // Thrown by parse_xnotam on malformed XML or a structural mismatch
    // (e.g. the document is well-formed but doesn't carry the
    // XNOTAM-Update / Group / Add / Not envelope we expect). The
    // tfr_source layer catches this so a single corrupt detail XML
    // doesn't abort the whole batch.
    struct xnotam_parse_error : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    // Parse one XNOTAM detail document into a populated `tfr` record.
    // Returns nullopt for inputs that are well-formed but don't
    // describe a TFR with at least one polygon area — most commonly
    // cancellation entries that carry no geometry. tfr_id and
    // tfr_area::area_id are left at 0; the caller assigns them.
    std::optional<tfr> parse_xnotam(const std::string& xml);
}
