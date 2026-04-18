#include "feature_builder.hpp"
#include "chart_style.hpp"
#include "feature_type.hpp"
#include "map_view.hpp"
#include "nasr_database.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace nasrbrowse
{
    bool build_context::is_at_navaid(float x, float y) const
    {
        constexpr float NAVAID_OVERLAP_TOL = 0.1F;
        float tol = state.navaid_clearance * NAVAID_OVERLAP_TOL;
        float tol_sq = tol * tol;
        for(const auto& np : state.navaid_positions)
        {
            float dx = x - np.x;
            float dy = y - np.y;
            if(dx * dx + dy * dy < tol_sq) return true;
        }
        return false;
    }

    bool build_context::fix_on_airway(const std::string& fix_id) const
    {
        return state.airway_waypoints.find(fix_id) != state.airway_waypoints.end();
    }

    struct feature_builder::impl
    {
        nasr_database db;
        chart_style styles;
        std::vector<std::unique_ptr<feature_type>> types;

        mutable std::mutex mutex;
        std::condition_variable cv;
        std::optional<feature_build_request> pending_request;
        std::optional<feature_build_result> completed_result;
        std::thread worker;
        bool shutdown = false;

        // Worker-thread scratch state; reset per build pass.
        feature_build_state state;
        std::array<polyline_data, layer_sdf_count> poly;
        std::vector<label_candidate> labels;

        impl(const char* db_path, const chart_style& cs)
            : db(db_path)
            , styles(cs)
            , types(make_feature_types())
        {
        }

        // Dispatch a single build pass at a given horizontal Mercator
        // offset (0 for the primary pass; ±WORLD_SIZE for antimeridian
        // wrap copies).
        void build_all_features(const geo_bbox& bbox, const feature_build_request& req,
                                 double mx_offset)
        {
            feature_build_request local = req;
            local.lon_min = bbox.lon_min;
            local.lat_min = bbox.lat_min;
            local.lon_max = bbox.lon_max;
            local.lat_max = bbox.lat_max;

            build_context ctx{db, styles, local, mx_offset, poly, labels, state};
            for(const auto& t : types)
                t->build(ctx);
        }

        void build_vertices(const feature_build_request& req)
        {
            constexpr double WORLD_SIZE = 2.0 * HALF_CIRCUMFERENCE;

            geo_bbox qbox{req.lon_min, req.lat_min, req.lon_max, req.lat_max};

            for(auto& p : poly) p.clear();
            labels.clear();

            build_all_features(qbox, req, 0.0);

            if(qbox.lon_max > 180.0)
            {
                geo_bbox shifted{qbox.lon_min - 360.0, qbox.lat_min,
                                 qbox.lon_max - 360.0, qbox.lat_max};
                build_all_features(shifted, req, WORLD_SIZE);
            }
            if(qbox.lon_min < -180.0)
            {
                geo_bbox shifted{qbox.lon_min + 360.0, qbox.lat_min,
                                 qbox.lon_max + 360.0, qbox.lat_max};
                build_all_features(shifted, req, -WORLD_SIZE);
            }
        }

        // Build the halo + re-emitted icon/outline for the currently-
        // selected feature, dispatching to the feature_type that owns it.
        void build_selection_overlay(const feature_build_request& req,
                                      const feature& sel,
                                      polyline_data& out,
                                      polygon_fill_data& fill_out)
        {
            build_context ctx{db, styles, req, 0.0, poly, labels, state};
            find_feature_type(types, sel).build_selection(ctx, sel, out, fill_out);
        }

        void worker_loop()
        {
            while(true)
            {
                feature_build_request req;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this] { return shutdown || pending_request.has_value(); });
                    if(shutdown) return;
                    req = *pending_request;
                    pending_request.reset();
                }

                build_vertices(req);

                feature_build_result result;
                result.poly = std::move(poly);
                if(req.selection)
                    build_selection_overlay(req, *req.selection,
                                             result.selection_overlay,
                                             result.selection_fill);
                result.labels = std::move(labels);

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    completed_result = std::move(result);
                }
            }
        }
    };

    feature_builder::feature_builder(const char* db_path, const chart_style& cs)
        : pimpl(new impl(db_path, cs))
    {
        pimpl->worker = std::thread(&impl::worker_loop, pimpl.get());
    }

    feature_builder::~feature_builder()
    {
        {
            std::lock_guard<std::mutex> lock(pimpl->mutex);
            pimpl->shutdown = true;
        }
        pimpl->cv.notify_one();
        pimpl->worker.join();
    }

    void feature_builder::request(const feature_build_request& req)
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        pimpl->pending_request = req;
        pimpl->cv.notify_one();
    }

    std::optional<feature_build_result> feature_builder::drain_result()
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        std::optional<feature_build_result> result;
        result.swap(pimpl->completed_result);
        return result;
    }

} // namespace nasrbrowse
