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
        sqlite3_stmt* stmt_mtrs;
        sqlite3_stmt* stmt_maas;
        sqlite3_stmt* stmt_maa_shape;
        sqlite3_stmt* stmt_cls_arsp;
        sqlite3_stmt* stmt_cls_arsp_shape;
        sqlite3_stmt* stmt_runways;
        sqlite3_stmt* stmt_sua;
        sqlite3_stmt* stmt_sua_shape;
        sqlite3_stmt* stmt_obstacles;
        sqlite3_stmt* stmt_artcc;
        sqlite3_stmt* stmt_artcc_shape;
        sqlite3_stmt* stmt_pjas;
        sqlite3_stmt* stmt_adiz;
        sqlite3_stmt* stmt_adiz_shape;
        sqlite3_stmt* stmt_fss;
        sqlite3_stmt* stmt_awos;
        sqlite3_stmt* stmt_comm_outlets;

        std::vector<airport> airports;
        std::vector<navaid> navaids;
        std::vector<fix> fixes;
        std::vector<airway_segment> airways;
        std::vector<airway_segment> mtrs;
        std::vector<maa> maas;
        std::vector<class_airspace> class_airspaces;
        std::vector<runway> runways;
        std::vector<sua> suas;
        std::vector<obstacle> obstacles;
        std::vector<artcc> artccs;
        std::vector<pja> pjas;
        std::vector<adiz> adizs;
        std::vector<fss> fsss;
        std::vector<awos> awoss;
        std::vector<comm_outlet> comm_outlets;

        impl(const char* db_path)
            : db(nullptr)
            , stmt_airports(nullptr)
            , stmt_navaids(nullptr)
            , stmt_fixes(nullptr)
            , stmt_airways(nullptr)
            , stmt_mtrs(nullptr)
            , stmt_maas(nullptr)
            , stmt_maa_shape(nullptr)
            , stmt_cls_arsp(nullptr)
            , stmt_cls_arsp_shape(nullptr)
            , stmt_runways(nullptr)
            , stmt_sua(nullptr)
            , stmt_sua_shape(nullptr)
            , stmt_obstacles(nullptr)
            , stmt_artcc(nullptr)
            , stmt_artcc_shape(nullptr)
            , stmt_pjas(nullptr)
            , stmt_adiz(nullptr)
            , stmt_adiz_shape(nullptr)
            , stmt_fss(nullptr)
            , stmt_awos(nullptr)
            , stmt_comm_outlets(nullptr)
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
            sqlite3_finalize(stmt_mtrs);
            sqlite3_finalize(stmt_maas);
            sqlite3_finalize(stmt_maa_shape);
            sqlite3_finalize(stmt_cls_arsp);
            sqlite3_finalize(stmt_cls_arsp_shape);
            sqlite3_finalize(stmt_runways);
            sqlite3_finalize(stmt_sua);
            sqlite3_finalize(stmt_sua_shape);
            sqlite3_finalize(stmt_obstacles);
            sqlite3_finalize(stmt_artcc);
            sqlite3_finalize(stmt_artcc_shape);
            sqlite3_finalize(stmt_pjas);
            sqlite3_finalize(stmt_adiz);
            sqlite3_finalize(stmt_adiz_shape);
            sqlite3_finalize(stmt_fss);
            sqlite3_finalize(stmt_awos);
            sqlite3_finalize(stmt_comm_outlets);
            sqlite3_close(db);
        }

        void prepare_statements()
        {
            // R-tree overlap query: find points where the stored point
            // (min=max) overlaps with the search box
            prepare(&stmt_airports, R"(
                SELECT a.ARPT_ID, a.ARPT_NAME, a.SITE_TYPE_CODE, a.TWR_TYPE_CODE,
                       a.OWNERSHIP_TYPE_CODE, a.FACILITY_USE_CODE, a.ARPT_STATUS,
                       a.ICAO_ID, a.HARD_SURFACE, a.LAT_DECIMAL, a.LONG_DECIMAL,
                       a.ELEV,
                       CASE
                           WHEN c.CLASS_B_AIRSPACE = 'Y' THEN 'B'
                           WHEN c.CLASS_C_AIRSPACE = 'Y' THEN 'C'
                           WHEN c.CLASS_D_AIRSPACE = 'Y' THEN 'D'
                           WHEN c.CLASS_E_AIRSPACE = 'Y' THEN 'E'
                           ELSE ''
                       END,
                       a.MAG_VARN, a.MAG_HEMIS, a.TPA,
                       a.RESP_ARTCC_ID, a.FSS_ID, a.NOTAM_ID,
                       a.ACTIVATION_DATE, a.FUEL_TYPES,
                       a.LGT_SKED, a.BCN_LGT_SKED
                FROM APT_BASE a
                LEFT JOIN CLS_ARSP c ON c.SITE_NO = a.SITE_NO
                WHERE a.rowid IN (
                    SELECT id FROM APT_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_navaids, R"(
                SELECT NAV_ID, NAV_TYPE, NAME, FREQ, LAT_DECIMAL, LONG_DECIMAL
                FROM NAV_BASE
                WHERE NAV_STATUS != 'SHUTDOWN'
                AND rowid IN (
                    SELECT id FROM NAV_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_fixes, R"(
                SELECT FIX_ID, FIX_USE_CODE, LAT_DECIMAL, LONG_DECIMAL
                FROM FIX_BASE
                WHERE FIX_USE_CODE IN ('WP', 'RP', 'VFR', 'CN', 'MR', 'MW', 'NRS')
                AND rowid IN (
                    SELECT id FROM FIX_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_airways, R"(
                SELECT AWY_ID, FROM_POINT, TO_POINT,
                       FROM_LAT, FROM_LON, TO_LAT, TO_LON
                FROM AWY_SEG
                WHERE AWY_SEG_GAP_FLAG != 'Y'
                AND rowid IN (
                    SELECT id FROM AWY_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_mtrs, R"(
                SELECT MTR_ID, FROM_POINT, TO_POINT,
                       FROM_LAT, FROM_LON, TO_LAT, TO_LON
                FROM MTR_SEG
                WHERE rowid IN (
                    SELECT id FROM MTR_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            // Miscellaneous activity areas
            prepare(&stmt_maas, R"(
                SELECT MAA_ID, TYPE, NAME, LAT, LON, RADIUS_NM
                FROM MAA_BASE
                WHERE rowid IN (
                    SELECT id FROM MAA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            // Shape points for a shape-defined MAA
            prepare(&stmt_maa_shape, R"(
                SELECT LON_DECIMAL, LAT_DECIMAL
                FROM MAA_SHP
                WHERE MAA_ID = ?1
                ORDER BY POINT_SEQ
            )");

            // Class airspace (B/C/D/E) from shapefile
            prepare(&stmt_cls_arsp, R"(
                SELECT ARSP_ID, NAME, CLASS, LOCAL_TYPE,
                       UPPER_DESC, UPPER_VAL, LOWER_DESC, LOWER_VAL
                FROM CLS_ARSP_BASE
                WHERE ARSP_ID IN (
                    SELECT id FROM CLS_ARSP_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_cls_arsp_shape, R"(
                SELECT PART_NUM, IS_HOLE, LON_DECIMAL, LAT_DECIMAL
                FROM CLS_ARSP_SHP
                WHERE ARSP_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )");

            // Runways: pre-built segments with direct R-tree query
            prepare(&stmt_runways, R"(
                SELECT END1_LAT, END1_LON, END2_LAT, END2_LON
                FROM RWY_SEG
                WHERE rowid IN (
                    SELECT id FROM RWY_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            // SUA (MOA/Restricted/Warning/Alert/Prohibited) from AIXM
            prepare(&stmt_sua, R"(
                SELECT SUA_ID, DESIGNATOR, NAME, SUA_TYPE
                FROM SUA_BASE
                WHERE SUA_ID IN (
                    SELECT id FROM SUA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_sua_shape, R"(
                SELECT PART_NUM, UPPER_LIMIT, LOWER_LIMIT, LON_DECIMAL, LAT_DECIMAL
                FROM SUA_SHP
                WHERE SUA_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )");

            prepare(&stmt_obstacles, R"(
                SELECT OAS_NUM, LAT_DECIMAL, LON_DECIMAL, AGL_HT, LIGHTING
                FROM OBS_BASE
                WHERE rowid IN (
                    SELECT id FROM OBS_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_artcc, R"(
                SELECT ARTCC_ID, LOCATION_ID, LOCATION_NAME, ALTITUDE
                FROM ARTCC_BASE
                WHERE ARTCC_ID IN (
                    SELECT id FROM ARTCC_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_artcc_shape, R"(
                SELECT LON_DECIMAL, LAT_DECIMAL
                FROM ARTCC_SHP
                WHERE ARTCC_ID = ?1
                ORDER BY POINT_SEQ
            )");

            prepare(&stmt_pjas, R"(
                SELECT PJA_ID, NAME, LAT, LON, RADIUS_NM
                FROM PJA_BASE
                WHERE rowid IN (
                    SELECT id FROM PJA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_adiz, R"(
                SELECT ADIZ_ID, NAME
                FROM ADIZ_BASE
                WHERE ADIZ_ID IN (
                    SELECT id FROM ADIZ_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_adiz_shape, R"(
                SELECT PART_NUM, LON_DECIMAL, LAT_DECIMAL
                FROM ADIZ_SHP
                WHERE ADIZ_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )");

            prepare(&stmt_fss, R"(
                SELECT FSS_ID, NAME, VOICE_CALL, CITY, STATE_CODE,
                       CAST(LAT_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
                       OPR_HOURS, FAC_STATUS
                FROM FSS_BASE
                WHERE rowid IN (
                    SELECT id FROM FSS_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_awos, R"(
                SELECT ASOS_AWOS_ID, ASOS_AWOS_TYPE, CITY, STATE_CODE,
                       CAST(LAT_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
                       CAST(ELEV AS REAL), PHONE_NO, SITE_NO
                FROM AWOS
                WHERE rowid IN (
                    SELECT id FROM AWOS_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )");

            prepare(&stmt_comm_outlets, R"(
                SELECT COMM_TYPE, COMM_OUTLET_NAME, FACILITY_ID, FACILITY_NAME,
                       CAST(LAT_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL)
                FROM COM
                WHERE COMM_TYPE IN ('RCO', 'RCO1', 'RCAG')
                  AND rowid IN (
                    SELECT id FROM COM_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
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

        template<typename T, typename RowMapper>
        const std::vector<T>& query_bbox(std::vector<T>& results,
                                          sqlite3_stmt* stmt,
                                          double lon_min, double lat_min,
                                          double lon_max, double lat_max,
                                          RowMapper&& map_row)
        {
            results.clear();
            bind_bbox(stmt, lon_min, lat_min, lon_max, lat_max);
            while(sqlite3_step(stmt) == SQLITE_ROW)
            {
                results.push_back(map_row(stmt));
            }
            return results;
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
        return d.query_bbox(d.airports, d.stmt_airports,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return airport{
                col_text(s, 0), col_text(s, 1), col_text(s, 2), col_text(s, 3),
                col_text(s, 4), col_text(s, 5), col_text(s, 6), col_text(s, 7),
                sqlite3_column_int(s, 8) != 0,
                col_double(s, 9), col_double(s, 10), col_double(s, 11),
                col_text(s, 12),
                col_text(s, 13), col_text(s, 14), col_text(s, 15),
                col_text(s, 16), col_text(s, 17), col_text(s, 18),
                col_text(s, 19), col_text(s, 20),
                col_text(s, 21), col_text(s, 22)};
        });
    }

    const std::vector<navaid>& nasr_database::query_navaids(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.navaids, d.stmt_navaids,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return navaid{col_text(s, 0), col_text(s, 1), col_text(s, 2),
                          col_text(s, 3), col_double(s, 4), col_double(s, 5)};
        });
    }

    const std::vector<fix>& nasr_database::query_fixes(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.fixes, d.stmt_fixes,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return fix{col_text(s, 0), col_text(s, 1),
                       col_double(s, 2), col_double(s, 3)};
        });
    }

    const std::vector<airway_segment>& nasr_database::query_airways(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.airways, d.stmt_airways,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return airway_segment{
                col_text(s, 0), col_text(s, 1), col_text(s, 2),
                col_double(s, 3), col_double(s, 4),
                col_double(s, 5), col_double(s, 6)};
        });
    }

    const std::vector<airway_segment>& nasr_database::query_mtrs(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.mtrs, d.stmt_mtrs,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return airway_segment{
                col_text(s, 0), col_text(s, 1), col_text(s, 2),
                col_double(s, 3), col_double(s, 4),
                col_double(s, 5), col_double(s, 6)};
        });
    }

    const std::vector<maa>& nasr_database::query_maas(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.maas, d.stmt_maas,
            lon_min, lat_min, lon_max, lat_max, [&](sqlite3_stmt* s)
        {
            maa m{col_text(s, 0), col_text(s, 1), col_text(s, 2),
                  col_double(s, 3), col_double(s, 4), col_double(s, 5), {}};

            // Load shape polygon for shape-defined entries
            if(m.lat == 0.0)
            {
                sqlite3_reset(d.stmt_maa_shape);
                sqlite3_bind_text(d.stmt_maa_shape, 1, m.maa_id.c_str(), -1, SQLITE_TRANSIENT);
                while(sqlite3_step(d.stmt_maa_shape) == SQLITE_ROW)
                {
                    m.shape.push_back({col_double(d.stmt_maa_shape, 1),
                                       col_double(d.stmt_maa_shape, 0)});
                }
            }
            return m;
        });
    }

    const std::vector<class_airspace>& nasr_database::query_class_airspace(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.class_airspaces, d.stmt_cls_arsp,
            lon_min, lat_min, lon_max, lat_max, [&](sqlite3_stmt* s)
        {
            class_airspace a{sqlite3_column_int(s, 0),
                col_text(s, 1), col_text(s, 2), col_text(s, 3),
                col_text(s, 4), col_text(s, 5), col_text(s, 6), col_text(s, 7), {}};

            sqlite3_reset(d.stmt_cls_arsp_shape);
            sqlite3_bind_int(d.stmt_cls_arsp_shape, 1, a.arsp_id);
            int current_part = -1;
            while(sqlite3_step(d.stmt_cls_arsp_shape) == SQLITE_ROW)
            {
                int part_num = sqlite3_column_int(d.stmt_cls_arsp_shape, 0);
                if(part_num != current_part)
                {
                    polygon_ring ring;
                    ring.is_hole = sqlite3_column_int(d.stmt_cls_arsp_shape, 1) != 0;
                    a.parts.push_back(std::move(ring));
                    current_part = part_num;
                }
                a.parts.back().points.push_back(
                    {col_double(d.stmt_cls_arsp_shape, 3),
                     col_double(d.stmt_cls_arsp_shape, 2)});
            }
            return a;
        });
    }

    const std::vector<runway>& nasr_database::query_runways(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.runways, d.stmt_runways,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return runway{col_double(s, 0), col_double(s, 1),
                          col_double(s, 2), col_double(s, 3)};
        });
    }

    const std::vector<sua>& nasr_database::query_sua(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.suas, d.stmt_sua,
            lon_min, lat_min, lon_max, lat_max, [&](sqlite3_stmt* s)
        {
            sua su{sqlite3_column_int(s, 0), col_text(s, 1),
                   col_text(s, 2), col_text(s, 3), {}};

            sqlite3_reset(d.stmt_sua_shape);
            sqlite3_bind_int(d.stmt_sua_shape, 1, su.sua_id);
            int current_part = -1;
            while(sqlite3_step(d.stmt_sua_shape) == SQLITE_ROW)
            {
                int part_num = sqlite3_column_int(d.stmt_sua_shape, 0);
                if(part_num != current_part)
                {
                    sua_ring ring;
                    ring.upper_limit = col_text(d.stmt_sua_shape, 1);
                    ring.lower_limit = col_text(d.stmt_sua_shape, 2);
                    su.parts.push_back(std::move(ring));
                    current_part = part_num;
                }
                su.parts.back().points.push_back(
                    {col_double(d.stmt_sua_shape, 4),
                     col_double(d.stmt_sua_shape, 3)});
            }
            return su;
        });
    }

    const std::vector<obstacle>& nasr_database::query_obstacles(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.obstacles, d.stmt_obstacles,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return obstacle{col_text(s, 0), col_double(s, 1), col_double(s, 2),
                            sqlite3_column_int(s, 3), col_text(s, 4)};
        });
    }

    const std::vector<artcc>& nasr_database::query_artcc(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.artccs, d.stmt_artcc,
            lon_min, lat_min, lon_max, lat_max, [&](sqlite3_stmt* s)
        {
            artcc a{sqlite3_column_int(s, 0), col_text(s, 1),
                    col_text(s, 2), col_text(s, 3), {}};

            sqlite3_reset(d.stmt_artcc_shape);
            sqlite3_bind_int(d.stmt_artcc_shape, 1, a.artcc_id);
            while(sqlite3_step(d.stmt_artcc_shape) == SQLITE_ROW)
            {
                a.points.push_back({col_double(d.stmt_artcc_shape, 1),
                                    col_double(d.stmt_artcc_shape, 0)});
            }
            return a;
        });
    }

    const std::vector<pja>& nasr_database::query_pjas(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.pjas, d.stmt_pjas,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return pja{col_text(s, 0), col_text(s, 1),
                       col_double(s, 2), col_double(s, 3), col_double(s, 4)};
        });
    }

    const std::vector<adiz>& nasr_database::query_adiz(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.adizs, d.stmt_adiz,
            lon_min, lat_min, lon_max, lat_max, [&](sqlite3_stmt* s)
        {
            adiz a{sqlite3_column_int(s, 0), col_text(s, 1), {}};

            sqlite3_reset(d.stmt_adiz_shape);
            sqlite3_bind_int(d.stmt_adiz_shape, 1, a.adiz_id);
            int current_part = -1;
            while(sqlite3_step(d.stmt_adiz_shape) == SQLITE_ROW)
            {
                int part_num = sqlite3_column_int(d.stmt_adiz_shape, 0);
                if(part_num != current_part)
                {
                    a.parts.push_back({});
                    current_part = part_num;
                }
                a.parts.back().push_back(
                    {col_double(d.stmt_adiz_shape, 2),
                     col_double(d.stmt_adiz_shape, 1)});
            }
            return a;
        });
    }

    const std::vector<fss>& nasr_database::query_fss(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.fsss, d.stmt_fss,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return fss{col_text(s, 0), col_text(s, 1), col_text(s, 2),
                        col_text(s, 3), col_text(s, 4),
                        col_double(s, 5), col_double(s, 6),
                        col_text(s, 7), col_text(s, 8)};
        });
    }

    const std::vector<awos>& nasr_database::query_awos(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.awoss, d.stmt_awos,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return awos{col_text(s, 0), col_text(s, 1),
                         col_text(s, 2), col_text(s, 3),
                         col_double(s, 4), col_double(s, 5),
                         col_double(s, 6), col_text(s, 7), col_text(s, 8)};
        });
    }

    const std::vector<comm_outlet>& nasr_database::query_comm_outlets(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.comm_outlets, d.stmt_comm_outlets,
            lon_min, lat_min, lon_max, lat_max, [](sqlite3_stmt* s)
        {
            return comm_outlet{col_text(s, 0), col_text(s, 1),
                                col_text(s, 2), col_text(s, 3),
                                col_double(s, 4), col_double(s, 5)};
        });
    }

} // namespace nasrbrowse
