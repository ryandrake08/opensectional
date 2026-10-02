#include "airspace_index.hpp"
#include "altitude_filter.hpp"
#include "elevation_source.hpp"
#include "ephemeral_database.hpp"
#include "geo_math.hpp"
#include "geo_polygon.hpp"
#include "nasr_database.hpp"
#include <algorithm>
#include <cmath>
#include <optional>

namespace osect
{
    namespace
    {
        // Longest piece of a leg's great circle tested as a straight
        // lon/lat chord.
        constexpr double AIRSPACE_PIECE_NM = 10.0;

        constexpr int LAT_CELLS = 180;
        constexpr int LON_CELLS = 360;

        int lat_cell(double lat)
        {
            return std::clamp(static_cast<int>(std::floor(lat)) + 90, 0, LAT_CELLS - 1);
        }

        int lon_cell(double lon)
        {
            return std::clamp(static_cast<int>(std::floor(lon)) + 180, 0, LON_CELLS - 1);
        }

        geo_bbox bbox_of(const std::vector<airspace_ring>& rings)
        {
            geo_bbox box{180.0, 90.0, -180.0, -90.0};
            for(const auto& ring : rings)
            {
                for(const auto& p : ring.points)
                {
                    box = {std::min(box.lon_min, p.lon), std::min(box.lat_min, p.lat), std::max(box.lon_max, p.lon),
                           std::max(box.lat_max, p.lat)};
                }
            }
            return box;
        }

        bool overlaps(const geo_bbox& a, const geo_bbox& b)
        {
            return a.lon_min <= b.lon_max && b.lon_min <= a.lon_max && a.lat_min <= b.lat_max && b.lat_min <= a.lat_max;
        }

        bool surface_referenced(const std::string& ref)
        {
            return ref == "SFC" || ref == "AGL";
        }

        // An upper limit in feet MSL. Unlimited ("OTHER") stays unlimited;
        // a surface-referenced one is raised by the highest terrain in
        // `area`, or is unlimited when the terrain there is unknown.
        double upper_msl_ft(int value, const std::string& ref, const geo_bbox& area, const elevation_source& terrain)
        {
            if(ref == "OTHER")
            {
                return altitude_filter::UNLIMITED_FT;
            }
            if(surface_referenced(ref))
            {
                if(!terrain.covers(area.lat_min, area.lon_min, area.lat_max, area.lon_max))
                {
                    return altitude_filter::UNLIMITED_FT;
                }
                const auto ground_ft =
                    terrain.maximum_elevation_ft(area.lat_min, area.lon_min, area.lat_max, area.lon_max);
                return ground_ft ? value + *ground_ft : altitude_filter::UNLIMITED_FT;
            }
            return value;
        }

        // SUA_BASE.SUA_TYPE to its class. Unrecognized types are MOAs, as
        // the chart style draws them.
        airspace_class sua_class(const std::string& type)
        {
            if(type == "PA")
            {
                return airspace_class::prohibited;
            }
            if(type == "RA")
            {
                return airspace_class::restricted;
            }
            if(type == "WA")
            {
                return airspace_class::warning;
            }
            if(type == "AA")
            {
                return airspace_class::alert;
            }
            if(type == "NSA")
            {
                return airspace_class::nsa;
            }
            return airspace_class::moa;
        }
    }

    std::vector<airspace_volume> sua_volumes(const std::vector<sua>& suas, const elevation_source& terrain)
    {
        std::vector<airspace_volume> volumes;
        for(const auto& s : suas)
        {
            for(const auto& stratum : s.strata)
            {
                airspace_volume v{sua_class(s.sua_type), s.name, 0.0, 0.0, {}};
                for(const auto& part : stratum.parts)
                {
                    v.rings.push_back({part.points, part.is_hole});
                }
                v.lower_ft = stratum.lower_ft_val;
                v.upper_ft = upper_msl_ft(stratum.upper_ft_val, stratum.upper_ft_ref, bbox_of(v.rings), terrain);
                volumes.push_back(std::move(v));
            }
        }
        return volumes;
    }

    std::vector<airspace_volume> tfr_volumes(const std::vector<tfr>& tfrs, const elevation_source& terrain)
    {
        std::vector<airspace_volume> volumes;
        for(const auto& t : tfrs)
        {
            for(const auto& area : t.areas)
            {
                airspace_volume v{airspace_class::tfr, t.notam_id, 0.0, 0.0, {{area.points, false}}};
                v.lower_ft = area.lower_ft_val;
                v.upper_ft = upper_msl_ft(area.upper_ft_val, area.upper_ft_ref, bbox_of(v.rings), terrain);
                volumes.push_back(std::move(v));
            }
        }
        return volumes;
    }

    airspace_index::airspace_index(std::vector<airspace_volume> volumes)
        : cells(static_cast<std::size_t>(LAT_CELLS) * LON_CELLS), all(std::move(volumes))
    {
        bboxes.reserve(all.size());
        for(std::size_t i = 0; i < all.size(); ++i)
        {
            const auto box = bbox_of(all[i].rings);
            bboxes.push_back(box);
            for(int lat = lat_cell(box.lat_min); lat <= lat_cell(box.lat_max); ++lat)
            {
                for(int lon = lon_cell(box.lon_min); lon <= lon_cell(box.lon_max); ++lon)
                {
                    cells[static_cast<std::size_t>(lat) * LON_CELLS + lon].push_back(static_cast<std::uint32_t>(i));
                }
            }
        }
    }

    const std::vector<airspace_volume>& airspace_index::volumes() const
    {
        return all;
    }

    void airspace_index::candidates(const geo_bbox& box, std::vector<std::size_t>& out) const
    {
        const auto first = out.size();
        for(int lat = lat_cell(box.lat_min); lat <= lat_cell(box.lat_max); ++lat)
        {
            for(int lon = lon_cell(box.lon_min); lon <= lon_cell(box.lon_max); ++lon)
            {
                const auto& cell = cells[static_cast<std::size_t>(lat) * LON_CELLS + lon];
                out.insert(out.end(), cell.begin(), cell.end());
            }
        }
        std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end());
        out.erase(std::unique(out.begin() + static_cast<std::ptrdiff_t>(first), out.end()), out.end());
    }

    std::vector<std::size_t> airspace_index::containing(const geo_point& point) const
    {
        std::vector<std::size_t> found;
        candidates({point.lon, point.lat, point.lon, point.lat}, found);
        found.erase(std::remove_if(found.begin(), found.end(), [&](std::size_t i)
                                   { return !point_in_multi_ring(point.lon, point.lat, all[i].rings); }),
                    found.end());
        return found;
    }

    airspace_index::crossing airspace_index::costliest_crossing(const geo_point& from, const geo_point& to,
                                                                double cruise_ft, const airspace_costs& costs,
                                                                const std::vector<std::size_t>& exempt) const
    {
        // Stations along the great circle, with longitudes continuous
        // from `from`'s.
        const double length_nm = haversine_distance_nm(from.lat, from.lon, to.lat, to.lon);
        const int pieces = std::max(1, static_cast<int>(std::ceil(length_nm / AIRSPACE_PIECE_NM)));
        const geo_point end{to.lat, unwrap_longitude(to.lon, from.lon)};
        std::vector<geo_point> stations{from};
        for(int i = 1; i < pieces; ++i)
        {
            auto point = geodesic_point(from.lat, from.lon, end.lat, end.lon, static_cast<double>(i) / pieces);
            point.lon = unwrap_longitude(point.lon, from.lon);
            stations.push_back(point);
        }
        stations.push_back(end);

        geo_bbox leg_box{from.lon, from.lat, from.lon, from.lat};
        for(const auto& s : stations)
        {
            leg_box = {std::min(leg_box.lon_min, s.lon), std::min(leg_box.lat_min, s.lat),
                       std::max(leg_box.lon_max, s.lon), std::max(leg_box.lat_max, s.lat)};
        }

        // A leg reaching past the antimeridian is also tested shifted by
        // a turn, against the volumes on the far side.
        std::vector<double> shifts{0.0};
        if(leg_box.lon_max > 180.0)
        {
            shifts.push_back(-360.0);
        }
        if(leg_box.lon_min < -180.0)
        {
            shifts.push_back(360.0);
        }

        crossing best;
        std::vector<std::size_t> found;
        for(const double shift : shifts)
        {
            found.clear();
            candidates({leg_box.lon_min + shift, leg_box.lat_min, leg_box.lon_max + shift, leg_box.lat_max}, found);
            for(const auto i : found)
            {
                const auto& v = all[i];
                const double cost = costs.at(static_cast<std::size_t>(v.kind));
                if(cost <= best.cost || cruise_ft < v.lower_ft || cruise_ft > v.upper_ft ||
                   std::find(exempt.begin(), exempt.end(), i) != exempt.end())
                {
                    continue;
                }
                for(std::size_t k = 0; k + 1 < stations.size(); ++k)
                {
                    const geo_point a{stations[k].lat, stations[k].lon + shift};
                    const geo_point b{stations[k + 1].lat, stations[k + 1].lon + shift};
                    const geo_bbox chord{std::min(a.lon, b.lon), std::min(a.lat, b.lat), std::max(a.lon, b.lon),
                                         std::max(a.lat, b.lat)};
                    if(overlaps(chord, bboxes[i]) && segment_meets_multi_ring(a, b, v.rings))
                    {
                        best = {cost, &v};
                        break;
                    }
                }
            }
        }
        return best;
    }

} // namespace osect
