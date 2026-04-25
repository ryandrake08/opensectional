#include "flight_route.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace nasrbrowse
{
    // ---------------------------------------------------------------
    // Lat/lon parsing and formatting (file-local)
    // ---------------------------------------------------------------

    static bool is_lat_hemi(char c) { return c == 'N' || c == 'S'; }
    static bool is_lon_hemi(char c) { return c == 'E' || c == 'W'; }

    std::optional<latlon_ref> parse_latlon(const std::string& token)
    {
        // Format: DDMMSSXDDDMMSSY (15 chars)
        // DD = lat degrees (2 digits)
        // MM = lat minutes (2 digits)
        // SS = lat seconds (2 digits)
        // X  = N or S
        // DDD = lon degrees (3 digits)
        // MM  = lon minutes (2 digits)
        // SS  = lon seconds (2 digits)
        // Y   = E or W
        // DDMMSSXDDDMMSSY: 2+2+2+1+3+2+2+1 = 15 chars
        if(token.size() != 15)
            return std::nullopt;

        for(int i = 0; i < 6; ++i)
            if(!std::isdigit(static_cast<unsigned char>(token[i])))
                return std::nullopt;
        if(!is_lat_hemi(token[6]))
            return std::nullopt;
        for(int i = 7; i < 14; ++i)
            if(!std::isdigit(static_cast<unsigned char>(token[i])))
                return std::nullopt;
        if(!is_lon_hemi(token[14]))
            return std::nullopt;

        auto lat_d = std::stoi(token.substr(0, 2));
        auto lat_m = std::stoi(token.substr(2, 2));
        auto lat_s = std::stoi(token.substr(4, 2));
        auto lat_h = token[6];

        auto lon_d = std::stoi(token.substr(7, 3));
        auto lon_m = std::stoi(token.substr(10, 2));
        auto lon_s = std::stoi(token.substr(12, 2));
        auto lon_h = token[14];

        if(lat_m >= 60 || lat_s >= 60 || lon_m >= 60 || lon_s >= 60)
            return std::nullopt;
        if(lat_d > 90 || lon_d > 180)
            return std::nullopt;

        auto lat = lat_d + lat_m / 60.0 + lat_s / 3600.0;
        auto lon = lon_d + lon_m / 60.0 + lon_s / 3600.0;
        if(lat_h == 'S') lat = -lat;
        if(lon_h == 'W') lon = -lon;

        return latlon_ref{lat, lon};
    }

    static std::string format_latlon(double lat, double lon)
    {
        auto lat_h = lat >= 0 ? 'N' : 'S';
        auto lon_h = lon >= 0 ? 'E' : 'W';
        lat = std::abs(lat);
        lon = std::abs(lon);

        auto lat_d = static_cast<int>(lat);
        auto lat_m = static_cast<int>((lat - lat_d) * 60.0);
        auto lat_s = static_cast<int>(std::round((lat - lat_d - lat_m / 60.0) * 3600.0));
        if(lat_s == 60) { lat_m++; lat_s = 0; }
        if(lat_m == 60) { lat_d++; lat_m = 0; }

        auto lon_d = static_cast<int>(lon);
        auto lon_m = static_cast<int>((lon - lon_d) * 60.0);
        auto lon_s = static_cast<int>(std::round((lon - lon_d - lon_m / 60.0) * 3600.0));
        if(lon_s == 60) { lon_m++; lon_s = 0; }
        if(lon_m == 60) { lon_d++; lon_m = 0; }

        std::array<char, 16> buf{};
        std::snprintf(buf.data(), buf.size(), "%02d%02d%02d%c%03d%02d%02d%c",
                      lat_d, lat_m, lat_s, lat_h,
                      lon_d, lon_m, lon_s, lon_h);
        return buf.data();
    }

    // ---------------------------------------------------------------
    // Waypoint accessors
    // ---------------------------------------------------------------

    double waypoint_lat(const route_waypoint& wp)
    {
        return std::visit([](const auto& v) -> double
        {
            if constexpr(std::is_same_v<std::decay_t<decltype(v)>, airport_ref>)
                return v.resolved.lat;
            else if constexpr(std::is_same_v<std::decay_t<decltype(v)>, navaid_ref>)
                return v.resolved.lat;
            else if constexpr(std::is_same_v<std::decay_t<decltype(v)>, fix_ref>)
                return v.resolved.lat;
            else
                return v.lat;
        }, wp);
    }

    double waypoint_lon(const route_waypoint& wp)
    {
        return std::visit([](const auto& v) -> double
        {
            if constexpr(std::is_same_v<std::decay_t<decltype(v)>, airport_ref>)
                return v.resolved.lon;
            else if constexpr(std::is_same_v<std::decay_t<decltype(v)>, navaid_ref>)
                return v.resolved.lon;
            else if constexpr(std::is_same_v<std::decay_t<decltype(v)>, fix_ref>)
                return v.resolved.lon;
            else
                return v.lon;
        }, wp);
    }

    std::string waypoint_id(const route_waypoint& wp)
    {
        return std::visit([](const auto& v) -> std::string
        {
            if constexpr(std::is_same_v<std::decay_t<decltype(v)>, airport_ref>)
                return v.id;
            else if constexpr(std::is_same_v<std::decay_t<decltype(v)>, navaid_ref>)
                return v.id;
            else if constexpr(std::is_same_v<std::decay_t<decltype(v)>, fix_ref>)
                return v.id;
            else
                return format_latlon(v.lat, v.lon);
        }, wp);
    }

    // ---------------------------------------------------------------
    // Airway expansion
    // ---------------------------------------------------------------

    // Haversine distance in NM between two lat/lon points
    static double haversine_nm(double lat1, double lon1, double lat2, double lon2)
    {
        constexpr auto DEG2RAD = 3.14159265358979323846 / 180.0;
        constexpr auto EARTH_RADIUS_NM = 3440.065;
        auto dlat = (lat2 - lat1) * DEG2RAD;
        auto dlon = (lon2 - lon1) * DEG2RAD;
        auto a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                   std::cos(lat1 * DEG2RAD) * std::cos(lat2 * DEG2RAD) *
                   std::sin(dlon / 2) * std::sin(dlon / 2);
        return 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a)) * EARTH_RADIUS_NM;
    }

    // Signed cross-track distance in NM from point (lat_p, lon_p) to the
    // great circle through (lat1, lon1) and (lat2, lon2). Absolute value
    // is the perpendicular distance from the point to the great circle.
    static double cross_track_nm(double lat1, double lon1,
                                  double lat2, double lon2,
                                  double lat_p, double lon_p)
    {
        constexpr auto DEG2RAD = 3.14159265358979323846 / 180.0;
        constexpr auto EARTH_RADIUS_NM = 3440.065;
        auto d13 = haversine_nm(lat1, lon1, lat_p, lon_p);
        if(d13 == 0.0) return 0.0;
        auto rla1 = lat1 * DEG2RAD;
        auto rla2 = lat2 * DEG2RAD;
        auto rla3 = lat_p * DEG2RAD;
        auto dlon12 = (lon2 - lon1) * DEG2RAD;
        auto dlon13 = (lon_p - lon1) * DEG2RAD;
        auto b12 = std::atan2(std::sin(dlon12) * std::cos(rla2),
                              std::cos(rla1) * std::sin(rla2) -
                              std::sin(rla1) * std::cos(rla2) * std::cos(dlon12));
        auto b13 = std::atan2(std::sin(dlon13) * std::cos(rla3),
                              std::cos(rla1) * std::sin(rla3) -
                              std::sin(rla1) * std::cos(rla3) * std::cos(dlon13));
        return std::asin(std::sin(d13 / EARTH_RADIUS_NM) *
                         std::sin(b13 - b12)) * EARTH_RADIUS_NM;
    }

    // One point on an airway — the fix name plus its coordinates as
    // stored on the airway segment. `lat`/`lon` are authoritative for
    // the airway's geometry; the same name may resolve to a nearby
    // navaid or fix via resolve_awy_point. `gap_after` marks a
    // discontinuity in the airway: the segment from this point to the
    // next one in the list is a published AWY_SEG row with gap_flag='Y'
    // — the airway does not actually connect them, and a route that
    // traverses this pair must bridge it with an explicit direct leg.
    struct awy_point
    {
        std::string name;
        double lat;
        double lon;
        bool gap_after = false;
    };

    // Resolve an airway point's name to the closest navaid/fix, falling
    // back to a bare lat/lon if neither matches. Duplicate navaid/fix
    // names are disambiguated by proximity to the airway's coordinates.
    static route_waypoint resolve_awy_point(const awy_point& p,
                                             const nasr_database& db)
    {
        auto navs = db.lookup_navaids(p.name);
        if(!navs.empty())
        {
            auto& best = *std::min_element(navs.begin(), navs.end(),
                [&](const navaid& a, const navaid& b)
                { return haversine_nm(p.lat, p.lon, a.lat, a.lon) <
                         haversine_nm(p.lat, p.lon, b.lat, b.lon); });
            return navaid_ref{p.name, std::move(best)};
        }
        auto fixes = db.lookup_fixes(p.name);
        if(!fixes.empty())
        {
            auto& best = *std::min_element(fixes.begin(), fixes.end(),
                [&](const fix& a, const fix& b)
                { return haversine_nm(p.lat, p.lon, a.lat, a.lon) <
                         haversine_nm(p.lat, p.lon, b.lat, b.lon); });
            return fix_ref{p.name, std::move(best)};
        }
        return latlon_ref{p.lat, p.lon};
    }

    // Build an ordered list of unique (name, lat, lon) points along an
    // airway, including the gap_after marker for published
    // discontinuities. Segments are sorted by point_seq.
    static std::vector<awy_point>
    airway_points(const std::string& airway_id, const nasr_database& db)
    {
        auto segments = db.query_airway_by_id(airway_id);
        std::sort(segments.begin(), segments.end(),
                  [](const airway_segment& a, const airway_segment& b)
                  { return a.point_seq < b.point_seq; });
        std::vector<awy_point> points;
        for(const auto& seg : segments)
        {
            if(points.empty() || points.back().name != seg.from_point)
                points.push_back({seg.from_point, seg.from_lat, seg.from_lon, false});
            if(seg.gap_flag == "Y")
                points.back().gap_after = true;
            points.push_back({seg.to_point, seg.to_lat, seg.to_lon, false});
        }
        return points;
    }

    // Walk an airway from entry_id to exit_id, splitting at any
    // published discontinuities. Returns one or more elements:
    //
    //   - A continuous traversal produces a single airway_ref.
    //   - A traversal crossing N gaps produces N+1 airway_refs
    //     interleaved with N explicit bridge waypoints.
    //   - An "empty" section (entry adjacent to exit via just a gap)
    //     is emitted as two bare waypoints rather than a degenerate
    //     airway_ref over a single fix.
    //
    // Returns an empty vector on failure (unknown entry/exit, or
    // entry == exit).
    static std::vector<route_element>
    expand_airway(const std::string& airway_id,
                  const std::string& entry_id,
                  const std::string& exit_id,
                  const nasr_database& db)
    {
        auto points = airway_points(airway_id, db);
        if(points.empty()) return {};

        auto entry_idx = -1;
        auto exit_idx = -1;
        for(int i = 0; i < static_cast<int>(points.size()); ++i)
        {
            if(points[i].name == entry_id && entry_idx < 0)
                entry_idx = i;
            if(points[i].name == exit_id)
                exit_idx = i;
        }

        if(entry_idx < 0 || exit_idx < 0 || entry_idx == exit_idx)
            return {};

        auto step = (exit_idx > entry_idx) ? 1 : -1;

        // Is the segment between points[pos] and points[pos+step] a gap?
        auto crosses_gap = [&](int pos)
        {
            auto g = (step == 1) ? pos : pos + step;
            return g >= 0 && g < static_cast<int>(points.size())
                   && points[g].gap_after;
        };

        // Finalize a contiguous section of length `section.size()` as
        // either an airway_ref (>= 2 points) or a bare waypoint (1).
        // A 0-length section is skipped.
        auto flush = [&](std::vector<route_waypoint>& section,
                          std::vector<route_element>& out)
        {
            if(section.size() >= 2)
            {
                out.push_back(airway_ref{
                    airway_id,
                    waypoint_id(section.front()),
                    waypoint_id(section.back()),
                    section});
            }
            else if(section.size() == 1)
            {
                out.push_back(section.front());
            }
            section.clear();
        };

        std::vector<route_element> result;
        std::vector<route_waypoint> section;
        auto i = entry_idx;
        while(true)
        {
            section.push_back(resolve_awy_point(points[i], db));
            if(i == exit_idx) break;
            if(crosses_gap(i))
            {
                flush(section, result);
                // Step across the gap and emit the far side as an
                // explicit bridge waypoint, which also seeds the next
                // section.
                i += step;
                auto bridge = resolve_awy_point(points[i], db);
                result.push_back(bridge);
                section.push_back(std::move(bridge));
                if(i == exit_idx) break;
            }
            i += step;
        }
        // The last point (exit) is already in `section`, and the
        // bridge-seeding branch above already pushed its duplicate to
        // `result`. Avoid re-emitting when the section collapsed to
        // just that bridge.
        if(section.size() >= 2)
            flush(section, result);
        return result;
    }

    // Find the closest fix on the airway to a given lat/lon
    static std::string find_closest_airway_fix(
        const std::string& airway_id,
        double lat, double lon,
        const nasr_database& db)
    {
        auto segments = db.query_airway_by_id(airway_id);
        std::string best;
        auto best_dist = 1e18;
        for(const auto& seg : segments)
        {
            auto d = haversine_nm(lat, lon, seg.from_lat, seg.from_lon);
            if(d < best_dist) { best_dist = d; best = seg.from_point; }
            d = haversine_nm(lat, lon, seg.to_lat, seg.to_lon);
            if(d < best_dist) { best_dist = d; best = seg.to_point; }
        }
        return best;
    }

    // Check if a fix name exists on an airway
    static bool is_on_airway(const std::string& fix_name,
                             const std::string& airway_id,
                             const nasr_database& db)
    {
        auto segments = db.query_airway_by_id(airway_id);
        return std::any_of(segments.begin(), segments.end(),
            [&](const auto& seg)
            { return seg.from_point == fix_name || seg.to_point == fix_name; });
    }

    // ---------------------------------------------------------------
    // Resolve a single token to a waypoint (airport > navaid > fix)
    // ---------------------------------------------------------------

    // Pick the index of the candidate nearest to (hint_lat, hint_lon).
    // When `hint` is empty or the candidate list has one entry, returns 0
    // (preserving the historical "first match wins" behavior).
    template <typename T>
    static std::size_t nearest_index(
        const std::vector<T>& candidates,
        const std::optional<std::pair<double, double>>& hint)
    {
        if(!hint || candidates.size() <= 1) return 0;
        std::size_t best = 0;
        auto best_d = haversine_nm(hint->first, hint->second,
                                    candidates[0].lat, candidates[0].lon);
        for(std::size_t i = 1; i < candidates.size(); ++i)
        {
            auto d = haversine_nm(hint->first, hint->second,
                                   candidates[i].lat, candidates[i].lon);
            if(d < best_d) { best_d = d; best = i; }
        }
        return best;
    }

    // `hint`, when set, is the lat/lon of the nearest already-resolved
    // waypoint — used to disambiguate ID collisions where the database
    // contains multiple navaids or fixes sharing a name (e.g. several
    // "IL" fixes in different states). The candidate geographically
    // closest to the hint wins.
    static std::optional<route_waypoint>
    resolve_waypoint(const std::string& token, const nasr_database& db,
                      const std::optional<std::pair<double, double>>& hint
                          = std::nullopt)
    {
        // Try lat/lon first
        auto ll = parse_latlon(token);
        if(ll) return route_waypoint{*ll};

        // Try airport — IDs are unique in practice, no disambiguation needed.
        auto apts = db.lookup_airports(token);
        if(!apts.empty())
            return route_waypoint{airport_ref{token, std::move(apts.front())}};

        // Try navaid
        auto navs = db.lookup_navaids(token);
        if(!navs.empty())
        {
            auto i = nearest_index(navs, hint);
            return route_waypoint{navaid_ref{token, std::move(navs[i])}};
        }

        // Try fix
        auto fixes = db.lookup_fixes(token);
        if(!fixes.empty())
        {
            auto i = nearest_index(fixes, hint);
            return route_waypoint{fix_ref{token, std::move(fixes[i])}};
        }

        return std::nullopt;
    }

    // Lat/lon of the last already-resolved waypoint in `elements`,
    // for passing to resolve_waypoint as a disambiguation hint. When
    // the last element is an airway_ref, returns the coords of its
    // last expanded waypoint; when `elements` is empty, returns
    // nullopt.
    static std::optional<std::pair<double, double>>
    hint_from_elements(const std::vector<route_element>& elements)
    {
        if(elements.empty()) return std::nullopt;
        const auto& last = elements.back();
        if(std::holds_alternative<route_waypoint>(last))
        {
            const auto& wp = std::get<route_waypoint>(last);
            return std::pair{waypoint_lat(wp), waypoint_lon(wp)};
        }
        const auto& awy = std::get<airway_ref>(last);
        if(awy.expanded.empty()) return std::nullopt;
        const auto& wp = awy.expanded.back();
        return std::pair{waypoint_lat(wp), waypoint_lon(wp)};
    }

    // Check if a token is an airway identifier
    static bool is_airway(const std::string& token, const nasr_database& db)
    {
        auto segs = db.query_airway_by_id(token);
        return !segs.empty();
    }

    // ---------------------------------------------------------------
    // Expand elements into flat waypoint list
    // ---------------------------------------------------------------

    static void expand_elements(const std::vector<route_element>& elements,
                                std::vector<route_waypoint>& out)
    {
        out.clear();
        for(const auto& elem : elements)
        {
            if(std::holds_alternative<route_waypoint>(elem))
            {
                const auto& wp = std::get<route_waypoint>(elem);
                // Skip if duplicate of last waypoint
                if(!out.empty() && waypoint_id(wp) == waypoint_id(out.back()))
                    continue;
                out.push_back(wp);
            }
            else
            {
                const auto& awy = std::get<airway_ref>(elem);
                for(size_t i = 0; i < awy.expanded.size(); ++i)
                {
                    // Skip first if it duplicates the last output waypoint
                    if(i == 0 && !out.empty() &&
                       waypoint_id(awy.expanded[i]) == waypoint_id(out.back()))
                        continue;
                    out.push_back(awy.expanded[i]);
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // Constructor — parse and resolve, or throw
    // ---------------------------------------------------------------

    flight_route::flight_route(const std::string& text, const nasr_database& db)
    {
        // Tokenize
        std::vector<std::string> tokens;
        std::istringstream iss(text);
        std::string tok;
        while(iss >> tok)
        {
            for(auto& c : tok)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            tokens.push_back(std::move(tok));
        }

        if(tokens.empty())
            throw route_parse_error("empty route");

        for(size_t i = 0; i < tokens.size(); ++i)
        {
            auto ti = static_cast<int>(i);

            // Check if this token is an airway
            if(i > 0 && i + 1 < tokens.size() && is_airway(tokens[i], db))
            {
                const auto& airway_id = tokens[i];

                // Previous element provides the entry point
                std::string entry_id;
                if(!elements.empty())
                {
                    auto& prev = elements.back();
                    if(std::holds_alternative<route_waypoint>(prev))
                        entry_id = waypoint_id(std::get<route_waypoint>(prev));
                    else
                    {
                        auto& aref = std::get<airway_ref>(prev);
                        if(!aref.expanded.empty())
                            entry_id = waypoint_id(aref.expanded.back());
                    }
                }

                if(entry_id.empty())
                    throw route_parse_error(
                        "airway " + airway_id + " has no entry point",
                        airway_id, ti);

                // Next token is the exit point
                ++i;
                const auto& exit_id = tokens[i];

                // Auto-correct entry if not on airway
                auto actual_entry = entry_id;
                if(!is_on_airway(entry_id, airway_id, db))
                {
                    auto lat = 0.0;
                    auto lon = 0.0;
                    if(!elements.empty())
                    {
                        auto& prev = elements.back();
                        if(std::holds_alternative<route_waypoint>(prev))
                        {
                            const auto& wp = std::get<route_waypoint>(prev);
                            lat = waypoint_lat(wp);
                            lon = waypoint_lon(wp);
                        }
                        else
                        {
                            auto& aref = std::get<airway_ref>(prev);
                            if(!aref.expanded.empty())
                            {
                                lat = waypoint_lat(aref.expanded.back());
                                lon = waypoint_lon(aref.expanded.back());
                            }
                        }
                    }
                    actual_entry = find_closest_airway_fix(airway_id, lat, lon, db);
                    if(actual_entry.empty())
                        throw route_parse_error(
                            "cannot find entry point on " + airway_id,
                            airway_id, ti);
                }

                // Auto-correct exit if not on airway
                auto actual_exit = exit_id;
                if(!is_on_airway(exit_id, airway_id, db))
                {
                    auto exit_wp = resolve_waypoint(exit_id, db,
                                                     hint_from_elements(elements));
                    if(!exit_wp)
                        throw route_parse_error(
                            "unknown waypoint: " + exit_id,
                            exit_id, static_cast<int>(i));
                    actual_exit = find_closest_airway_fix(
                        airway_id, waypoint_lat(*exit_wp), waypoint_lon(*exit_wp), db);
                    if(actual_exit.empty())
                        throw route_parse_error(
                            "cannot find exit point on " + airway_id,
                            airway_id, ti);
                }

                auto expanded = expand_airway(airway_id, actual_entry, actual_exit, db);
                if(expanded.empty())
                    throw route_parse_error(
                        std::string("cannot expand ").append(airway_id)
                            .append(" from ").append(actual_entry)
                            .append(" to ").append(actual_exit),
                        airway_id, ti);

                // expand_airway may have split at a discontinuity,
                // producing multiple airway_refs interleaved with
                // bridge waypoints.
                for(auto& elem : expanded)
                    elements.push_back(std::move(elem));

                // If exit was auto-corrected, add the original exit as a
                // separate waypoint
                if(actual_exit != exit_id)
                {
                    auto exit_wp = resolve_waypoint(exit_id, db,
                                                     hint_from_elements(elements));
                    if(!exit_wp)
                        throw route_parse_error(
                            "unknown waypoint: " + exit_id,
                            exit_id, static_cast<int>(i));
                    elements.push_back(*exit_wp);
                }
            }
            else
            {
                auto wp = resolve_waypoint(tokens[i], db,
                                            hint_from_elements(elements));
                if(!wp)
                    throw route_parse_error(
                        "unknown waypoint: " + tokens[i],
                        tokens[i], ti);
                elements.push_back(*wp);
            }
        }

        if(elements.size() < 2)
            throw route_parse_error("route must have at least two waypoints");

        expand_elements(elements, waypoints);
        airway_ize(db);
    }

    // ---------------------------------------------------------------
    // to_text — reconstruct shorthand
    // ---------------------------------------------------------------

    std::string flight_route::to_text() const
    {
        std::string result;
        for(const auto& elem : elements)
        {
            if(!result.empty()) result += ' ';
            if(std::holds_alternative<route_waypoint>(elem))
            {
                result += waypoint_id(std::get<route_waypoint>(elem));
            }
            else
            {
                const auto& awy = std::get<airway_ref>(elem);
                // Entry is already the previous element, so just emit
                // the airway and exit
                result += awy.airway_id;
                result += ' ';
                result += awy.exit_id;
            }
        }
        return result;
    }

    // ---------------------------------------------------------------
    // insert_waypoint
    // ---------------------------------------------------------------

    // ---------------------------------------------------------------
    // Leg computation
    // ---------------------------------------------------------------

    // Initial great-circle bearing from (lat1, lon1) to (lat2, lon2), in
    // degrees, normalized to [0, 360).
    static double true_course_deg(double lat1, double lon1,
                                  double lat2, double lon2)
    {
        constexpr auto DEG2RAD = 3.14159265358979323846 / 180.0;
        constexpr auto RAD2DEG = 180.0 / 3.14159265358979323846;
        auto rlat1 = lat1 * DEG2RAD;
        auto rlat2 = lat2 * DEG2RAD;
        auto dlon = (lon2 - lon1) * DEG2RAD;
        auto y = std::sin(dlon) * std::cos(rlat2);
        auto x = std::cos(rlat1) * std::sin(rlat2) -
                   std::sin(rlat1) * std::cos(rlat2) * std::cos(dlon);
        auto brg = std::atan2(y, x) * RAD2DEG;
        if(brg < 0) brg += 360.0;
        return brg;
    }

    std::vector<route_leg> flight_route::compute_legs() const
    {
        std::vector<route_leg> legs;
        legs.reserve(!waypoints.empty() ? waypoints.size() - 1 : 0);
        for(size_t i = 1; i < waypoints.size(); ++i)
        {
            const auto& a = waypoints[i - 1];
            const auto& b = waypoints[i];
            auto la = waypoint_lat(a);
            auto lo_a = waypoint_lon(a);
            auto lb = waypoint_lat(b);
            auto lo_b = waypoint_lon(b);
            legs.push_back({
                waypoint_id(a),
                waypoint_id(b),
                haversine_nm(la, lo_a, lb, lo_b),
                true_course_deg(la, lo_a, lb, lo_b)});
        }
        return legs;
    }

    double flight_route::total_distance_nm() const
    {
        auto t = 0.0;
        for(const auto& l : compute_legs()) t += l.distance_nm;
        return t;
    }

    // ---------------------------------------------------------------
    // insert_waypoint
    // ---------------------------------------------------------------

    void flight_route::insert_waypoint_raw(int segment_index,
                                            const route_waypoint& wp)
    {
        auto& r = *this;
        // Find which element owns the segment endpoints
        // Walk through elements, tracking the expanded waypoint index
        auto wp_idx = 0;
        for(size_t ei = 0; ei < r.elements.size(); ++ei)
        {
            if(std::holds_alternative<route_waypoint>(r.elements[ei]))
            {
                if(wp_idx == segment_index)
                {
                    // Insert after this element
                    r.elements.insert(r.elements.begin() + ei + 1, wp);
                    expand_elements(r.elements, r.waypoints);
                    return;
                }
                wp_idx++;
            }
            else
            {
                auto& awy = std::get<airway_ref>(r.elements[ei]);
                auto awy_start = wp_idx;
                // Count how many waypoints this airway contributes
                // (accounting for dedup with previous)
                auto awy_count = 0;
                for(size_t j = 0; j < awy.expanded.size(); ++j)
                {
                    if(j == 0 && wp_idx > 0)
                    {
                        // First waypoint may have been deduped
                        if(wp_idx > 0 && waypoint_id(awy.expanded[0]) ==
                           waypoint_id(r.waypoints[wp_idx - 1]))
                            continue;
                    }
                    awy_count++;
                }

                if(segment_index >= awy_start &&
                   segment_index < awy_start + awy_count)
                {
                    // Segment falls within this airway — split it.
                    // Replace the airway element with explicit waypoints
                    // and the new insertion.
                    std::vector<route_element> replacement;
                    for(size_t j = 0; j < awy.expanded.size(); ++j)
                    {
                        replacement.push_back(awy.expanded[j]);
                        auto global_idx = awy_start +
                            static_cast<int>(j) -
                            (wp_idx > 0 && waypoint_id(awy.expanded[0]) ==
                             waypoint_id(r.waypoints[wp_idx - 1]) ? 1 : 0);
                        if(global_idx == segment_index)
                            replacement.push_back(wp);
                    }
                    r.elements.erase(r.elements.begin() + ei);
                    r.elements.insert(r.elements.begin() + ei,
                                      replacement.begin(), replacement.end());
                    expand_elements(r.elements, r.waypoints);
                    return;
                }
                wp_idx = awy_start + awy_count;
            }
        }

        // Fallback: insert into waypoints directly and rebuild elements
        r.waypoints.insert(r.waypoints.begin() + segment_index + 1, wp);
        r.elements.clear();
        for(const auto& w : r.waypoints)
            r.elements.push_back(w);
    }

    void flight_route::insert_waypoint(int segment_index,
                                        const route_waypoint& wp,
                                        const nasr_database& db)
    {
        assert(segment_index >= 0 &&
               segment_index < static_cast<int>(waypoints.size()) - 1);
        insert_waypoint_raw(segment_index, wp);
        airway_ize(db);
    }

    void flight_route::replace_waypoint(int waypoint_index, route_waypoint wp,
                                         const nasr_database& db)
    {
        assert(waypoint_index >= 0 &&
               waypoint_index < static_cast<int>(waypoints.size()));
        waypoints[waypoint_index] = std::move(wp);
        elements.clear();
        for(const auto& w : waypoints)
            elements.push_back(w);
        airway_ize(db);
    }

    void flight_route::delete_waypoint(int waypoint_index,
                                        const nasr_database& db)
    {
        assert(waypoint_index >= 0 &&
               waypoint_index < static_cast<int>(waypoints.size()));
        assert(waypoints.size() > 2);
        waypoints.erase(waypoints.begin() + waypoint_index);
        elements.clear();
        for(const auto& w : waypoints)
            elements.push_back(w);
        airway_ize(db);
    }

    // ---------------------------------------------------------------
    // airway_ize
    //
    // Two passes over `waypoints`:
    //
    // 1. Coerce pass — for each consecutive pair (a, b) that the user
    //    typed as a direct leg, check whether both lie on a common
    //    airway and all intermediate fixes of that airway are colinear
    //    (within COERCE_XTE_NM) with the user's direct path. If so,
    //    insert the intermediates so the collapse pass can fold them
    //    into shorthand. Colinearity is checked iteratively: the GC
    //    shortens as each intermediate is accepted, so slightly-bent
    //    long airways still pass as long as every local hop is on-line.
    //    All-or-nothing: if any intermediate fails, the pair is left
    //    untouched rather than partially filled.
    //
    // 2. Collapse pass — walk `waypoints` and detect runs of three or
    //    more consecutive points that form a sequential path along a
    //    common airway. Collapse each run into an airway_ref element.
    //    A length-2 run isn't collapsed — "A AWY B" has more tokens
    //    than "A B".
    // ---------------------------------------------------------------

    // Tight tolerance: false positives silently reroute the user, so
    // only coerce airways that are effectively a single straight line.
    // 0.5 NM is an order of magnitude tighter than enroute airway
    // width (4 NM each side of centerline).
    static constexpr auto COERCE_XTE_NM = 0.5;

    void flight_route::airway_ize(const nasr_database& db)
    {
        if(waypoints.size() < 2) return;

        // Per-call cache of airway_id -> ordered list of points.
        std::unordered_map<std::string, std::vector<awy_point>> awy_cache;

        auto get_airway_points = [&](const std::string& awy_id)
            -> const std::vector<awy_point>&
        {
            auto it = awy_cache.find(awy_id);
            if(it != awy_cache.end()) return it->second;
            return awy_cache.emplace(awy_id, airway_points(awy_id, db))
                .first->second;
        };

        // -----------------------------------------------------------
        // Coerce pass
        // -----------------------------------------------------------
        //
        // For each pair (waypoints[i], waypoints[i+1]), find common
        // airways and try to insert all intermediates if they are
        // colinear. If successful, advance past the inserted range.
        {
            auto i = size_t{0};
            while(i + 1 < waypoints.size())
            {
                const auto& a = waypoints[i];
                const auto& b = waypoints[i + 1];
                auto id_a = waypoint_id(a);
                auto id_b = waypoint_id(b);

                // Find airways containing both a and b.
                auto aa = db.airways_containing(id_a);
                auto ab = db.airways_containing(id_b);
                std::sort(aa.begin(), aa.end());
                std::sort(ab.begin(), ab.end());
                std::vector<std::string> common;
                std::set_intersection(aa.begin(), aa.end(),
                                      ab.begin(), ab.end(),
                                      std::back_inserter(common));

                // Pick the airway that yields the most accepted
                // intermediates. Ties broken by lexicographic order
                // for determinism.
                std::vector<route_waypoint> best_inserts;
                for(const auto& awy_id : common)
                {
                    const auto& points = get_airway_points(awy_id);

                    // First occurrence of each name. Airway fix names
                    // are unique in practice; if they ever aren't, we
                    // err on the side of coercing less rather than more.
                    auto idx_a = -1;
                    for(auto k = 0; k < static_cast<int>(points.size()); ++k)
                        if(points[k].name == id_a) { idx_a = k; break; }
                    auto idx_b = -1;
                    for(auto k = 0; k < static_cast<int>(points.size()); ++k)
                        if(points[k].name == id_b) { idx_b = k; break; }
                    if(idx_a < 0 || idx_b < 0 || idx_a == idx_b) continue;

                    // A published gap anywhere in the walk means the
                    // airway does not actually connect a and b. Skip.
                    auto lo = std::min(idx_a, idx_b);
                    auto hi = std::max(idx_a, idx_b);
                    auto has_gap = false;
                    for(auto k = lo; k < hi; ++k)
                        if(points[k].gap_after) { has_gap = true; break; }
                    if(has_gap) continue;

                    auto step = (idx_b > idx_a) ? 1 : -1;
                    auto anchor_lat = waypoint_lat(a);
                    auto anchor_lon = waypoint_lon(a);
                    auto end_lat = waypoint_lat(b);
                    auto end_lon = waypoint_lon(b);
                    std::vector<route_waypoint> accepted;
                    auto ok = true;
                    for(auto k = idx_a + step; k != idx_b; k += step)
                    {
                        const auto& p = points[k];
                        // Guard against a name collision where an
                        // intermediate happens to have the same name
                        // as the endpoint.
                        if(p.name == id_a || p.name == id_b) { ok = false; break; }
                        auto xt = cross_track_nm(anchor_lat, anchor_lon,
                                                  end_lat, end_lon,
                                                  p.lat, p.lon);
                        if(std::abs(xt) > COERCE_XTE_NM) { ok = false; break; }
                        accepted.push_back(resolve_awy_point(p, db));
                        anchor_lat = p.lat;
                        anchor_lon = p.lon;
                    }
                    if(ok && accepted.size() > best_inserts.size())
                        best_inserts = std::move(accepted);
                }

                if(!best_inserts.empty())
                {
                    auto inserted = best_inserts.size();
                    waypoints.insert(waypoints.begin() + i + 1,
                                     std::make_move_iterator(best_inserts.begin()),
                                     std::make_move_iterator(best_inserts.end()));
                    i += inserted + 1;
                }
                else
                {
                    ++i;
                }
            }
        }

        // -----------------------------------------------------------
        // Collapse pass
        // -----------------------------------------------------------
        //
        // If the coerce pass inserted anything, waypoints.size() is
        // guaranteed >= 3 here (we entered with >= 2 and only grew).
        // So size < 3 means coerce did nothing and elements still
        // matches waypoints — preserve any existing airway_ref the
        // user typed for an adjacent pair.
        if(waypoints.size() < 3) return;

        std::vector<route_element> new_elements;
        auto i = size_t{0};
        while(i < waypoints.size())
        {
            std::string best_awy;
            auto best_end = i;

            if(i + 1 < waypoints.size())
            {
                const auto& id_i = waypoint_id(waypoints[i]);
                const auto& id_i1 = waypoint_id(waypoints[i + 1]);
                auto candidates = db.adjacent_airways(id_i, id_i1);

                for(const auto& awy_id : candidates)
                {
                    const auto& points = get_airway_points(awy_id);

                    for(size_t start = 0; start < points.size(); ++start)
                    {
                        if(points[start].name != id_i) continue;
                        for(auto dir : {1, -1})
                        {
                            auto next_pos = static_cast<int>(start) + dir;
                            if(next_pos < 0 ||
                               next_pos >= static_cast<int>(points.size()))
                                continue;
                            if(points[next_pos].name != id_i1) continue;

                            auto k = i + 1;
                            auto pos = next_pos;
                            while(k + 1 < waypoints.size())
                            {
                                auto nxt = pos + dir;
                                if(nxt < 0 ||
                                   nxt >= static_cast<int>(points.size()))
                                    break;
                                // Stop at a published discontinuity:
                                // even if the names line up, the airway
                                // does not connect pos to nxt.
                                auto gap_idx = std::min(pos, nxt);
                                if(points[gap_idx].gap_after) break;
                                if(points[nxt].name != waypoint_id(waypoints[k + 1]))
                                    break;
                                ++k;
                                pos = nxt;
                            }

                            if(k > best_end)
                            {
                                best_end = k;
                                best_awy = awy_id;
                            }
                        }
                    }
                }
            }

            if(best_end - i >= 2)
            {
                new_elements.push_back(waypoints[i]);
                airway_ref aref;
                aref.airway_id = best_awy;
                aref.entry_id = waypoint_id(waypoints[i]);
                aref.exit_id = waypoint_id(waypoints[best_end]);
                aref.expanded.assign(waypoints.begin() + i,
                                     waypoints.begin() + best_end + 1);
                new_elements.push_back(std::move(aref));
                i = best_end + 1;
            }
            else
            {
                new_elements.push_back(waypoints[i]);
                ++i;
            }
        }

        elements = std::move(new_elements);
    }

} // namespace nasrbrowse
