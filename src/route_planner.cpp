#include "route_planner.hpp"
#include "flight_route.hpp" // parse_latlon, route_parse_error
#include "geo_math.hpp"
#include "geo_types.hpp"
#include "nasr_database.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace osect
{
    namespace
    {
        // Approximate bounding box of `radius_nm` around (lat, lon).
        // Over-estimates near the poles and across the antimeridian
        // (CONUS-only at v1; the caller still filters results by
        // exact haversine distance).
        geo_bbox bbox_around(double lat, double lon, double radius_nm)
        {
            constexpr auto NM_PER_DEG = 60.0;
            auto dlat = radius_nm / NM_PER_DEG;
            auto cos_lat = std::cos(lat * M_PI / 180.0);
            auto dlon = (cos_lat > 1e-6) ? radius_nm / (NM_PER_DEG * cos_lat) : 180.0;
            return {lon - dlon, lat - dlat, lon + dlon, lat + dlat};
        }

        // Uniform 1°×1° grid over the routable-waypoint catalog.
        // Replaces three SQLite R*Tree queries per A* expansion with
        // direct cell indexing into in-memory vectors. Built once at
        // route_planner construction; immutable thereafter.
        struct spatial_grid
        {
            static constexpr double cell_deg = 1.0;
            static constexpr int lat_cells = 180; // -90..+90
            static constexpr int lon_cells = 360; //-180..+180

            struct entry
            {
                std::size_t idx;
                double lat;
                double lon;
            };
            std::vector<std::vector<entry>> cells;

            static int lat_idx(double lat)
            {
                auto i = static_cast<int>(std::floor((lat + 90.0) / cell_deg));
                if(i < 0)
                {
                    return 0;
                }
                if(i >= lat_cells)
                {
                    return lat_cells - 1;
                }
                return i;
            }

            static int lon_idx(double lon)
            {
                // Wrap to [-180, 180); antimeridian queries are split
                // into two non-wrapping ranges by the caller.
                auto wrapped = std::fmod(lon + 180.0, 360.0);
                if(wrapped < 0)
                {
                    wrapped += 360.0;
                }
                auto i = static_cast<int>(std::floor(wrapped / cell_deg));
                if(i < 0)
                {
                    return 0;
                }
                if(i >= lon_cells)
                {
                    return lon_cells - 1;
                }
                return i;
            }

            static std::size_t cell_id(int la, int lo)
            {
                return static_cast<std::size_t>(la) * lon_cells + static_cast<std::size_t>(lo);
            }

            void reset()
            {
                cells.assign(static_cast<std::size_t>(lat_cells) * lon_cells, {});
            }

            void insert(std::size_t idx, double lat, double lon)
            {
                cells[cell_id(lat_idx(lat), lon_idx(lon))].push_back({idx, lat, lon});
            }

            // Append node indices whose stored point lies inside `bbox`
            // to `out`. Caller still filters by exact haversine
            // distance — the grid only prunes obviously-distant nodes.
            void query(const geo_bbox& bbox, std::vector<std::size_t>& out) const
            {
                auto la_lo = lat_idx(bbox.lat_min);
                auto la_hi = lat_idx(bbox.lat_max);

                auto emit_lon_range = [&](int lo_lo, int lo_hi)
                {
                    for(int la = la_lo; la <= la_hi; ++la)
                    {
                        for(int lo = lo_lo; lo <= lo_hi; ++lo)
                        {
                            const auto& bucket = cells[cell_id(la, lo)];
                            for(const auto& e : bucket)
                            {
                                if(e.lat >= bbox.lat_min && e.lat <= bbox.lat_max && e.lon >= bbox.lon_min &&
                                   e.lon <= bbox.lon_max)
                                {
                                    out.push_back(e.idx);
                                }
                            }
                        }
                    }
                };

                // Antimeridian: bbox_around can produce lon_min < -180
                // or lon_max > 180 at high latitudes. Split into two
                // ranges that each fit inside [-180, 180).
                if(bbox.lon_min < -180.0)
                {
                    emit_lon_range(0, lon_idx(bbox.lon_max));
                    emit_lon_range(lon_idx(bbox.lon_min + 360.0), lon_cells - 1);
                }
                else if(bbox.lon_max > 180.0)
                {
                    emit_lon_range(0, lon_idx(bbox.lon_max - 360.0));
                    emit_lon_range(lon_idx(bbox.lon_min), lon_cells - 1);
                }
                else
                {
                    emit_lon_range(lon_idx(bbox.lon_min), lon_idx(bbox.lon_max));
                }
            }
        };

        // Per-plan_segment scratch state. Indices into pimpl->nodes
        // are dense in [0, N), so a flat vector is faster than the
        // unordered_map version we used to use. Reset between runs by
        // walking only the entries we touched (`dirty`), keeping the
        // amortized cost O(visited) rather than O(N).
        struct astar_scratch
        {
            std::vector<double> g;
            std::vector<std::size_t> came_from;
            std::vector<std::uint8_t> closed;
            std::vector<std::size_t> dirty;
            std::vector<std::size_t> hits; // reused per expansion
        };

        // Classify an airport SITE_TYPE_CODE into a wp_subtype.
        wp_subtype classify_airport_subtype(const std::string& code)
        {
            using st = wp_subtype;
            if(code == "A")
            {
                return st::airport_landplane;
            }
            if(code == "B")
            {
                return st::airport_balloonport;
            }
            if(code == "C")
            {
                return st::airport_seaplane;
            }
            if(code == "G")
            {
                return st::airport_gliderport;
            }
            if(code == "H")
            {
                return st::airport_heliport;
            }
            if(code == "U")
            {
                return st::airport_ultralight;
            }
            return st::airport_landplane; // unknown — best guess
        }

        wp_subtype classify_navaid_subtype(const std::string& nav_type)
        {
            using st = wp_subtype;
            if(nav_type == "VOR")
            {
                return st::navaid_vor;
            }
            if(nav_type == "VORTAC")
            {
                return st::navaid_vortac;
            }
            if(nav_type == "VOR/DME")
            {
                return st::navaid_vor_dme;
            }
            if(nav_type == "DME")
            {
                return st::navaid_dme;
            }
            if(nav_type == "NDB")
            {
                return st::navaid_ndb;
            }
            if(nav_type == "NDB/DME")
            {
                return st::navaid_ndb_dme;
            }
            return st::navaid_vor; // unknown — least-distinctive default
        }

        wp_subtype classify_fix_subtype(const std::string& use_code)
        {
            using st = wp_subtype;
            if(use_code == "WP")
            {
                return st::fix_wp;
            }
            if(use_code == "RP")
            {
                return st::fix_rp;
            }
            if(use_code == "CN")
            {
                return st::fix_cn;
            }
            if(use_code == "MR")
            {
                return st::fix_mr;
            }
            if(use_code == "VFR")
            {
                return st::fix_vfr;
            }
            return st::fix_wp; // unknown — generic
        }

        // Classify an airway ID into a coarse class. Two-letter
        // prefixes are checked first so e.g. "AT" doesn't collide
        // with "A".
        awy_class classify_airway_id(const std::string& id)
        {
            using ac = awy_class;
            if(id.size() >= 2)
            {
                auto p2 = id.substr(0, 2);
                if(p2 == "AT" || p2 == "AR" || p2 == "BR" || p2 == "BF" || p2 == "PA" || p2 == "PR")
                {
                    return ac::other;
                }
                if(p2 == "TK")
                {
                    return ac::rnav;
                }
            }
            switch(id.empty() ? '\0' : id[0])
            {
            case 'V':
                return ac::victor;
            case 'J':
                return ac::jet;
            case 'T':
            case 'Q':
                return ac::rnav;
            case 'G':
            case 'A':
            case 'R':
            case 'B':
                return ac::color;
            default:
                return ac::other;
            }
        }

        // Subtypes whose cost is forced to INCLUDE when use_airways
        // is true (mirrors g3xfplan's --airway override).
        bool is_airway_routable_subtype(wp_subtype st)
        {
            using ws = wp_subtype;
            switch(st)
            {
            case ws::navaid_vor:
            case ws::navaid_vortac:
            case ws::navaid_vor_dme:
            case ws::navaid_dme:
            case ws::navaid_ndb:
            case ws::navaid_ndb_dme:
            case ws::fix_wp:
            case ws::fix_rp:
            case ws::fix_cn:
            case ws::fix_mr:
                return true;
            default:
                return false;
            }
        }

        double effective_wp_cost(wp_subtype st, const route_planner::options& opts)
        {
            if(opts.use_airways && is_airway_routable_subtype(st))
            {
                return cost_include;
            }
            return opts.wp_cost.at(static_cast<std::size_t>(st));
        }
    }

    struct route_planner::impl
    {
        std::vector<node> nodes;
        std::unordered_map<std::string, std::size_t> index_by_id;
        // All indices matching a given ID. Used internally to
        // disambiguate airway endpoints by coordinates when multiple
        // fixes/navaids share a name.
        std::unordered_map<std::string, std::vector<std::size_t>> indices_by_id;
        // Adjacency list keyed by node index. Indices are dense in
        // [0, nodes.size()), so a flat vector is one indirection
        // shorter than the unordered_map we used to use.
        std::vector<std::vector<airway_edge>> neighbors;
        // In-memory 1° grid replacing the per-table SQLite R*Tree
        // queries that used to run inside the A* hot loop.
        spatial_grid grid;

        // Reusable A* state. plan_segment is single-threaded in
        // practice — route_submitter owns the only thread that calls
        // it — so we keep one scratch buffer here and reset it via
        // the dirty list between runs.
        mutable astar_scratch scratch;

        const nasr_database& db;

        explicit impl(const nasr_database& db) : db(db)
        {
        }

        std::size_t resolve_nearest(const std::string& id, double lat, double lon) const
        {
            auto it = indices_by_id.find(id);
            if(it == indices_by_id.end() || it->second.empty())
            {
                return synthetic;
            }
            const auto& candidates = it->second;
            auto best = candidates.front();
            auto best_d = haversine_nm(lat, lon, nodes[best].lat, nodes[best].lon);
            for(std::size_t i = 1; i < candidates.size(); ++i)
            {
                auto c = candidates[i];
                auto d = haversine_nm(lat, lon, nodes[c].lat, nodes[c].lon);
                if(d < best_d)
                {
                    best_d = d;
                    best = c;
                }
            }
            return best;
        }
    };

    route_planner::route_planner(const nasr_database& db) : pimpl(std::make_unique<impl>(db))
    {
        auto add_node = [&](std::string id, node_kind kind, wp_subtype sub, double lat, double lon)
        {
            auto idx = pimpl->nodes.size();
            pimpl->nodes.push_back({std::move(id), kind, sub, lat, lon});
            const auto& nid = pimpl->nodes.back().id;
            pimpl->index_by_id.emplace(nid, idx);
            pimpl->indices_by_id[nid].push_back(idx);
        };

        for(auto& r : db.load_routable_airports())
        {
            add_node(std::move(r.id), node_kind::airport, classify_airport_subtype(r.type_code), r.lat, r.lon);
        }
        for(auto& r : db.load_routable_navaids())
        {
            add_node(std::move(r.id), node_kind::navaid, classify_navaid_subtype(r.type_code), r.lat, r.lon);
        }
        for(auto& r : db.load_routable_fixes())
        {
            add_node(std::move(r.id), node_kind::fix, classify_fix_subtype(r.type_code), r.lat, r.lon);
        }

        // Build the spatial grid once now that nodes are stable.
        pimpl->grid.reset();
        for(std::size_t i = 0; i < pimpl->nodes.size(); ++i)
        {
            pimpl->grid.insert(i, pimpl->nodes[i].lat, pimpl->nodes[i].lon);
        }

        pimpl->neighbors.assign(pimpl->nodes.size(), {});
        for(auto& e : db.load_airway_edges())
        {
            auto from_idx = pimpl->resolve_nearest(e.from_id, e.from_lat, e.from_lon);
            auto to_idx = pimpl->resolve_nearest(e.to_id, e.to_lat, e.to_lon);
            if(from_idx == synthetic || to_idx == synthetic)
            {
                continue;
            }
            auto cls = classify_airway_id(e.awy_id);
            pimpl->neighbors[from_idx].push_back({to_idx, e.awy_id, cls, e.is_gap});
            pimpl->neighbors[to_idx].push_back({from_idx, std::move(e.awy_id), cls, e.is_gap});
        }
    }

    route_planner::~route_planner() = default;

    std::size_t route_planner::node_count() const
    {
        return pimpl->nodes.size();
    }

    const route_planner::node& route_planner::get_node(std::size_t index) const
    {
        if(index >= pimpl->nodes.size())
        {
            throw std::out_of_range("route_planner::get_node");
        }
        return pimpl->nodes[index];
    }

    std::optional<std::size_t> route_planner::node_index(const std::string& id) const
    {
        auto it = pimpl->index_by_id.find(id);
        if(it == pimpl->index_by_id.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    const std::vector<route_planner::airway_edge>& route_planner::airway_neighbors(std::size_t index) const
    {
        static const std::vector<airway_edge> empty;
        if(index >= pimpl->neighbors.size())
        {
            return empty;
        }
        return pimpl->neighbors[index];
    }

    namespace
    {
        // Sentinel predecessor value used when the predecessor is the
        // origin endpoint (which has no graph index in the intermediate
        // path reconstruction).
        constexpr auto PREV_FROM_ORIGIN = static_cast<std::size_t>(-1);

        struct open_entry
        {
            double f;
            std::size_t node;
            // std::priority_queue is a max-heap; invert so smallest f
            // pops first.
            bool operator<(const open_entry& o) const
            {
                return f > o.f;
            }
        };
    }

    std::optional<std::vector<std::size_t>> route_planner::plan_segment(const endpoint& origin,
                                                                        const endpoint& destination,
                                                                        const options& opts) const
    {
        const auto& nodes = pimpl->nodes;
        const auto max_leg = opts.max_leg_length_nm;

        // Direct leg short-circuit. Note: even at uniform cost,
        // this is correct because no intermediate path can be
        // cheaper than the direct great-circle when the heuristic
        // is admissible.
        if(haversine_nm(origin.lat, origin.lon, destination.lat, destination.lon) <= max_leg)
        {
            return std::vector<std::size_t>{};
        }

        // Pre-compute the per-edge minimum cost factor for the
        // heuristic. We need an admissible underestimate of any
        // single-edge cost factor — using the minimum of effective
        // wp_cost (squared, for from×to) times the minimum effective
        // airway factor keeps the heuristic admissible while
        // staying as tight as possible.
        double min_wp = cost_reject;
        for(std::size_t i = 0; i < opts.wp_cost.size(); ++i)
        {
            min_wp = std::min(min_wp, effective_wp_cost(static_cast<wp_subtype>(i), opts));
        }
        double min_awy = 1.0;
        if(opts.use_airways)
        {
            for(auto c : opts.awy_cost)
            {
                min_awy = std::min(min_awy, c);
            }
            min_awy = std::min(min_awy, opts.gap_cost);
        }
        const double heuristic_factor = min_wp * min_wp * min_awy;

        constexpr auto INF = std::numeric_limits<double>::infinity();
        constexpr auto NPOS = static_cast<std::size_t>(-1);

        const auto N = nodes.size();
        auto& sc = pimpl->scratch;
        if(sc.g.size() != N)
        {
            sc.g.assign(N, INF);
            sc.came_from.assign(N, NPOS);
            sc.closed.assign(N, 0);
            sc.dirty.clear();
        }
        else
        {
            // Reset only the entries the previous run touched.
            for(auto i : sc.dirty)
            {
                sc.g[i] = INF;
                sc.came_from[i] = NPOS;
                sc.closed[i] = 0;
            }
            sc.dirty.clear();
        }

        std::priority_queue<open_entry> open;

        auto heuristic = [&](std::size_t n) -> double
        { return haversine_nm(nodes[n].lat, nodes[n].lon, destination.lat, destination.lon) * heuristic_factor; };

        // Cost factor of a single A* step. `from_st` is nullopt
        // when the step originates from the synthetic origin
        // endpoint; in that case only the destination subtype
        // contributes. `airway_factor` is 1.0 for non-airway
        // edges.
        auto edge_cost = [&](std::optional<wp_subtype> from_st, wp_subtype to_st, double dist_nm, double airway_factor)
        {
            auto over_max = (dist_nm > max_leg) ? cost_reject : 1.0;
            auto from_mod = from_st ? effective_wp_cost(*from_st, opts) : 1.0;
            auto to_mod = effective_wp_cost(to_st, opts);
            return dist_nm * from_mod * to_mod * airway_factor * over_max;
        };

        auto relax = [&](std::size_t from, std::size_t to, double cost)
        {
            auto from_g = (from == PREV_FROM_ORIGIN) ? 0.0 : sc.g[from];
            auto tentative = from_g + cost;
            if(tentative >= sc.g[to])
            {
                return;
            }
            if(sc.g[to] == INF)
            {
                sc.dirty.push_back(to);
            }
            sc.g[to] = tentative;
            sc.came_from[to] = from;
            open.push({tentative + heuristic(to), to});
        };

        auto expand_around = [&](double from_lat, double from_lon, std::size_t from_index)
        {
            auto bbox = bbox_around(from_lat, from_lon, max_leg);
            std::optional<wp_subtype> from_st;
            if(from_index != PREV_FROM_ORIGIN && from_index != synthetic)
            {
                from_st = nodes[from_index].subtype;
            }

            sc.hits.clear();
            pimpl->grid.query(bbox, sc.hits);
            for(auto to_idx : sc.hits)
            {
                if(to_idx == from_index)
                {
                    continue;
                }
                if(sc.closed[to_idx])
                {
                    continue;
                }
                auto d = haversine_nm(from_lat, from_lon, nodes[to_idx].lat, nodes[to_idx].lon);
                if(d > max_leg)
                {
                    continue;
                }
                auto cost = edge_cost(from_st, nodes[to_idx].subtype, d, 1.0);
                relax(from_index, to_idx, cost);
            }

            // Airway neighbors get an alternate cost path that
            // (when use_airways is enabled) applies the
            // airway-class modifier. relax keeps whichever cost is
            // lower.
            if(from_index != synthetic && from_index != PREV_FROM_ORIGIN)
            {
                for(const auto& e : airway_neighbors(from_index))
                {
                    if(sc.closed[e.neighbor_index])
                    {
                        continue;
                    }
                    auto d = haversine_nm(from_lat, from_lon, nodes[e.neighbor_index].lat, nodes[e.neighbor_index].lon);
                    if(d > max_leg)
                    {
                        continue;
                    }
                    auto awy_factor = 1.0;
                    if(opts.use_airways)
                    {
                        awy_factor = e.is_gap ? opts.gap_cost : opts.awy_cost.at(static_cast<std::size_t>(e.type));
                    }
                    auto cost = edge_cost(from_st, nodes[e.neighbor_index].subtype, d, awy_factor);
                    relax(from_index, e.neighbor_index, cost);
                }
            }
        };

        // Seed: expand around origin. The predecessor is PREV_FROM_ORIGIN
        // so path reconstruction knows to stop.
        expand_around(origin.lat, origin.lon, PREV_FROM_ORIGIN);

        while(!open.empty())
        {
            auto [f, n] = open.top();
            open.pop();
            if(sc.closed[n])
            {
                continue;
            }
            sc.closed[n] = 1;

            // Goal test: if the final direct leg from n to destination
            // fits within max_leg, n is the last intermediate node.
            if(haversine_nm(nodes[n].lat, nodes[n].lon, destination.lat, destination.lon) <= max_leg)
            {
                std::vector<std::size_t> path;
                auto cur = n;
                while(cur != PREV_FROM_ORIGIN)
                {
                    path.push_back(cur);
                    cur = sc.came_from[cur];
                }
                std::reverse(path.begin(), path.end());
                return path;
            }

            expand_around(nodes[n].lat, nodes[n].lon, n);
        }

        return std::nullopt;
    }

    namespace
    {
        // Tokenize a route string for sigil expansion: split on
        // whitespace, treat `?` as its own token even when unspaced
        // ("KSMF?KBFL" → three tokens), and upper-case every token.
        std::vector<std::string> tokenize_with_sigils(const std::string& text)
        {
            std::string normalized;
            normalized.reserve(text.size() + 8);
            for(char c : text)
            {
                if(c == '?')
                {
                    normalized += ' ';
                    normalized += '?';
                    normalized += ' ';
                }
                else
                {
                    normalized += c;
                }
            }

            std::vector<std::string> tokens;
            std::istringstream iss(normalized);
            std::string tok;
            while(iss >> tok)
            {
                for(auto& c : tok)
                {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                tokens.push_back(std::move(tok));
            }
            return tokens;
        }
    }

    namespace
    {
        bool is_airway_token(const std::string& token, const nasr_database& db)
        {
            return !db.query_airway_by_id(token).empty();
        }

        // Pick the fix along `airway_id` that a point at (lat, lon)
        // projects closest to, then choose the adjacent fix that is
        // closer to (toward_lat, toward_lon). The "project-and-walk"
        // selection: the projected point lies on some segment
        // Fi-Fi+1; we return whichever of {Fi, Fi+1} points in the
        // direction we intend to traverse the airway.
        struct project_walk_result
        {
            std::string fix_id;
            double lat;
            double lon;
        };

        project_walk_result project_and_walk(const std::string& airway_id, double lat, double lon, double toward_lat,
                                             double toward_lon, const nasr_database& db)
        {
            auto segs = db.query_airway_by_id(airway_id);
            if(segs.empty())
            {
                throw route_parse_error("airway has no segments: " + airway_id);
            }
            std::sort(segs.begin(), segs.end(),
                      [](const airway_segment& a, const airway_segment& b) { return a.point_seq < b.point_seq; });

            // Find the segment whose clamped projection minimizes
            // distance from the point. The "segment" here is one
            // AWY_SEG row; its endpoints are the candidate flanking
            // fixes.
            std::size_t best = 0;
            auto best_dist = std::numeric_limits<double>::infinity();
            for(std::size_t i = 0; i < segs.size(); ++i)
            {
                auto d = point_to_segment_distance_nm(segs[i].from_lat, segs[i].from_lon, segs[i].to_lat,
                                                      segs[i].to_lon, lat, lon);
                if(d < best_dist)
                {
                    best_dist = d;
                    best = i;
                }
            }

            const auto& s = segs[best];
            auto d_from = haversine_nm(s.from_lat, s.from_lon, toward_lat, toward_lon);
            auto d_to = haversine_nm(s.to_lat, s.to_lon, toward_lat, toward_lon);
            if(d_from <= d_to)
            {
                return {s.from_point, s.from_lat, s.from_lon};
            }
            return {s.to_point, s.to_lat, s.to_lon};
        }
    }

    std::string route_planner::expand_sigils(const std::string& text, const options& opts) const
    {
        auto tokens = tokenize_with_sigils(text);

        std::vector<std::size_t> sigils;
        for(std::size_t i = 0; i < tokens.size(); ++i)
        {
            if(tokens[i] == "?")
            {
                sigils.push_back(i);
            }
        }

        // Fast path: no sigils → unchanged.
        if(sigils.empty())
        {
            return text;
        }

        // Validate basic sigil grammar. Airway-adjacency is checked
        // on demand during substitution below — an airway token is a
        // legal sigil neighbor, but its OWN other side must be a
        // point waypoint.
        for(auto s : sigils)
        {
            if(s == 0)
            {
                throw route_parse_error("route cannot start with '?'", "?", static_cast<int>(s));
            }
            if(s + 1 >= tokens.size())
            {
                throw route_parse_error("route cannot end with '?'", "?", static_cast<int>(s));
            }
            if(tokens[s - 1] == "?" || tokens[s + 1] == "?")
            {
                throw route_parse_error("consecutive '?' not allowed", "?", static_cast<int>(s));
            }
        }

        // Resolve a point-waypoint token to (lat, lon). Accepts a
        // coordinate literal or any waypoint ID in the planner's
        // catalog. Airway tokens fail — that's intentional; the
        // caller is expected to classify the token before calling.
        auto resolve_point = [&](const std::string& tok, int token_index) -> std::pair<double, double>
        {
            if(auto ll = parse_latlon(tok))
            {
                return {ll->lat, ll->lon};
            }
            if(auto idx = node_index(tok))
            {
                const auto& n = pimpl->nodes[*idx];
                return {n.lat, n.lon};
            }
            throw route_parse_error("'" + tok +
                                        "' cannot be used as a route waypoint — "
                                        "expected airport / navaid / fix / lat-lon",
                                    tok, token_index);
        };

        // Look for the point waypoint on the side of an airway
        // opposite to the sigil. `airway_pos` is the index of the
        // airway token; `direction` is +1 (look past the airway's
        // exit side) or -1 (look past the entry side). A `?` token
        // in that direction is skipped once to find the underlying
        // point.
        auto point_past_airway = [&](std::size_t airway_pos, int direction) -> std::pair<double, double>
        {
            auto idx = static_cast<long long>(airway_pos) + direction;
            if(idx < 0 || idx >= static_cast<long long>(tokens.size()))
            {
                throw route_parse_error("airway '" + tokens[airway_pos] +
                                            "' adjacent to '?' needs a point waypoint on its other side",
                                        tokens[airway_pos], static_cast<int>(airway_pos));
            }
            if(tokens[idx] == "?")
            {
                idx += direction;
                if(idx < 0 || idx >= static_cast<long long>(tokens.size()))
                {
                    throw route_parse_error("airway '" + tokens[airway_pos] +
                                                "' adjacent to '?' needs a point waypoint on its other side",
                                            tokens[airway_pos], static_cast<int>(airway_pos));
                }
            }
            return resolve_point(tokens[idx], static_cast<int>(idx));
        };

        std::vector<std::string> out;
        out.reserve(tokens.size());
        std::size_t cursor = 0;

        for(auto s : sigils)
        {
            // Emit tokens between the previous cursor and this sigil
            // verbatim. Airway tokens sitting in this range are
            // emitted unchanged — they're handled by flight_route's
            // own airway shorthand. Sigil-adjacent airways only
            // affect the rewriting at the `?` itself.
            for(auto k = cursor; k < s; ++k)
            {
                out.push_back(tokens[k]);
            }

            const auto& left_tok = tokens[s - 1];
            const auto& right_tok = tokens[s + 1];
            bool left_airway = is_airway_token(left_tok, pimpl->db);
            bool right_airway = is_airway_token(right_tok, pimpl->db);

            if(left_airway && right_airway)
            {
                throw route_parse_error("'?' cannot have an airway on both sides", "?", static_cast<int>(s));
            }

            if(!left_airway && !right_airway)
            {
                // Point-to-point planning.
                auto [fl, gl] = resolve_point(left_tok, static_cast<int>(s - 1));
                auto [fr, gr] = resolve_point(right_tok, static_cast<int>(s + 1));
                auto path = plan_segment(endpoint{synthetic, fl, gl}, endpoint{synthetic, fr, gr}, opts);
                if(!path)
                {
                    std::string msg = "no route from ";
                    msg += left_tok;
                    msg += " to ";
                    msg += right_tok;
                    throw route_parse_error(msg);
                }
                for(auto idx : *path)
                {
                    out.push_back(pimpl->nodes[idx].id);
                }
            }
            else if(right_airway)
            {
                // Pattern: A ? X. Plan A → entry_fix of X, where
                // entry_fix is project-and-walk from A using the
                // point past X as the direction reference.
                auto [fl, gl] = resolve_point(left_tok, static_cast<int>(s - 1));
                auto [tl, gtl] = point_past_airway(s + 1, +1);
                auto entry = project_and_walk(right_tok, fl, gl, tl, gtl, pimpl->db);
                auto path = plan_segment(endpoint{synthetic, fl, gl}, endpoint{synthetic, entry.lat, entry.lon}, opts);
                if(!path)
                {
                    std::string msg = "no route from ";
                    msg += left_tok;
                    msg += " to ";
                    msg += entry.fix_id;
                    msg += " (entry of ";
                    msg += right_tok;
                    msg += ")";
                    throw route_parse_error(msg);
                }
                for(auto idx : *path)
                {
                    out.push_back(pimpl->nodes[idx].id);
                }
                out.push_back(entry.fix_id);
            }
            else
            {
                // Pattern: X ? B. Pick exit_fix of X via
                // project-and-walk from B, then plan exit_fix → B.
                auto [fr, gr] = resolve_point(right_tok, static_cast<int>(s + 1));
                auto [tl, gtl] = point_past_airway(s - 1, -1);
                auto exit = project_and_walk(left_tok, fr, gr, tl, gtl, pimpl->db);
                out.push_back(exit.fix_id);
                auto path = plan_segment(endpoint{synthetic, exit.lat, exit.lon}, endpoint{synthetic, fr, gr}, opts);
                if(!path)
                {
                    std::string msg = "no route from ";
                    msg += exit.fix_id;
                    msg += " (exit of ";
                    msg += left_tok;
                    msg += ") to ";
                    msg += right_tok;
                    throw route_parse_error(msg);
                }
                for(auto idx : *path)
                {
                    out.push_back(pimpl->nodes[idx].id);
                }
            }

            cursor = s + 1; // skip the '?' marker
        }
        for(auto k = cursor; k < tokens.size(); ++k)
        {
            out.push_back(tokens[k]);
        }

        // Reassemble with single-space separators.
        std::string joined;
        for(std::size_t k = 0; k < out.size(); ++k)
        {
            if(k > 0)
            {
                joined += ' ';
            }
            joined += out[k];
        }
        return joined;
    }

    flight_route route_planner::parse(const std::string& text, const options& opts) const
    {
        return {expand_sigils(text, opts), pimpl->db};
    }

} // namespace osect
