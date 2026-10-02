#pragma once

#include "geo_types.hpp"
#include "route_plan_options.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace osect
{
    class elevation_source;
    struct sua;
    struct tfr;

    struct airspace_ring
    {
        std::vector<geo_point> points;
        bool is_hole = false;
    };

    // One block of airspace the planner can avoid: an SUA stratum or a
    // TFR area, with its vertical limits in feet MSL.
    struct airspace_volume
    {
        airspace_class kind;
        std::string name;
        double lower_ft;
        double upper_ft;
        std::vector<airspace_ring> rings;
    };

    // The volumes of every stratum of `suas`. A surface-referenced lower
    // limit is taken as MSL; a surface-referenced upper limit is raised by
    // the highest terrain under the stratum, or is unlimited when
    // `terrain` has no elevation there.
    std::vector<airspace_volume> sua_volumes(const std::vector<sua>& suas, const elevation_source& terrain);

    // The volumes of every area of `tfrs`, with limits normalized as in
    // sua_volumes.
    std::vector<airspace_volume> tfr_volumes(const std::vector<tfr>& tfrs, const elevation_source& terrain);

    // Spatial lookup of airspace volumes for testing route legs.
    class airspace_index
    {
    public:
        explicit airspace_index(std::vector<airspace_volume> volumes = {});

        const std::vector<airspace_volume>& volumes() const;

        // Indices of the volumes whose area holds `point`, at any altitude.
        std::vector<std::size_t> containing(const geo_point& point) const;

        // The costliest volume the great-circle leg `from` -> `to` passes
        // through at `cruise_ft`, by `costs`, skipping the volumes whose
        // indices are in `exempt`. `volume` is null when the leg passes
        // through none costing more than cost_include.
        struct crossing
        {
            double cost = cost_include;
            const airspace_volume* volume = nullptr;
        };
        crossing costliest_crossing(const geo_point& from, const geo_point& to, double cruise_ft,
                                    const airspace_costs& costs, const std::vector<std::size_t>& exempt) const;

    private:
        // Volume indices whose bbox touches a 1-degree cell, for cells
        // lat [-90, 90) x lon [-180, 180).
        std::vector<std::vector<std::uint32_t>> cells;
        std::vector<airspace_volume> all;
        std::vector<geo_bbox> bboxes;

        // Appends the indices of volumes whose cells overlap `box`.
        void candidates(const geo_bbox& box, std::vector<std::size_t>& out) const;
    };

} // namespace osect
