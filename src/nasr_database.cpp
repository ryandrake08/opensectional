#include "nasr_database.hpp"
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace nasrbrowse
{
    // Helper to extract a text column, returning empty string for NULL
    static std::string col_text(sqlite3_stmt* stmt, int col)
    {
        const unsigned char* text = sqlite3_column_text(stmt, col);
        return text ? std::string(reinterpret_cast<const char*>(text)) : std::string();
    }

    static double col_double(sqlite3_stmt* stmt, int col)
    {
        return sqlite3_column_double(stmt, col);
    }

    struct nasr_database::impl
    {
        sqlite3* db;

        sqlite3_stmt* stmt_airports;
        sqlite3_stmt* stmt_navaids;
        sqlite3_stmt* stmt_fixes;
        sqlite3_stmt* stmt_airways;
        sqlite3_stmt* stmt_airspaces;
        sqlite3_stmt* stmt_airspace_shape;
        sqlite3_stmt* stmt_cls_arsp;
        sqlite3_stmt* stmt_cls_arsp_shape;

        std::vector<airport> airports;
        std::vector<navaid> navaids;
        std::vector<fix> fixes;
        std::vector<airway_segment> airways;
        std::vector<airspace> airspaces;
        std::vector<class_airspace> class_airspaces;

        impl(const char* db_path)
            : db(nullptr)
            , stmt_airports(nullptr)
            , stmt_navaids(nullptr)
            , stmt_fixes(nullptr)
            , stmt_airways(nullptr)
            , stmt_airspaces(nullptr)
            , stmt_airspace_shape(nullptr)
            , stmt_cls_arsp(nullptr)
            , stmt_cls_arsp_shape(nullptr)
        {
            int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, nullptr);
            if(rc != SQLITE_OK)
            {
                std::string msg = "Failed to open NASR database: ";
                msg += sqlite3_errmsg(db);
                sqlite3_close(db);
                throw std::runtime_error(msg);
            }

            prepare_statements();
        }

        ~impl()
        {
            sqlite3_finalize(stmt_airports);
            sqlite3_finalize(stmt_navaids);
            sqlite3_finalize(stmt_fixes);
            sqlite3_finalize(stmt_airways);
            sqlite3_finalize(stmt_airspaces);
            sqlite3_finalize(stmt_airspace_shape);
            sqlite3_finalize(stmt_cls_arsp);
            sqlite3_finalize(stmt_cls_arsp_shape);
            sqlite3_close(db);
        }

        void prepare_statements()
        {
            // R-tree overlap query: find points where the stored point
            // (min=max) overlaps with the search box
            prepare(&stmt_airports, R"(
                SELECT ARPT_ID, ARPT_NAME, SITE_TYPE_CODE, TWR_TYPE_CODE,
                       ICAO_ID, LAT_DECIMAL, LONG_DECIMAL, ELEV
                FROM APT_BASE
                WHERE rowid IN (
                    SELECT id FROM APT_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_navaids, R"(
                SELECT NAV_ID, NAV_TYPE, NAME, FREQ, LAT_DECIMAL, LONG_DECIMAL
                FROM NAV_BASE
                WHERE rowid IN (
                    SELECT id FROM NAV_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_fixes, R"(
                SELECT FIX_ID, LAT_DECIMAL, LONG_DECIMAL
                FROM FIX_BASE
                WHERE rowid IN (
                    SELECT id FROM FIX_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_airways, R"(
                SELECT AWY_ID, FROM_POINT, TO_POINT,
                       FROM_LAT, FROM_LON, TO_LAT, TO_LON
                FROM AWY_SEG
                WHERE rowid IN (
                    SELECT id FROM AWY_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            // Airspace: query base records whose bounding box overlaps
            prepare(&stmt_airspaces, R"(
                SELECT MAA_ID, MAA_TYPE_NAME, MAA_NAME
                FROM MAA_BASE
                WHERE rowid IN (
                    SELECT id FROM MAA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            // Shape points for a specific airspace
            prepare(&stmt_airspace_shape, R"(
                SELECT LAT_DECIMAL, LON_DECIMAL
                FROM MAA_SHP
                WHERE MAA_ID = ?1
                ORDER BY POINT_SEQ
            )");

            // Class airspace (B/C/D/E) from shapefile
            prepare(&stmt_cls_arsp, R"(
                SELECT ARSP_ID, NAME, CLASS, LOCAL_TYPE, UPPER_VAL, LOWER_VAL
                FROM CLS_ARSP_BASE
                WHERE ARSP_ID IN (
                    SELECT id FROM CLS_ARSP_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_cls_arsp_shape, R"(
                SELECT PART_NUM, LON_DECIMAL, LAT_DECIMAL
                FROM CLS_ARSP_SHP
                WHERE ARSP_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )");
        }

        void prepare(sqlite3_stmt** stmt, const char* sql)
        {
            int rc = sqlite3_prepare_v2(db, sql, -1, stmt, nullptr);
            if(rc != SQLITE_OK)
            {
                std::string msg = "Failed to prepare statement: ";
                msg += sqlite3_errmsg(db);
                throw std::runtime_error(msg);
            }
        }

        void bind_bbox(sqlite3_stmt* stmt, double lon_min, double lat_min,
                       double lon_max, double lat_max)
        {
            sqlite3_reset(stmt);
            sqlite3_bind_double(stmt, 1, lon_min);
            sqlite3_bind_double(stmt, 2, lat_min);
            sqlite3_bind_double(stmt, 3, lon_max);
            sqlite3_bind_double(stmt, 4, lat_max);
        }
    };

    nasr_database::nasr_database(const char* db_path)
        : pimpl(new impl(db_path))
    {
    }

    nasr_database::~nasr_database() = default;

    const std::vector<airport>& nasr_database::query_airports(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        d.airports.clear();
        d.bind_bbox(d.stmt_airports, lon_min, lat_min, lon_max, lat_max);

        while(sqlite3_step(d.stmt_airports) == SQLITE_ROW)
        {
            airport a;
            a.arpt_id = col_text(d.stmt_airports, 0);
            a.arpt_name = col_text(d.stmt_airports, 1);
            a.site_type_code = col_text(d.stmt_airports, 2);
            a.twr_type_code = col_text(d.stmt_airports, 3);
            a.icao_id = col_text(d.stmt_airports, 4);
            a.lat = col_double(d.stmt_airports, 5);
            a.lon = col_double(d.stmt_airports, 6);
            a.elev = col_double(d.stmt_airports, 7);
            d.airports.push_back(std::move(a));
        }

        return d.airports;
    }

    const std::vector<navaid>& nasr_database::query_navaids(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        d.navaids.clear();
        d.bind_bbox(d.stmt_navaids, lon_min, lat_min, lon_max, lat_max);

        while(sqlite3_step(d.stmt_navaids) == SQLITE_ROW)
        {
            navaid n;
            n.nav_id = col_text(d.stmt_navaids, 0);
            n.nav_type = col_text(d.stmt_navaids, 1);
            n.name = col_text(d.stmt_navaids, 2);
            n.freq = col_text(d.stmt_navaids, 3);
            n.lat = col_double(d.stmt_navaids, 4);
            n.lon = col_double(d.stmt_navaids, 5);
            d.navaids.push_back(std::move(n));
        }

        return d.navaids;
    }

    const std::vector<fix>& nasr_database::query_fixes(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        d.fixes.clear();
        d.bind_bbox(d.stmt_fixes, lon_min, lat_min, lon_max, lat_max);

        while(sqlite3_step(d.stmt_fixes) == SQLITE_ROW)
        {
            fix f;
            f.fix_id = col_text(d.stmt_fixes, 0);
            f.lat = col_double(d.stmt_fixes, 1);
            f.lon = col_double(d.stmt_fixes, 2);
            d.fixes.push_back(std::move(f));
        }

        return d.fixes;
    }

    const std::vector<airway_segment>& nasr_database::query_airways(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        d.airways.clear();
        d.bind_bbox(d.stmt_airways, lon_min, lat_min, lon_max, lat_max);

        while(sqlite3_step(d.stmt_airways) == SQLITE_ROW)
        {
            airway_segment s;
            s.awy_id = col_text(d.stmt_airways, 0);
            s.from_point = col_text(d.stmt_airways, 1);
            s.to_point = col_text(d.stmt_airways, 2);
            s.from_lat = col_double(d.stmt_airways, 3);
            s.from_lon = col_double(d.stmt_airways, 4);
            s.to_lat = col_double(d.stmt_airways, 5);
            s.to_lon = col_double(d.stmt_airways, 6);
            d.airways.push_back(std::move(s));
        }

        return d.airways;
    }

    const std::vector<airspace>& nasr_database::query_airspaces(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        d.airspaces.clear();
        d.bind_bbox(d.stmt_airspaces, lon_min, lat_min, lon_max, lat_max);

        while(sqlite3_step(d.stmt_airspaces) == SQLITE_ROW)
        {
            airspace a;
            a.maa_id = col_text(d.stmt_airspaces, 0);
            a.maa_type = col_text(d.stmt_airspaces, 1);
            a.maa_name = col_text(d.stmt_airspaces, 2);

            // Load shape points
            sqlite3_reset(d.stmt_airspace_shape);
            sqlite3_bind_text(d.stmt_airspace_shape, 1, a.maa_id.c_str(), -1, SQLITE_TRANSIENT);
            while(sqlite3_step(d.stmt_airspace_shape) == SQLITE_ROW)
            {
                airspace_point pt;
                pt.lat = col_double(d.stmt_airspace_shape, 0);
                pt.lon = col_double(d.stmt_airspace_shape, 1);
                a.shape.push_back(pt);
            }

            d.airspaces.push_back(std::move(a));
        }

        return d.airspaces;
    }

    const std::vector<class_airspace>& nasr_database::query_class_airspace(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        d.class_airspaces.clear();
        d.bind_bbox(d.stmt_cls_arsp, lon_min, lat_min, lon_max, lat_max);

        while(sqlite3_step(d.stmt_cls_arsp) == SQLITE_ROW)
        {
            class_airspace a;
            a.arsp_id = sqlite3_column_int(d.stmt_cls_arsp, 0);
            a.name = col_text(d.stmt_cls_arsp, 1);
            a.airspace_class = col_text(d.stmt_cls_arsp, 2);
            a.local_type = col_text(d.stmt_cls_arsp, 3);
            a.upper_val = col_text(d.stmt_cls_arsp, 4);
            a.lower_val = col_text(d.stmt_cls_arsp, 5);

            // Load shape parts
            sqlite3_reset(d.stmt_cls_arsp_shape);
            sqlite3_bind_int(d.stmt_cls_arsp_shape, 1, a.arsp_id);
            int current_part = -1;
            while(sqlite3_step(d.stmt_cls_arsp_shape) == SQLITE_ROW)
            {
                int part_num = sqlite3_column_int(d.stmt_cls_arsp_shape, 0);
                if(part_num != current_part)
                {
                    a.parts.emplace_back();
                    current_part = part_num;
                }
                airspace_point pt;
                pt.lon = col_double(d.stmt_cls_arsp_shape, 1);
                pt.lat = col_double(d.stmt_cls_arsp_shape, 2);
                a.parts.back().push_back(pt);
            }

            d.class_airspaces.push_back(std::move(a));
        }

        return d.class_airspaces;
    }

} // namespace nasrbrowse
