#include "nasr_database.hpp"
#include <sqlite/database.hpp>
#include <sqlite/statement.hpp>
#include <stdexcept>
#include <string>

namespace nasrbrowse
{
    // Prepare a statement and validate its column count at startup
    static sqlite::statement prepare_checked(sqlite::database& db, const char* sql,
                                              int expected_columns)
    {
        auto stmt = db.prepare(sql);
        int actual = stmt.column_count();
        if(actual != expected_columns)
        {
            throw std::runtime_error(
                "Column count mismatch: expected " + std::to_string(expected_columns) +
                " but got " + std::to_string(actual));
        }
        return stmt;
    }

    struct nasr_database::impl
    {
        sqlite::database db;

        sqlite::statement stmt_airports;
        sqlite::statement stmt_navaids;
        sqlite::statement stmt_fixes;
        sqlite::statement stmt_airways;
        sqlite::statement stmt_mtrs;
        sqlite::statement stmt_maas;
        sqlite::statement stmt_maa_shape;
        sqlite::statement stmt_cls_arsp;
        sqlite::statement stmt_cls_arsp_shape;
        sqlite::statement stmt_runways;
        sqlite::statement stmt_sua;
        sqlite::statement stmt_sua_shape;
        sqlite::statement stmt_sua_circle;
        sqlite::statement stmt_sua_circles_bbox;
        sqlite::statement stmt_obstacles;
        sqlite::statement stmt_artcc;
        sqlite::statement stmt_artcc_shape;
        sqlite::statement stmt_pjas;
        sqlite::statement stmt_adiz;
        sqlite::statement stmt_adiz_shape;
        sqlite::statement stmt_fss;
        sqlite::statement stmt_awos;
        sqlite::statement stmt_comm_outlets;
        sqlite::statement stmt_artcc_seg;
        sqlite::statement stmt_adiz_seg;
        sqlite::statement stmt_cls_arsp_seg;
        sqlite::statement stmt_sua_seg;

        std::vector<airport> airports;
        std::vector<navaid> navaids;
        std::vector<fix> fixes;
        std::vector<airway_segment> airways;
        std::vector<mtr_segment> mtrs;
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
        std::vector<boundary_segment> artcc_segs;
        std::vector<boundary_segment> adiz_segs;
        std::vector<airspace_segment> cls_arsp_segs;
        std::vector<sua_segment> sua_segs;
        std::vector<sua_circle> sua_circles;

        impl(const char* db_path)
            : db(db_path)

            , stmt_airports(prepare_checked(db, R"(
                SELECT * FROM (
                    SELECT a.SITE_TYPE_CODE, a.STATE_CODE, a.ARPT_ID, a.CITY,
                           a.COUNTRY_CODE, a.ARPT_NAME,
                           a.OWNERSHIP_TYPE_CODE, a.FACILITY_USE_CODE,
                           a.LAT_DECIMAL, a.LONG_DECIMAL, a.ELEV,
                           a.MAG_VARN, a.MAG_HEMIS, a.TPA,
                           a.RESP_ARTCC_ID, a.FSS_ID, a.NOTAM_ID,
                           a.ACTIVATION_DATE, a.ARPT_STATUS, a.FUEL_TYPES,
                           a.LGT_SKED, a.BCN_LGT_SKED,
                           a.TWR_TYPE_CODE, a.ICAO_ID, a.HARD_SURFACE,
                           CASE
                               WHEN c.CLASS_B_AIRSPACE = 'Y' THEN 'B'
                               WHEN c.CLASS_C_AIRSPACE = 'Y' THEN 'C'
                               WHEN c.CLASS_D_AIRSPACE = 'Y' THEN 'D'
                               WHEN c.CLASS_E_AIRSPACE = 'Y' THEN 'E'
                               ELSE ''
                           END AS airspace_class
                    FROM APT_BASE a
                    LEFT JOIN CLS_ARSP c ON c.SITE_NO = a.SITE_NO
                    WHERE a.rowid IN (
                        SELECT id FROM APT_BASE_RTREE
                        WHERE max_lon >= ?1 AND min_lon <= ?3
                          AND max_lat >= ?2 AND min_lat <= ?4
                    )
                )
                WHERE ?5 IS NULL OR airspace_class IN (SELECT value FROM json_each(?5))
            )", 26))

            , stmt_navaids(prepare_checked(db, R"(
                SELECT NAV_ID, NAV_TYPE, STATE_CODE, CITY, COUNTRY_CODE,
                       NAV_STATUS, NAME, OPER_HOURS,
                       HIGH_ALT_ARTCC_ID, LOW_ALT_ARTCC_ID,
                       LAT_DECIMAL, LONG_DECIMAL, ELEV,
                       MAG_VARN, MAG_VARN_HEMIS, FREQ,
                       CHAN, PWR_OUTPUT, SIMUL_VOICE_FLAG,
                       VOICE_CALL, RESTRICTION_FLAG
                FROM NAV_BASE
                WHERE NAV_STATUS != 'SHUTDOWN'
                AND rowid IN (
                    SELECT id FROM NAV_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 21))

            , stmt_fixes(prepare_checked(db, R"(
                SELECT FIX_ID, STATE_CODE, COUNTRY_CODE, ICAO_REGION_CODE,
                       LAT_DECIMAL, LONG_DECIMAL, FIX_USE_CODE,
                       ARTCC_ID_HIGH, ARTCC_ID_LOW
                FROM FIX_BASE
                WHERE FIX_USE_CODE IN ('WP', 'RP', 'VFR', 'CN', 'MR', 'MW', 'NRS')
                AND rowid IN (
                    SELECT id FROM FIX_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 9))

            , stmt_airways(prepare_checked(db, R"(
                SELECT AWY_ID, AWY_LOCATION, POINT_SEQ, FROM_POINT, TO_POINT,
                       FROM_LAT, FROM_LON, TO_LAT, TO_LON,
                       AWY_SEG_GAP_FLAG, MIN_ENROUTE_ALT, MAG_COURSE_DIST
                FROM AWY_SEG
                WHERE AWY_SEG_GAP_FLAG != 'Y'
                AND rowid IN (
                    SELECT id FROM AWY_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 12))

            , stmt_mtrs(prepare_checked(db, R"(
                SELECT MTR_ID, FROM_POINT, TO_POINT,
                       FROM_LAT, FROM_LON, TO_LAT, TO_LON
                FROM MTR_SEG
                WHERE rowid IN (
                    SELECT id FROM MTR_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 7))

            , stmt_maas(prepare_checked(db, R"(
                SELECT MAA_ID, TYPE, NAME, LAT, LON, RADIUS_NM,
                       MAX_ALT, MIN_ALT
                FROM MAA_BASE
                WHERE rowid IN (
                    SELECT id FROM MAA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 8))

            , stmt_maa_shape(prepare_checked(db, R"(
                SELECT LON_DECIMAL, LAT_DECIMAL
                FROM MAA_SHP
                WHERE MAA_ID = ?1
                ORDER BY POINT_SEQ
            )", 2))

            , stmt_cls_arsp(prepare_checked(db, R"(
                SELECT ARSP_ID, NAME, CLASS, LOCAL_TYPE,
                       IDENT, SECTOR,
                       UPPER_DESC, UPPER_VAL, LOWER_DESC, LOWER_VAL,
                       WKHR_CODE, WKHR_RMK
                FROM CLS_ARSP_BASE
                WHERE ARSP_ID IN (
                    SELECT id FROM CLS_ARSP_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 12))

            , stmt_cls_arsp_shape(prepare_checked(db, R"(
                SELECT PART_NUM, IS_HOLE, LON_DECIMAL, LAT_DECIMAL
                FROM CLS_ARSP_SHP
                WHERE ARSP_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )", 4))

            , stmt_runways(prepare_checked(db, R"(
                SELECT SITE_NO, RWY_ID,
                       END1_LAT, END1_LON, END2_LAT, END2_LON
                FROM RWY_SEG
                WHERE rowid IN (
                    SELECT id FROM RWY_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 6))

            , stmt_sua(prepare_checked(db, R"(
                SELECT SUA_ID, DESIGNATOR, NAME, SUA_TYPE,
                       UPPER_LIMIT, LOWER_LIMIT,
                       CONTROLLING_AUTHORITY, ADMIN_AREA, CITY,
                       MILITARY, ACTIVITY, STATUS,
                       WORKING_HOURS, ICAO_COMPLIANT, LEGAL_NOTE
                FROM SUA_BASE
                WHERE SUA_ID IN (
                    SELECT id FROM SUA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                AND (?5 IS NULL OR SUA_TYPE IN (SELECT value FROM json_each(?5)))
            )", 15))

            , stmt_sua_shape(prepare_checked(db, R"(
                SELECT PART_NUM, UPPER_LIMIT, LOWER_LIMIT, LON_DECIMAL, LAT_DECIMAL
                FROM SUA_SHP
                WHERE SUA_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )", 5))

            , stmt_sua_circle(prepare_checked(db, R"(
                SELECT PART_NUM, CENTER_LON, CENTER_LAT, RADIUS_NM
                FROM SUA_CIRCLE
                WHERE SUA_ID = ?1
            )", 4))

            , stmt_sua_circles_bbox(prepare_checked(db, R"(
                SELECT b.SUA_TYPE, c.CENTER_LON, c.CENTER_LAT, c.RADIUS_NM
                FROM SUA_CIRCLE c
                JOIN SUA_BASE b ON b.SUA_ID = c.SUA_ID
                WHERE c.SUA_ID IN (
                    SELECT id FROM SUA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                AND (?5 IS NULL OR b.SUA_TYPE IN (SELECT value FROM json_each(?5)))
            )", 4))

            , stmt_obstacles(prepare_checked(db, R"(
                SELECT OAS_NUM, VERIFY_STATUS, COUNTRY, STATE, CITY,
                       LAT_DECIMAL, LON_DECIMAL,
                       OBSTACLE_TYPE, QUANTITY, AGL_HT, AMSL_HT,
                       LIGHTING, HORIZ_ACC, VERT_ACC,
                       MARKING, FAA_STUDY, ACTION, JDATE
                FROM OBS_BASE
                WHERE rowid IN (
                    SELECT id FROM OBS_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 18))

            , stmt_artcc(prepare_checked(db, R"(
                SELECT ARTCC_ID, LOCATION_ID, LOCATION_NAME, ALTITUDE
                FROM ARTCC_BASE
                WHERE ARTCC_ID IN (
                    SELECT id FROM ARTCC_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 4))

            , stmt_artcc_shape(prepare_checked(db, R"(
                SELECT LON_DECIMAL, LAT_DECIMAL
                FROM ARTCC_SHP
                WHERE ARTCC_ID = ?1
                ORDER BY POINT_SEQ
            )", 2))

            , stmt_pjas(prepare_checked(db, R"(
                SELECT PJA_ID, NAME, LAT, LON, RADIUS_NM, MAX_ALTITUDE
                FROM PJA_BASE
                WHERE rowid IN (
                    SELECT id FROM PJA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 6))

            , stmt_adiz(prepare_checked(db, R"(
                SELECT ADIZ_ID, NAME, LOCATION, WORKING_HOURS, MILITARY
                FROM ADIZ_BASE
                WHERE ADIZ_ID IN (
                    SELECT id FROM ADIZ_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 5))

            , stmt_adiz_shape(prepare_checked(db, R"(
                SELECT PART_NUM, LON_DECIMAL, LAT_DECIMAL
                FROM ADIZ_SHP
                WHERE ADIZ_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )", 3))

            , stmt_fss(prepare_checked(db, R"(
                SELECT FSS_ID, NAME, FSS_FAC_TYPE, VOICE_CALL,
                       CITY, STATE_CODE, COUNTRY_CODE,
                       CAST(LAT_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
                       OPR_HOURS, FAC_STATUS, PHONE_NO, TOLL_FREE_NO
                FROM FSS_BASE
                WHERE rowid IN (
                    SELECT id FROM FSS_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 13))

            , stmt_awos(prepare_checked(db, R"(
                SELECT ASOS_AWOS_ID, ASOS_AWOS_TYPE, STATE_CODE, CITY,
                       COUNTRY_CODE, COMMISSIONED_DATE, NAVAID_FLAG,
                       CAST(LAT_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
                       CAST(ELEV AS REAL),
                       PHONE_NO, SECOND_PHONE_NO,
                       SITE_NO, SITE_TYPE_CODE, REMARK
                FROM AWOS
                WHERE rowid IN (
                    SELECT id FROM AWOS_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 15))

            , stmt_comm_outlets(prepare_checked(db, R"(
                SELECT COMM_LOC_ID, COMM_TYPE, NAV_ID, NAV_TYPE,
                       CITY, STATE_CODE, COUNTRY_CODE,
                       COMM_OUTLET_NAME,
                       CAST(LAT_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
                       FACILITY_ID, FACILITY_NAME,
                       OPR_HRS, COMM_STATUS_CODE, REMARK
                FROM COM
                WHERE COMM_TYPE IN ('RCO', 'RCO1', 'RCAG')
                  AND rowid IN (
                    SELECT id FROM COM_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 15))

            , stmt_artcc_seg(prepare_checked(db, R"(
                SELECT SEG_ID, ALTITUDE, LON_DECIMAL, LAT_DECIMAL
                FROM ARTCC_SEG
                WHERE SEG_ID IN (
                    SELECT id FROM ARTCC_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                ORDER BY SEG_ID, POINT_SEQ
            )", 4))

            , stmt_adiz_seg(prepare_checked(db, R"(
                SELECT SEG_ID, LON_DECIMAL, LAT_DECIMAL
                FROM ADIZ_SEG
                WHERE SEG_ID IN (
                    SELECT id FROM ADIZ_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                ORDER BY SEG_ID, POINT_SEQ
            )", 3))

            // Suppress segments of airspaces "shadowed" by a higher-priority
            // airspace at the same airport (same non-empty IDENT, lower CLASS
            // letter — B<C<D<E lexicographically matches the class hierarchy).
            // Example: KBFL Class E2 is hidden because KBFL also has a Class D.
            // Picking still returns the shadowed airspace from CLS_ARSP_BASE.
            , stmt_cls_arsp_seg(prepare_checked(db, R"(
                SELECT s.SEG_ID, s.CLASS, s.LOCAL_TYPE, s.LON_DECIMAL, s.LAT_DECIMAL
                FROM CLS_ARSP_SEG s
                WHERE s.SEG_ID IN (
                    SELECT id FROM CLS_ARSP_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                AND (?5 IS NULL
                     OR s.CLASS IN (SELECT value FROM json_each(?5))
                     OR s.LOCAL_TYPE IN (SELECT value FROM json_each(?5)))
                AND NOT EXISTS (
                    SELECT 1 FROM CLS_ARSP_BASE me
                    JOIN CLS_ARSP_BASE other ON me.IDENT = other.IDENT
                    WHERE me.ARSP_ID = s.ARSP_ID
                      AND me.IDENT != ''
                      AND other.CLASS < me.CLASS
                )
                ORDER BY s.SEG_ID, s.POINT_SEQ
            )", 5))

            , stmt_sua_seg(prepare_checked(db, R"(
                SELECT SEG_ID, SUA_TYPE, LON_DECIMAL, LAT_DECIMAL
                FROM SUA_SEG
                WHERE SEG_ID IN (
                    SELECT id FROM SUA_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                AND (?5 IS NULL OR SUA_TYPE IN (SELECT value FROM json_each(?5)))
                ORDER BY SEG_ID, POINT_SEQ
            )", 4))
        {
        }

        void bind_bbox(sqlite::statement& stmt, const geo_bbox& bbox)
        {
            stmt.reset();
            stmt.bind(1, bbox.lon_min);
            stmt.bind(2, bbox.lat_min);
            stmt.bind(3, bbox.lon_max);
            stmt.bind(4, bbox.lat_max);
        }

        // Binds a filter list at the given parameter index: NULL when the
        // filter is absent (SQL treats the filter clause as a no-op), or a
        // JSON array string consumed by json_each() otherwise.
        std::string filter_json;  // kept alive for the duration of step()
        void bind_filter(sqlite::statement& stmt, int index,
                         const std::optional<std::vector<std::string>>& filter)
        {
            if(!filter)
            {
                stmt.bind_null(index);
                return;
            }
            filter_json.clear();
            filter_json.push_back('[');
            bool first = true;
            for(const auto& v : *filter)
            {
                if(!first) filter_json.push_back(',');
                first = false;
                filter_json.push_back('"');
                filter_json.append(v);
                filter_json.push_back('"');
            }
            filter_json.push_back(']');
            stmt.bind(index, filter_json);
        }

        template<typename T, typename RowMapper>
        const std::vector<T>& query_bbox(std::vector<T>& results,
                                          sqlite::statement& stmt,
                                          const geo_bbox& bbox,
                                          RowMapper&& map_row)
        {
            results.clear();
            bind_bbox(stmt, bbox);
            while (stmt.step())
            {
                results.push_back(map_row(stmt));
            }
            return results;
        }

        template<typename T, typename RowMapper>
        const std::vector<T>& query_bbox_filtered(
            std::vector<T>& results,
            sqlite::statement& stmt,
            const geo_bbox& bbox,
            const std::optional<std::vector<std::string>>& filter,
            RowMapper&& map_row)
        {
            results.clear();
            bind_bbox(stmt, bbox);
            bind_filter(stmt, 5, filter);
            while (stmt.step())
            {
                results.push_back(map_row(stmt));
            }
            return results;
        }
    };

    nasr_database::nasr_database(const char* db_path) : pimpl(new impl(db_path))
    {
    }

    nasr_database::~nasr_database() = default;

    const std::vector<airport>& nasr_database::query_airports(const geo_bbox& bbox,
                                                                const filter_list& class_filter)
    {
        auto& d = *pimpl;
        return d.query_bbox_filtered(d.airports, d.stmt_airports,
            bbox, class_filter, [](sqlite::statement& s)
        {
            return airport{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5), s.column_text(6), s.column_text(7),
                s.column_double(8), s.column_double(9), s.column_double(10),
                s.column_text(11), s.column_text(12), s.column_text(13),
                s.column_text(14), s.column_text(15), s.column_text(16),
                s.column_text(17), s.column_text(18), s.column_text(19),
                s.column_text(20), s.column_text(21),
                s.column_text(22), s.column_text(23),
                s.column_int(24) != 0,
                s.column_text(25)};
        });
    }

    const std::vector<navaid>& nasr_database::query_navaids(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.navaids, d.stmt_navaids,
            bbox, [](sqlite::statement& s)
        {
            return navaid{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5), s.column_text(6), s.column_text(7),
                s.column_text(8), s.column_text(9),
                s.column_double(10), s.column_double(11), s.column_double(12),
                s.column_text(13), s.column_text(14), s.column_text(15),
                s.column_text(16), s.column_text(17), s.column_text(18),
                s.column_text(19), s.column_text(20)};
        });
    }

    const std::vector<fix>& nasr_database::query_fixes(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.fixes, d.stmt_fixes,
            bbox, [](sqlite::statement& s)
        {
            return fix{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_double(4), s.column_double(5),
                s.column_text(6), s.column_text(7), s.column_text(8)};
        });
    }

    const std::vector<airway_segment>& nasr_database::query_airways(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.airways, d.stmt_airways,
            bbox, [](sqlite::statement& s)
        {
            return airway_segment{
                s.column_text(0), s.column_text(1), s.column_int(2),
                s.column_text(3), s.column_text(4),
                s.column_double(5), s.column_double(6),
                s.column_double(7), s.column_double(8),
                s.column_text(9), s.column_text(10), s.column_text(11)};
        });
    }

    const std::vector<mtr_segment>& nasr_database::query_mtrs(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.mtrs, d.stmt_mtrs,
            bbox, [](sqlite::statement& s)
        {
            return mtr_segment{
                s.column_text(0), s.column_text(1), s.column_text(2),
                s.column_double(3), s.column_double(4),
                s.column_double(5), s.column_double(6)};
        });
    }

    const std::vector<maa>& nasr_database::query_maas(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.maas, d.stmt_maas,
            bbox, [&](sqlite::statement& s)
        {
            maa m{s.column_text(0), s.column_text(1), s.column_text(2),
                  s.column_double(3), s.column_double(4), s.column_double(5),
                  s.column_text(6), s.column_text(7), {}};

            // Load shape polygon for shape-defined entries
            if (m.lat == 0.0)
            {
                d.stmt_maa_shape.reset();
                d.stmt_maa_shape.bind(1, m.maa_id);
                while (d.stmt_maa_shape.step())
                {
                    m.shape.push_back({d.stmt_maa_shape.column_double(1),
                                       d.stmt_maa_shape.column_double(0)});
                }
            }
            return m;
        });
    }

    const std::vector<class_airspace>& nasr_database::query_class_airspace(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.class_airspaces, d.stmt_cls_arsp,
            bbox, [&](sqlite::statement& s)
        {
            class_airspace a{s.column_int(0),
                s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5),
                s.column_text(6), s.column_text(7), s.column_text(8), s.column_text(9),
                s.column_text(10), s.column_text(11), {}};

            d.stmt_cls_arsp_shape.reset();
            d.stmt_cls_arsp_shape.bind(1, a.arsp_id);
            int current_part = -1;
            while (d.stmt_cls_arsp_shape.step())
            {
                int part_num = d.stmt_cls_arsp_shape.column_int(0);
                if (part_num != current_part)
                {
                    polygon_ring ring;
                    ring.is_hole = d.stmt_cls_arsp_shape.column_int(1) != 0;
                    a.parts.push_back(std::move(ring));
                    current_part = part_num;
                }
                a.parts.back().points.push_back(
                    {d.stmt_cls_arsp_shape.column_double(3),
                     d.stmt_cls_arsp_shape.column_double(2)});
            }
            return a;
        });
    }

    const std::vector<runway>& nasr_database::query_runways(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.runways, d.stmt_runways,
            bbox, [](sqlite::statement& s)
        {
            return runway{s.column_text(0), s.column_text(1),
                          s.column_double(2), s.column_double(3),
                          s.column_double(4), s.column_double(5)};
        });
    }

    const std::vector<sua>& nasr_database::query_sua(const geo_bbox& bbox,
                                                      const filter_list& type_filter)
    {
        auto& d = *pimpl;
        return d.query_bbox_filtered(d.suas, d.stmt_sua,
            bbox, type_filter, [&](sqlite::statement& s)
        {
            sua su{s.column_int(0), s.column_text(1), s.column_text(2), s.column_text(3),
                   s.column_text(4), s.column_text(5),
                   s.column_text(6), s.column_text(7), s.column_text(8),
                   s.column_text(9), s.column_text(10), s.column_text(11),
                   s.column_text(12), s.column_text(13), s.column_text(14), {}};

            // Load circle metadata if this SUA is a pure circle
            int circle_part = -1;
            double circle_lon = 0, circle_lat = 0, circle_radius_nm = 0;
            d.stmt_sua_circle.reset();
            d.stmt_sua_circle.bind(1, su.sua_id);
            if (d.stmt_sua_circle.step())
            {
                circle_part = d.stmt_sua_circle.column_int(0);
                circle_lon = d.stmt_sua_circle.column_double(1);
                circle_lat = d.stmt_sua_circle.column_double(2);
                circle_radius_nm = d.stmt_sua_circle.column_double(3);
            }

            d.stmt_sua_shape.reset();
            d.stmt_sua_shape.bind(1, su.sua_id);
            int current_part = -1;
            while (d.stmt_sua_shape.step())
            {
                int part_num = d.stmt_sua_shape.column_int(0);
                if (part_num != current_part)
                {
                    sua_ring ring;
                    ring.upper_limit = d.stmt_sua_shape.column_text(1);
                    ring.lower_limit = d.stmt_sua_shape.column_text(2);
                    if (part_num == circle_part)
                    {
                        ring.is_circle = true;
                        ring.circle_lon = circle_lon;
                        ring.circle_lat = circle_lat;
                        ring.circle_radius_nm = circle_radius_nm;
                    }
                    su.parts.push_back(std::move(ring));
                    current_part = part_num;
                }
                su.parts.back().points.push_back(
                    {d.stmt_sua_shape.column_double(4),
                     d.stmt_sua_shape.column_double(3)});
            }
            return su;
        });
    }

    const std::vector<sua_circle>& nasr_database::query_sua_circles(
        const geo_bbox& bbox, const filter_list& type_filter)
    {
        auto& d = *pimpl;
        return d.query_bbox_filtered(d.sua_circles, d.stmt_sua_circles_bbox,
            bbox, type_filter, [](sqlite::statement& s)
        {
            return sua_circle{s.column_text(0),
                              s.column_double(1),
                              s.column_double(2),
                              s.column_double(3)};
        });
    }

    const std::vector<obstacle>& nasr_database::query_obstacles(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.obstacles, d.stmt_obstacles,
            bbox, [](sqlite::statement& s)
        {
            return obstacle{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4),
                s.column_double(5), s.column_double(6),
                s.column_text(7), s.column_int(8), s.column_int(9), s.column_int(10),
                s.column_text(11), s.column_text(12), s.column_text(13),
                s.column_text(14), s.column_text(15), s.column_text(16), s.column_text(17)};
        });
    }

    const std::vector<artcc>& nasr_database::query_artcc(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.artccs, d.stmt_artcc,
            bbox, [&](sqlite::statement& s)
        {
            artcc a{s.column_int(0), s.column_text(1),
                    s.column_text(2), s.column_text(3), {}};

            d.stmt_artcc_shape.reset();
            d.stmt_artcc_shape.bind(1, a.artcc_id);
            while (d.stmt_artcc_shape.step())
            {
                a.points.push_back({d.stmt_artcc_shape.column_double(1),
                                    d.stmt_artcc_shape.column_double(0)});
            }
            return a;
        });
    }

    const std::vector<pja>& nasr_database::query_pjas(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.pjas, d.stmt_pjas,
            bbox, [](sqlite::statement& s)
        {
            return pja{s.column_text(0), s.column_text(1),
                       s.column_double(2), s.column_double(3), s.column_double(4),
                       s.column_text(5)};
        });
    }

    const std::vector<adiz>& nasr_database::query_adiz(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.adizs, d.stmt_adiz,
            bbox, [&](sqlite::statement& s)
        {
            adiz a{s.column_int(0), s.column_text(1),
                   s.column_text(2), s.column_text(3), s.column_text(4), {}};

            d.stmt_adiz_shape.reset();
            d.stmt_adiz_shape.bind(1, a.adiz_id);
            int current_part = -1;
            while (d.stmt_adiz_shape.step())
            {
                int part_num = d.stmt_adiz_shape.column_int(0);
                if (part_num != current_part)
                {
                    a.parts.push_back({});
                    current_part = part_num;
                }
                a.parts.back().push_back(
                    {d.stmt_adiz_shape.column_double(2),
                     d.stmt_adiz_shape.column_double(1)});
            }
            return a;
        });
    }

    const std::vector<fss>& nasr_database::query_fss(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.fsss, d.stmt_fss,
            bbox, [](sqlite::statement& s)
        {
            return fss{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5), s.column_text(6),
                s.column_double(7), s.column_double(8),
                s.column_text(9), s.column_text(10),
                s.column_text(11), s.column_text(12)};
        });
    }

    const std::vector<awos>& nasr_database::query_awos(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.awoss, d.stmt_awos,
            bbox, [](sqlite::statement& s)
        {
            return awos{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5), s.column_text(6),
                s.column_double(7), s.column_double(8), s.column_double(9),
                s.column_text(10), s.column_text(11),
                s.column_text(12), s.column_text(13), s.column_text(14)};
        });
    }

    const std::vector<comm_outlet>& nasr_database::query_comm_outlets(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.comm_outlets, d.stmt_comm_outlets,
            bbox, [](sqlite::statement& s)
        {
            return comm_outlet{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5), s.column_text(6), s.column_text(7),
                s.column_double(8), s.column_double(9),
                s.column_text(10), s.column_text(11),
                s.column_text(12), s.column_text(13), s.column_text(14)};
        });
    }

    const std::vector<boundary_segment>& nasr_database::query_artcc_segments(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        d.artcc_segs.clear();
        d.bind_bbox(d.stmt_artcc_seg, bbox);
        int current_seg = -1;
        while (d.stmt_artcc_seg.step())
        {
            int seg_id = d.stmt_artcc_seg.column_int(0);
            if (seg_id != current_seg)
            {
                d.artcc_segs.push_back({d.stmt_artcc_seg.column_text(1), {}});
                current_seg = seg_id;
            }
            d.artcc_segs.back().points.push_back(
                {d.stmt_artcc_seg.column_double(3),
                 d.stmt_artcc_seg.column_double(2)});
        }
        return d.artcc_segs;
    }

    const std::vector<boundary_segment>& nasr_database::query_adiz_segments(const geo_bbox& bbox)
    {
        auto& d = *pimpl;
        d.adiz_segs.clear();
        d.bind_bbox(d.stmt_adiz_seg, bbox);
        int current_seg = -1;
        while (d.stmt_adiz_seg.step())
        {
            int seg_id = d.stmt_adiz_seg.column_int(0);
            if (seg_id != current_seg)
            {
                d.adiz_segs.push_back({{}, {}});
                current_seg = seg_id;
            }
            d.adiz_segs.back().points.push_back(
                {d.stmt_adiz_seg.column_double(2),
                 d.stmt_adiz_seg.column_double(1)});
        }
        return d.adiz_segs;
    }

    const std::vector<airspace_segment>& nasr_database::query_class_airspace_segments(
        const geo_bbox& bbox, const filter_list& class_filter)
    {
        auto& d = *pimpl;
        d.cls_arsp_segs.clear();
        d.bind_bbox(d.stmt_cls_arsp_seg, bbox);
        d.bind_filter(d.stmt_cls_arsp_seg, 5, class_filter);
        int current_seg = -1;
        while (d.stmt_cls_arsp_seg.step())
        {
            int seg_id = d.stmt_cls_arsp_seg.column_int(0);
            if (seg_id != current_seg)
            {
                d.cls_arsp_segs.push_back(
                    {d.stmt_cls_arsp_seg.column_text(1),
                     d.stmt_cls_arsp_seg.column_text(2), {}});
                current_seg = seg_id;
            }
            d.cls_arsp_segs.back().points.push_back(
                {d.stmt_cls_arsp_seg.column_double(4),
                 d.stmt_cls_arsp_seg.column_double(3)});
        }
        return d.cls_arsp_segs;
    }

    const std::vector<sua_segment>& nasr_database::query_sua_segments(
        const geo_bbox& bbox, const filter_list& type_filter)
    {
        auto& d = *pimpl;
        d.sua_segs.clear();
        d.bind_bbox(d.stmt_sua_seg, bbox);
        d.bind_filter(d.stmt_sua_seg, 5, type_filter);
        int current_seg = -1;
        while (d.stmt_sua_seg.step())
        {
            int seg_id = d.stmt_sua_seg.column_int(0);
            if (seg_id != current_seg)
            {
                d.sua_segs.push_back({d.stmt_sua_seg.column_text(1), {}});
                current_seg = seg_id;
            }
            d.sua_segs.back().points.push_back(
                {d.stmt_sua_seg.column_double(3),
                 d.stmt_sua_seg.column_double(2)});
        }
        return d.sua_segs;
    }

} // namespace nasrbrowse
