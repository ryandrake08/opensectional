#include "nasr_database.hpp"
#include <sqlite/database.hpp>
#include <sqlite/statement.hpp>
#include <string>

namespace nasrbrowse
{
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
        std::vector<boundary_segment> artcc_segs;
        std::vector<boundary_segment> adiz_segs;
        std::vector<airspace_segment> cls_arsp_segs;

        impl(const char* db_path)
            : db(db_path)

            , stmt_airports(db.prepare(R"(
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
            )"))

            , stmt_navaids(db.prepare(R"(
                SELECT NAV_ID, NAV_TYPE, NAME, FREQ, LAT_DECIMAL, LONG_DECIMAL
                FROM NAV_BASE
                WHERE NAV_STATUS != 'SHUTDOWN'
                AND rowid IN (
                    SELECT id FROM NAV_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_fixes(db.prepare(R"(
                SELECT FIX_ID, FIX_USE_CODE, LAT_DECIMAL, LONG_DECIMAL
                FROM FIX_BASE
                WHERE FIX_USE_CODE IN ('WP', 'RP', 'VFR', 'CN', 'MR', 'MW', 'NRS')
                AND rowid IN (
                    SELECT id FROM FIX_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_airways(db.prepare(R"(
                SELECT AWY_ID, FROM_POINT, TO_POINT,
                       FROM_LAT, FROM_LON, TO_LAT, TO_LON
                FROM AWY_SEG
                WHERE AWY_SEG_GAP_FLAG != 'Y'
                AND rowid IN (
                    SELECT id FROM AWY_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_mtrs(db.prepare(R"(
                SELECT MTR_ID, FROM_POINT, TO_POINT,
                       FROM_LAT, FROM_LON, TO_LAT, TO_LON
                FROM MTR_SEG
                WHERE rowid IN (
                    SELECT id FROM MTR_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_maas(db.prepare(R"(
                SELECT MAA_ID, TYPE, NAME, LAT, LON, RADIUS_NM
                FROM MAA_BASE
                WHERE rowid IN (
                    SELECT id FROM MAA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_maa_shape(db.prepare(R"(
                SELECT LON_DECIMAL, LAT_DECIMAL
                FROM MAA_SHP
                WHERE MAA_ID = ?1
                ORDER BY POINT_SEQ
            )"))

            , stmt_cls_arsp(db.prepare(R"(
                SELECT ARSP_ID, NAME, CLASS, LOCAL_TYPE,
                       UPPER_DESC, UPPER_VAL, LOWER_DESC, LOWER_VAL
                FROM CLS_ARSP_BASE
                WHERE ARSP_ID IN (
                    SELECT id FROM CLS_ARSP_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_cls_arsp_shape(db.prepare(R"(
                SELECT PART_NUM, IS_HOLE, LON_DECIMAL, LAT_DECIMAL
                FROM CLS_ARSP_SHP
                WHERE ARSP_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )"))

            , stmt_runways(db.prepare(R"(
                SELECT END1_LAT, END1_LON, END2_LAT, END2_LON
                FROM RWY_SEG
                WHERE rowid IN (
                    SELECT id FROM RWY_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_sua(db.prepare(R"(
                SELECT SUA_ID, DESIGNATOR, NAME, SUA_TYPE
                FROM SUA_BASE
                WHERE SUA_ID IN (
                    SELECT id FROM SUA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_sua_shape(db.prepare(R"(
                SELECT PART_NUM, UPPER_LIMIT, LOWER_LIMIT, LON_DECIMAL, LAT_DECIMAL
                FROM SUA_SHP
                WHERE SUA_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )"))

            , stmt_sua_circle(db.prepare(R"(
                SELECT PART_NUM, CENTER_LON, CENTER_LAT, RADIUS_NM
                FROM SUA_CIRCLE
                WHERE SUA_ID = ?1
            )"))

            , stmt_obstacles(db.prepare(R"(
                SELECT OAS_NUM, LAT_DECIMAL, LON_DECIMAL, AGL_HT, LIGHTING
                FROM OBS_BASE
                WHERE rowid IN (
                    SELECT id FROM OBS_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_artcc(db.prepare(R"(
                SELECT ARTCC_ID, LOCATION_ID, LOCATION_NAME, ALTITUDE
                FROM ARTCC_BASE
                WHERE ARTCC_ID IN (
                    SELECT id FROM ARTCC_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_artcc_shape(db.prepare(R"(
                SELECT LON_DECIMAL, LAT_DECIMAL
                FROM ARTCC_SHP
                WHERE ARTCC_ID = ?1
                ORDER BY POINT_SEQ
            )"))

            , stmt_pjas(db.prepare(R"(
                SELECT PJA_ID, NAME, LAT, LON, RADIUS_NM
                FROM PJA_BASE
                WHERE rowid IN (
                    SELECT id FROM PJA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_adiz(db.prepare(R"(
                SELECT ADIZ_ID, NAME
                FROM ADIZ_BASE
                WHERE ADIZ_ID IN (
                    SELECT id FROM ADIZ_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_adiz_shape(db.prepare(R"(
                SELECT PART_NUM, LON_DECIMAL, LAT_DECIMAL
                FROM ADIZ_SHP
                WHERE ADIZ_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )"))

            , stmt_fss(db.prepare(R"(
                SELECT FSS_ID, NAME, VOICE_CALL, CITY, STATE_CODE,
                       CAST(LAT_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
                       OPR_HOURS, FAC_STATUS
                FROM FSS_BASE
                WHERE rowid IN (
                    SELECT id FROM FSS_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_awos(db.prepare(R"(
                SELECT ASOS_AWOS_ID, ASOS_AWOS_TYPE, CITY, STATE_CODE,
                       CAST(LAT_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL),
                       CAST(ELEV AS REAL), PHONE_NO, SITE_NO
                FROM AWOS
                WHERE rowid IN (
                    SELECT id FROM AWOS_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_comm_outlets(db.prepare(R"(
                SELECT COMM_TYPE, COMM_OUTLET_NAME, FACILITY_ID, FACILITY_NAME,
                       CAST(LAT_DECIMAL AS REAL), CAST(LONG_DECIMAL AS REAL)
                FROM COM
                WHERE COMM_TYPE IN ('RCO', 'RCO1', 'RCAG')
                  AND rowid IN (
                    SELECT id FROM COM_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )"))

            , stmt_artcc_seg(db.prepare(R"(
                SELECT SEG_ID, ALTITUDE, LON_DECIMAL, LAT_DECIMAL
                FROM ARTCC_SEG
                WHERE SEG_ID IN (
                    SELECT id FROM ARTCC_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                ORDER BY SEG_ID, POINT_SEQ
            )"))

            , stmt_adiz_seg(db.prepare(R"(
                SELECT SEG_ID, LON_DECIMAL, LAT_DECIMAL
                FROM ADIZ_SEG
                WHERE SEG_ID IN (
                    SELECT id FROM ADIZ_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                ORDER BY SEG_ID, POINT_SEQ
            )"))

            , stmt_cls_arsp_seg(db.prepare(R"(
                SELECT SEG_ID, CLASS, LOCAL_TYPE, LON_DECIMAL, LAT_DECIMAL
                FROM CLS_ARSP_SEG
                WHERE SEG_ID IN (
                    SELECT id FROM CLS_ARSP_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                ORDER BY SEG_ID, POINT_SEQ
            )"))
        {
        }

        void bind_bbox(sqlite::statement& stmt, double lon_min, double lat_min,
                       double lon_max, double lat_max)
        {
            stmt.reset();
            stmt.bind(1, lon_min);
            stmt.bind(2, lat_min);
            stmt.bind(3, lon_max);
            stmt.bind(4, lat_max);
        }

        template<typename T, typename RowMapper>
        const std::vector<T>& query_bbox(std::vector<T>& results,
                                          sqlite::statement& stmt,
                                          double lon_min, double lat_min,
                                          double lon_max, double lat_max,
                                          RowMapper&& map_row)
        {
            results.clear();
            bind_bbox(stmt, lon_min, lat_min, lon_max, lat_max);
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

    const std::vector<airport>& nasr_database::query_airports(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.airports, d.stmt_airports,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return airport{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5), s.column_text(6), s.column_text(7),
                s.column_int(8) != 0,
                s.column_double(9), s.column_double(10), s.column_double(11),
                s.column_text(12),
                s.column_text(13), s.column_text(14), s.column_text(15),
                s.column_text(16), s.column_text(17), s.column_text(18),
                s.column_text(19), s.column_text(20),
                s.column_text(21), s.column_text(22)};
        });
    }

    const std::vector<navaid>& nasr_database::query_navaids(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.navaids, d.stmt_navaids,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return navaid{s.column_text(0), s.column_text(1), s.column_text(2),
                          s.column_text(3), s.column_double(4), s.column_double(5)};
        });
    }

    const std::vector<fix>& nasr_database::query_fixes(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.fixes, d.stmt_fixes,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return fix{s.column_text(0), s.column_text(1),
                       s.column_double(2), s.column_double(3)};
        });
    }

    const std::vector<airway_segment>& nasr_database::query_airways(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.airways, d.stmt_airways,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return airway_segment{
                s.column_text(0), s.column_text(1), s.column_text(2),
                s.column_double(3), s.column_double(4),
                s.column_double(5), s.column_double(6)};
        });
    }

    const std::vector<airway_segment>& nasr_database::query_mtrs(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.mtrs, d.stmt_mtrs,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return airway_segment{
                s.column_text(0), s.column_text(1), s.column_text(2),
                s.column_double(3), s.column_double(4),
                s.column_double(5), s.column_double(6)};
        });
    }

    const std::vector<maa>& nasr_database::query_maas(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.maas, d.stmt_maas,
            lon_min, lat_min, lon_max, lat_max, [&](sqlite::statement& s)
        {
            maa m{s.column_text(0), s.column_text(1), s.column_text(2),
                  s.column_double(3), s.column_double(4), s.column_double(5), {}};

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

    const std::vector<class_airspace>& nasr_database::query_class_airspace(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.class_airspaces, d.stmt_cls_arsp,
            lon_min, lat_min, lon_max, lat_max, [&](sqlite::statement& s)
        {
            class_airspace a{s.column_int(0),
                s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5), s.column_text(6), s.column_text(7), {}};

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

    const std::vector<runway>& nasr_database::query_runways(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.runways, d.stmt_runways,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return runway{s.column_double(0), s.column_double(1),
                          s.column_double(2), s.column_double(3)};
        });
    }

    const std::vector<sua>& nasr_database::query_sua(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.suas, d.stmt_sua,
            lon_min, lat_min, lon_max, lat_max, [&](sqlite::statement& s)
        {
            sua su{s.column_int(0), s.column_text(1),
                   s.column_text(2), s.column_text(3), {}};

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

    const std::vector<obstacle>& nasr_database::query_obstacles(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.obstacles, d.stmt_obstacles,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return obstacle{s.column_text(0), s.column_double(1), s.column_double(2),
                            s.column_int(3), s.column_text(4)};
        });
    }

    const std::vector<artcc>& nasr_database::query_artcc(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.artccs, d.stmt_artcc,
            lon_min, lat_min, lon_max, lat_max, [&](sqlite::statement& s)
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

    const std::vector<pja>& nasr_database::query_pjas(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.pjas, d.stmt_pjas,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return pja{s.column_text(0), s.column_text(1),
                       s.column_double(2), s.column_double(3), s.column_double(4)};
        });
    }

    const std::vector<adiz>& nasr_database::query_adiz(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.adizs, d.stmt_adiz,
            lon_min, lat_min, lon_max, lat_max, [&](sqlite::statement& s)
        {
            adiz a{s.column_int(0), s.column_text(1), {}};

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

    const std::vector<fss>& nasr_database::query_fss(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.fsss, d.stmt_fss,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return fss{s.column_text(0), s.column_text(1), s.column_text(2),
                        s.column_text(3), s.column_text(4),
                        s.column_double(5), s.column_double(6),
                        s.column_text(7), s.column_text(8)};
        });
    }

    const std::vector<awos>& nasr_database::query_awos(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.awoss, d.stmt_awos,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return awos{s.column_text(0), s.column_text(1),
                         s.column_text(2), s.column_text(3),
                         s.column_double(4), s.column_double(5),
                         s.column_double(6), s.column_text(7), s.column_text(8)};
        });
    }

    const std::vector<comm_outlet>& nasr_database::query_comm_outlets(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        return d.query_bbox(d.comm_outlets, d.stmt_comm_outlets,
            lon_min, lat_min, lon_max, lat_max, [](sqlite::statement& s)
        {
            return comm_outlet{s.column_text(0), s.column_text(1),
                                s.column_text(2), s.column_text(3),
                                s.column_double(4), s.column_double(5)};
        });
    }

    const std::vector<boundary_segment>& nasr_database::query_artcc_segments(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        d.artcc_segs.clear();
        d.bind_bbox(d.stmt_artcc_seg, lon_min, lat_min, lon_max, lat_max);
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

    const std::vector<boundary_segment>& nasr_database::query_adiz_segments(
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        d.adiz_segs.clear();
        d.bind_bbox(d.stmt_adiz_seg, lon_min, lat_min, lon_max, lat_max);
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
        double lon_min, double lat_min, double lon_max, double lat_max)
    {
        auto& d = *pimpl;
        d.cls_arsp_segs.clear();
        d.bind_bbox(d.stmt_cls_arsp_seg, lon_min, lat_min, lon_max, lat_max);
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

} // namespace nasrbrowse
