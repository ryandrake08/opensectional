#include "nasr_database.hpp"
#include <memory>
#include <mutex>
#include <sqlite/database.hpp>
#include <sqlite/statement.hpp>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace nasrbrowse
{
    // Prepare a statement and validate its column count at startup
    static sqlite::statement prepare_checked(sqlite::database& db, const char* sql,
                                              int expected_columns)
    {
        auto stmt = db.prepare(sql);
        auto actual = stmt.column_count();
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
        sqlite::statement stmt_airway_by_id;
        sqlite::statement stmt_adjacent_airways;
        sqlite::statement stmt_mtrs;
        sqlite::statement stmt_mtr_by_id;
        sqlite::statement stmt_maas;
        sqlite::statement stmt_maa_shape;
        sqlite::statement stmt_cls_arsp;
        sqlite::statement stmt_cls_arsp_shape;
        sqlite::statement stmt_runways;
        sqlite::statement stmt_sua;
        sqlite::statement stmt_sua_strata;
        sqlite::statement stmt_sua_shape;
        sqlite::statement stmt_sua_schedules;
        sqlite::statement stmt_sua_freqs;
        sqlite::statement stmt_sua_circle;
        sqlite::statement stmt_sua_circles_bbox;
        sqlite::statement stmt_obstacles;
        sqlite::statement stmt_artcc;
        sqlite::statement stmt_artcc_shape;
        sqlite::statement stmt_pjas;
        sqlite::statement stmt_adiz;
        sqlite::statement stmt_adiz_shape;
        sqlite::statement stmt_tfr;
        sqlite::statement stmt_tfr_areas;
        sqlite::statement stmt_tfr_shape;
        sqlite::statement stmt_fss;
        sqlite::statement stmt_awos;
        sqlite::statement stmt_comm_outlets;
        sqlite::statement stmt_artcc_seg;
        sqlite::statement stmt_adiz_seg;
        sqlite::statement stmt_tfr_seg;
        sqlite::statement stmt_cls_arsp_seg;
        sqlite::statement stmt_sua_seg;
        sqlite::statement stmt_search;
        sqlite::statement stmt_lookup_airport;
        sqlite::statement stmt_lookup_navaid;
        sqlite::statement stmt_lookup_fix;

        // Serializes access to `db` and the prepared statements above.
        // Each public query method acquires this lock for its duration.
        mutable std::mutex mutex;

        // ARSP_IDs of class airspaces that are "shadowed" by a
        // higher-priority class at the same airport (same non-empty IDENT
        // with a lexicographically smaller CLASS letter, B<C<D<E).
        // Computed once at construction; used to skip segments in
        // query_class_airspace_segments() without a per-row SQL subquery.
        std::unordered_set<int> shadowed_arsp_ids;

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
                       VOICE_CALL, RESTRICTION_FLAG,
                       IS_LOW_NAV, IS_HIGH_NAV
                FROM NAV_BASE
                WHERE NAV_STATUS != 'SHUTDOWN'
                AND rowid IN (
                    SELECT id FROM NAV_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 23))

            , stmt_fixes(prepare_checked(db, R"(
                SELECT FIX_ID, STATE_CODE, COUNTRY_CODE, ICAO_REGION_CODE,
                       LAT_DECIMAL, LONG_DECIMAL, FIX_USE_CODE,
                       ARTCC_ID_HIGH, ARTCC_ID_LOW,
                       IS_LOW_FIX, IS_HIGH_FIX
                FROM FIX_BASE
                WHERE FIX_USE_CODE IN ('WP', 'RP', 'VFR', 'CN', 'MR', 'MW', 'NRS')
                AND rowid IN (
                    SELECT id FROM FIX_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 11))

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

            , stmt_airway_by_id(prepare_checked(db, R"(
                SELECT AWY_ID, AWY_LOCATION, POINT_SEQ, FROM_POINT, TO_POINT,
                       FROM_LAT, FROM_LON, TO_LAT, TO_LON,
                       AWY_SEG_GAP_FLAG, MIN_ENROUTE_ALT, MAG_COURSE_DIST
                FROM AWY_SEG
                WHERE AWY_SEG_GAP_FLAG != 'Y' AND AWY_ID = ?1
            )", 12))

            , stmt_adjacent_airways(prepare_checked(db, R"(
                SELECT DISTINCT AWY_ID FROM AWY_SEG
                WHERE AWY_SEG_GAP_FLAG != 'Y'
                  AND ((FROM_POINT = ?1 AND TO_POINT = ?2)
                    OR (FROM_POINT = ?2 AND TO_POINT = ?1))
            )", 1))

            , stmt_mtrs(prepare_checked(db, R"(
                SELECT MTR_ID, ROUTE_TYPE_CODE, FROM_POINT, TO_POINT,
                       FROM_LAT, FROM_LON, TO_LAT, TO_LON
                FROM MTR_SEG
                WHERE rowid IN (
                    SELECT id FROM MTR_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 8))

            , stmt_mtr_by_id(prepare_checked(db, R"(
                SELECT MTR_ID, ROUTE_TYPE_CODE, FROM_POINT, TO_POINT,
                       FROM_LAT, FROM_LON, TO_LAT, TO_LON
                FROM MTR_SEG
                WHERE MTR_ID = ?1
            )", 8))

            , stmt_maas(prepare_checked(db, R"(
                SELECT MAA_ID, TYPE, NAME, LAT, LON, RADIUS_NM,
                       MAX_ALT_FT, MAX_ALT_REF, MIN_ALT_FT, MIN_ALT_REF
                FROM MAA_BASE
                WHERE rowid IN (
                    SELECT id FROM MAA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 10))

            , stmt_maa_shape(prepare_checked(db, R"(
                SELECT LON_DECIMAL, LAT_DECIMAL
                FROM MAA_SHP
                WHERE MAA_ID = ?1
                ORDER BY POINT_SEQ
            )", 2))

            , stmt_cls_arsp(prepare_checked(db, R"(
                SELECT ARSP_ID, NAME, CLASS, LOCAL_TYPE,
                       IDENT, SECTOR,
                       UPPER_FT, UPPER_REF, LOWER_FT, LOWER_REF,
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
                       MIN_ALT_LIMIT, MAX_ALT_LIMIT,
                       CONDITIONAL_EXCLUSION, TRAFFIC_ALLOWED,
                       TIME_IN_ADVANCE_HR,
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
            )", 20))

            , stmt_sua_strata(prepare_checked(db, R"(
                SELECT STRATUM_ID, STRATUM_ORDER,
                       UPPER_LIMIT, LOWER_LIMIT,
                       UPPER_FT_VAL, UPPER_FT_REF,
                       LOWER_FT_VAL, LOWER_FT_REF
                FROM SUA_STRATUM
                WHERE SUA_ID = ?1
                ORDER BY STRATUM_ORDER
            )", 8))

            , stmt_sua_shape(prepare_checked(db, R"(
                SELECT PART_NUM, IS_HOLE, LON_DECIMAL, LAT_DECIMAL
                FROM SUA_SHP
                WHERE STRATUM_ID = ?1
                ORDER BY PART_NUM, POINT_SEQ
            )", 4))

            , stmt_sua_schedules(prepare_checked(db, R"(
                SELECT DAY_OF_WEEK, DAY_TIL,
                       START_TIME, END_TIME,
                       START_EVENT, END_EVENT,
                       START_EVENT_OFFSET, END_EVENT_OFFSET,
                       TIME_REF, TIME_OFFSET, DST_FLAG
                FROM SUA_SCHEDULE
                WHERE SUA_ID = ?1
                ORDER BY SCHED_SEQ
            )", 11))

            , stmt_sua_freqs(prepare_checked(db, R"(
                SELECT MODE, TX_FREQ, RX_FREQ,
                       COMM_ALLOWED, CHARTED, SECTORS
                FROM SUA_FREQ
                WHERE SUA_ID = ?1
                ORDER BY FREQ_SEQ
            )", 6))

            , stmt_sua_circle(prepare_checked(db, R"(
                SELECT PART_NUM, CENTER_LON, CENTER_LAT, RADIUS_NM
                FROM SUA_CIRCLE
                WHERE STRATUM_ID = ?1
            )", 4))

            , stmt_sua_circles_bbox(prepare_checked(db, R"(
                SELECT b.SUA_TYPE, c.CENTER_LON, c.CENTER_LAT, c.RADIUS_NM,
                       s.UPPER_FT_VAL, s.UPPER_FT_REF,
                       s.LOWER_FT_VAL, s.LOWER_FT_REF
                FROM SUA_CIRCLE c
                JOIN SUA_STRATUM s ON s.STRATUM_ID = c.STRATUM_ID
                JOIN SUA_BASE b ON b.SUA_ID = c.SUA_ID
                WHERE c.SUA_ID IN (
                    SELECT id FROM SUA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                AND (?5 IS NULL OR b.SUA_TYPE IN (SELECT value FROM json_each(?5)))
            )", 8))

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
                SELECT ARTCC_ID, LOCATION_ID, LOCATION_NAME, ALTITUDE, TYPE,
                       ICAO_ID, LOCATION_TYPE, CITY, STATE, COUNTRY_CODE, CROSS_REF
                FROM ARTCC_BASE
                WHERE ARTCC_ID IN (
                    SELECT id FROM ARTCC_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 11))

            , stmt_artcc_shape(prepare_checked(db, R"(
                SELECT LON_DECIMAL, LAT_DECIMAL
                FROM ARTCC_SHP
                WHERE ARTCC_ID = ?1
                ORDER BY POINT_SEQ
            )", 2))

            , stmt_pjas(prepare_checked(db, R"(
                SELECT PJA_ID, NAME, LAT, LON, RADIUS_NM, MAX_ALTITUDE,
                       MAX_ALT_FT_MSL
                FROM PJA_BASE
                WHERE rowid IN (
                    SELECT id FROM PJA_BASE_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
            )", 7))

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

            , stmt_tfr(prepare_checked(db, R"(
                SELECT TFR_ID, NOTAM_ID, TFR_TYPE, FACILITY,
                       DATE_EFFECTIVE, DATE_EXPIRE, DESCRIPTION
                FROM TFR_BASE
                WHERE TFR_ID IN (
                    SELECT DISTINCT TFR_ID FROM TFR_AREA
                    WHERE AREA_ID IN (
                        SELECT id FROM TFR_AREA_RTREE
                        WHERE max_lon >= ?1 AND min_lon <= ?3
                          AND max_lat >= ?2 AND min_lat <= ?4
                    )
                )
            )", 7))

            , stmt_tfr_areas(prepare_checked(db, R"(
                SELECT AREA_ID, AREA_NAME,
                       UPPER_FT_VAL, UPPER_FT_REF,
                       LOWER_FT_VAL, LOWER_FT_REF,
                       DATE_EFFECTIVE, DATE_EXPIRE
                FROM TFR_AREA
                WHERE TFR_ID = ?1
            )", 8))

            , stmt_tfr_shape(prepare_checked(db, R"(
                SELECT LON_DECIMAL, LAT_DECIMAL
                FROM TFR_SHP
                WHERE AREA_ID = ?1
                ORDER BY POINT_SEQ
            )", 2))

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
                SELECT SEG_ID, ALTITUDE, TYPE, LON_DECIMAL, LAT_DECIMAL
                FROM ARTCC_SEG
                WHERE SEG_ID IN (
                    SELECT id FROM ARTCC_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                ORDER BY SEG_ID, POINT_SEQ
            )", 5))

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

            , stmt_tfr_seg(prepare_checked(db, R"(
                SELECT SEG_ID, UPPER_FT_VAL, UPPER_FT_REF,
                       LOWER_FT_VAL, LOWER_FT_REF,
                       LON_DECIMAL, LAT_DECIMAL
                FROM TFR_SEG
                WHERE SEG_ID IN (
                    SELECT id FROM TFR_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                ORDER BY SEG_ID, POINT_SEQ
            )", 7))

            // Shadowed airspaces (ARSP_IDs with a higher-priority class at
            // the same IDENT) are filtered in query_class_airspace_segments
            // using a precomputed set — doing it per-row in SQL was O(N²)
            // at continental zoom.
            , stmt_cls_arsp_seg(prepare_checked(db, R"(
                SELECT s.SEG_ID, s.ARSP_ID, s.CLASS, s.LOCAL_TYPE,
                       s.LON_DECIMAL, s.LAT_DECIMAL
                FROM CLS_ARSP_SEG s
                WHERE s.SEG_ID IN (
                    SELECT id FROM CLS_ARSP_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                AND (?5 IS NULL
                     OR s.CLASS IN (SELECT value FROM json_each(?5))
                     OR s.LOCAL_TYPE IN (SELECT value FROM json_each(?5)))
                ORDER BY s.SEG_ID, s.POINT_SEQ
            )", 6))

            , stmt_sua_seg(prepare_checked(db, R"(
                SELECT SEG_ID, SUA_TYPE, LON_DECIMAL, LAT_DECIMAL,
                       UPPER_FT_VAL, UPPER_FT_REF,
                       LOWER_FT_VAL, LOWER_FT_REF
                FROM SUA_SEG
                WHERE SEG_ID IN (
                    SELECT id FROM SUA_SEG_RTREE
                    WHERE max_lon >= ?1 AND min_lon <= ?3
                      AND max_lat >= ?2 AND min_lat <= ?4
                )
                AND (?5 IS NULL OR SUA_TYPE IN (SELECT value FROM json_each(?5)))
                ORDER BY SEG_ID, POINT_SEQ
            )", 8))

            , stmt_search(prepare_checked(db, R"(
                SELECT entity_type, entity_rowid, ids, name,
                       bm25(search_fts, 0, 0, 10.0, 3.0, 1.0) AS rank
                FROM search_fts
                WHERE search_fts MATCH ?1
                ORDER BY rank
                LIMIT ?2
            )", 5))

            , stmt_lookup_airport(prepare_checked(db, R"(
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
                WHERE a.ARPT_ID = ?1 OR a.ICAO_ID = ?1
            )", 26))

            , stmt_lookup_navaid(prepare_checked(db, R"(
                SELECT NAV_ID, NAV_TYPE, STATE_CODE, CITY, COUNTRY_CODE,
                       NAV_STATUS, NAME, OPER_HOURS,
                       HIGH_ALT_ARTCC_ID, LOW_ALT_ARTCC_ID,
                       LAT_DECIMAL, LONG_DECIMAL, ELEV,
                       MAG_VARN, MAG_VARN_HEMIS, FREQ,
                       CHAN, PWR_OUTPUT, SIMUL_VOICE_FLAG,
                       VOICE_CALL, RESTRICTION_FLAG,
                       IS_LOW_NAV, IS_HIGH_NAV
                FROM NAV_BASE
                WHERE NAV_STATUS != 'SHUTDOWN' AND NAV_ID = ?1
            )", 23))

            , stmt_lookup_fix(prepare_checked(db, R"(
                SELECT FIX_ID, STATE_CODE, COUNTRY_CODE, ICAO_REGION_CODE,
                       LAT_DECIMAL, LONG_DECIMAL, FIX_USE_CODE,
                       ARTCC_ID_HIGH, ARTCC_ID_LOW,
                       IS_LOW_FIX, IS_HIGH_FIX
                FROM FIX_BASE
                WHERE FIX_ID = ?1
            )", 11))
        {
            // Precompute the set of shadowed ARSP_IDs. Any class airspace
            // that has a lower-class neighbor at the same non-empty IDENT
            // is suppressed at render time (B<C<D<E by CLASS letter).
            sqlite::statement stmt_shadows(prepare_checked(db, R"(
                SELECT me.ARSP_ID
                FROM CLS_ARSP_BASE me
                WHERE me.IDENT != ''
                  AND EXISTS (
                    SELECT 1 FROM CLS_ARSP_BASE other
                    WHERE other.IDENT = me.IDENT
                      AND other.CLASS < me.CLASS
                  )
            )", 1));
            while(stmt_shadows.step())
                shadowed_arsp_ids.insert(stmt_shadows.column_int(0));
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
            auto first = true;
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

        template<typename RowMapper>
        auto query_bbox(sqlite::statement& stmt,
                         const geo_bbox& bbox,
                         const RowMapper& map_row)
        {
            using T = std::invoke_result_t<RowMapper&, sqlite::statement&>;
            std::vector<T> results;
            bind_bbox(stmt, bbox);
            while (stmt.step())
            {
                results.push_back(map_row(stmt));
            }
            return results;
        }

        template<typename RowMapper>
        auto query_bbox_filtered(
            sqlite::statement& stmt,
            const geo_bbox& bbox,
            const std::optional<std::vector<std::string>>& filter,
            const RowMapper& map_row)
        {
            using T = std::invoke_result_t<RowMapper&, sqlite::statement&>;
            std::vector<T> results;
            bind_bbox(stmt, bbox);
            bind_filter(stmt, 5, filter);
            while (stmt.step())
            {
                results.push_back(map_row(stmt));
            }
            return results;
        }
    };

    nasr_database::nasr_database(const char* db_path) : pimpl(std::make_unique<impl>(db_path))
    {
    }

    nasr_database::~nasr_database() = default;

    std::vector<airport> nasr_database::query_airports(const geo_bbox& bbox,
                                                        const filter_list& class_filter) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox_filtered(d.stmt_airports,
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

    std::vector<navaid> nasr_database::query_navaids(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_navaids,
            bbox, [](sqlite::statement& s)
        {
            return navaid{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5), s.column_text(6), s.column_text(7),
                s.column_text(8), s.column_text(9),
                s.column_double(10), s.column_double(11), s.column_double(12),
                s.column_text(13), s.column_text(14), s.column_text(15),
                s.column_text(16), s.column_text(17), s.column_text(18),
                s.column_text(19), s.column_text(20),
                s.column_int(21) != 0, s.column_int(22) != 0};
        });
    }

    std::vector<fix> nasr_database::query_fixes(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_fixes,
            bbox, [](sqlite::statement& s)
        {
            return fix{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_double(4), s.column_double(5),
                s.column_text(6), s.column_text(7), s.column_text(8),
                s.column_int(9) != 0, s.column_int(10) != 0};
        });
    }

    std::vector<airway_segment> nasr_database::query_airways(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_airways,
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

    std::vector<mtr_segment> nasr_database::query_mtrs(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_mtrs,
            bbox, [](sqlite::statement& s)
        {
            return mtr_segment{
                s.column_text(0), s.column_text(1),
                s.column_text(2), s.column_text(3),
                s.column_double(4), s.column_double(5),
                s.column_double(6), s.column_double(7)};
        });
    }

    std::vector<airway_segment> nasr_database::query_airway_by_id(const std::string& awy_id) const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& s = pimpl->stmt_airway_by_id;
        s.reset();
        s.bind(1, awy_id);
        std::vector<airway_segment> out;
        while(s.step())
        {
            out.push_back(airway_segment{
                s.column_text(0), s.column_text(1), s.column_int(2),
                s.column_text(3), s.column_text(4),
                s.column_double(5), s.column_double(6),
                s.column_double(7), s.column_double(8),
                s.column_text(9), s.column_text(10), s.column_text(11)});
        }
        return out;
    }

    std::vector<std::string> nasr_database::adjacent_airways(
        const std::string& a, const std::string& b) const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& s = pimpl->stmt_adjacent_airways;
        s.reset();
        s.bind(1, a);
        s.bind(2, b);
        std::vector<std::string> out;
        while(s.step())
            out.push_back(s.column_text(0));
        return out;
    }

    std::vector<mtr_segment> nasr_database::query_mtr_by_id(const std::string& mtr_id) const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& s = pimpl->stmt_mtr_by_id;
        s.reset();
        s.bind(1, mtr_id);
        std::vector<mtr_segment> out;
        while(s.step())
        {
            out.push_back(mtr_segment{
                s.column_text(0), s.column_text(1),
                s.column_text(2), s.column_text(3),
                s.column_double(4), s.column_double(5),
                s.column_double(6), s.column_double(7)});
        }
        return out;
    }

    std::vector<maa> nasr_database::query_maas(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_maas,
            bbox, [&](sqlite::statement& s)
        {
            maa m{s.column_text(0), s.column_text(1), s.column_text(2),
                  s.column_double(3), s.column_double(4), s.column_double(5),
                  s.column_int(6), s.column_text(7),
                  s.column_int(8), s.column_text(9), {}};

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

    std::vector<class_airspace> nasr_database::query_class_airspace(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_cls_arsp,
            bbox, [&](sqlite::statement& s)
        {
            class_airspace a{s.column_int(0),
                s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5),
                s.column_int(6), s.column_text(7), s.column_int(8), s.column_text(9),
                s.column_text(10), s.column_text(11), {}};

            d.stmt_cls_arsp_shape.reset();
            d.stmt_cls_arsp_shape.bind(1, a.arsp_id);
            auto current_part = -1;
            while (d.stmt_cls_arsp_shape.step())
            {
                auto part_num = d.stmt_cls_arsp_shape.column_int(0);
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

    std::vector<runway> nasr_database::query_runways(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_runways,
            bbox, [](sqlite::statement& s)
        {
            return runway{s.column_text(0), s.column_text(1),
                          s.column_double(2), s.column_double(3),
                          s.column_double(4), s.column_double(5)};
        });
    }

    std::vector<sua> nasr_database::query_sua(const geo_bbox& bbox,
                                                const filter_list& type_filter) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox_filtered(d.stmt_sua,
            bbox, type_filter, [&](sqlite::statement& s)
        {
            sua su{s.column_int(0), s.column_text(1), s.column_text(2), s.column_text(3),
                   s.column_text(4), s.column_text(5),
                   s.column_text(6), s.column_text(7),
                   s.column_text(8), s.column_text(9),
                   s.column_text(10),
                   s.column_text(11), s.column_text(12), s.column_text(13),
                   s.column_text(14), s.column_text(15), s.column_text(16),
                   s.column_text(17), s.column_text(18), s.column_text(19),
                   {}, {}, {}};

            // Load each stratum with its parts. Strata are ordered by
            // STRATUM_ORDER (BASE first, then UNION bands, then partial-
            // cover SUBTR strata).
            d.stmt_sua_strata.reset();
            d.stmt_sua_strata.bind(1, su.sua_id);
            while (d.stmt_sua_strata.step())
            {
                sua_stratum st;
                st.stratum_id = d.stmt_sua_strata.column_int(0);
                st.stratum_order = d.stmt_sua_strata.column_int(1);
                st.upper_limit = d.stmt_sua_strata.column_text(2);
                st.lower_limit = d.stmt_sua_strata.column_text(3);
                st.upper_ft_val = d.stmt_sua_strata.column_int(4);
                st.upper_ft_ref = d.stmt_sua_strata.column_text(5);
                st.lower_ft_val = d.stmt_sua_strata.column_int(6);
                st.lower_ft_ref = d.stmt_sua_strata.column_text(7);

                // Circle metadata, if this stratum is a pure circle.
                auto circle_part = -1;
                auto c_lon = 0.0;
                auto c_lat = 0.0;
                auto c_rad = 0.0;
                d.stmt_sua_circle.reset();
                d.stmt_sua_circle.bind(1, st.stratum_id);
                if (d.stmt_sua_circle.step())
                {
                    circle_part = d.stmt_sua_circle.column_int(0);
                    c_lon = d.stmt_sua_circle.column_double(1);
                    c_lat = d.stmt_sua_circle.column_double(2);
                    c_rad = d.stmt_sua_circle.column_double(3);
                }

                d.stmt_sua_shape.reset();
                d.stmt_sua_shape.bind(1, st.stratum_id);
                auto current_part = -1;
                while (d.stmt_sua_shape.step())
                {
                    auto part_num = d.stmt_sua_shape.column_int(0);
                    if (part_num != current_part)
                    {
                        sua_ring ring;
                        ring.is_hole = d.stmt_sua_shape.column_int(1) != 0;
                        if (part_num == circle_part)
                        {
                            ring.is_circle = true;
                            ring.circle_lon = c_lon;
                            ring.circle_lat = c_lat;
                            ring.circle_radius_nm = c_rad;
                        }
                        st.parts.push_back(std::move(ring));
                        current_part = part_num;
                    }
                    st.parts.back().points.push_back(
                        {d.stmt_sua_shape.column_double(3),
                         d.stmt_sua_shape.column_double(2)});
                }
                su.strata.push_back(std::move(st));
            }

            // Schedules: zero-or-more Timesheet entries.
            d.stmt_sua_schedules.reset();
            d.stmt_sua_schedules.bind(1, su.sua_id);
            while (d.stmt_sua_schedules.step())
            {
                sua_schedule sc;
                sc.day                = d.stmt_sua_schedules.column_text(0);
                sc.day_til            = d.stmt_sua_schedules.column_text(1);
                sc.start_time         = d.stmt_sua_schedules.column_text(2);
                sc.end_time           = d.stmt_sua_schedules.column_text(3);
                sc.start_event        = d.stmt_sua_schedules.column_text(4);
                sc.end_event          = d.stmt_sua_schedules.column_text(5);
                sc.start_event_offset = d.stmt_sua_schedules.column_text(6);
                sc.end_event_offset   = d.stmt_sua_schedules.column_text(7);
                sc.time_ref           = d.stmt_sua_schedules.column_text(8);
                sc.time_offset        = d.stmt_sua_schedules.column_text(9);
                sc.dst_flag           = d.stmt_sua_schedules.column_text(10);
                su.schedules.push_back(std::move(sc));
            }

            // Frequencies allocated to this SUA.
            d.stmt_sua_freqs.reset();
            d.stmt_sua_freqs.bind(1, su.sua_id);
            while (d.stmt_sua_freqs.step())
            {
                sua_freq f;
                f.mode         = d.stmt_sua_freqs.column_text(0);
                f.tx           = d.stmt_sua_freqs.column_text(1);
                f.rx           = d.stmt_sua_freqs.column_text(2);
                f.comm_allowed = d.stmt_sua_freqs.column_text(3);
                f.charted      = d.stmt_sua_freqs.column_text(4);
                f.sectors      = d.stmt_sua_freqs.column_text(5);
                su.freqs.push_back(std::move(f));
            }
            return su;
        });
    }

    std::vector<sua_circle> nasr_database::query_sua_circles(
        const geo_bbox& bbox, const filter_list& type_filter) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox_filtered(d.stmt_sua_circles_bbox,
            bbox, type_filter, [](sqlite::statement& s)
        {
            return sua_circle{s.column_text(0),
                              s.column_double(1),
                              s.column_double(2),
                              s.column_double(3),
                              s.column_int(4),
                              s.column_text(5),
                              s.column_int(6),
                              s.column_text(7)};
        });
    }

    std::vector<obstacle> nasr_database::query_obstacles(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_obstacles,
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

    std::vector<artcc> nasr_database::query_artcc(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_artcc,
            bbox, [&](sqlite::statement& s)
        {
            artcc a{s.column_int(0),    s.column_text(1),
                    s.column_text(2),   s.column_text(3),
                    s.column_text(4),   s.column_text(5),
                    s.column_text(6),   s.column_text(7),
                    s.column_text(8),   s.column_text(9),
                    s.column_text(10),  {}};

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

    std::vector<pja> nasr_database::query_pjas(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_pjas,
            bbox, [](sqlite::statement& s)
        {
            return pja{s.column_text(0), s.column_text(1),
                       s.column_double(2), s.column_double(3), s.column_double(4),
                       s.column_text(5), s.column_int(6)};
        });
    }

    std::vector<adiz> nasr_database::query_adiz(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_adiz,
            bbox, [&](sqlite::statement& s)
        {
            adiz a{s.column_int(0), s.column_text(1),
                   s.column_text(2), s.column_text(3), s.column_text(4), {}};

            d.stmt_adiz_shape.reset();
            d.stmt_adiz_shape.bind(1, a.adiz_id);
            auto current_part = -1;
            while (d.stmt_adiz_shape.step())
            {
                auto part_num = d.stmt_adiz_shape.column_int(0);
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

    std::vector<tfr> nasr_database::query_tfr(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_tfr,
            bbox, [&](sqlite::statement& s)
        {
            tfr t{s.column_int(0), s.column_text(1), s.column_text(2),
                  s.column_text(3), s.column_text(4), s.column_text(5),
                  s.column_text(6), {}};

            d.stmt_tfr_areas.reset();
            d.stmt_tfr_areas.bind(1, t.tfr_id);
            while (d.stmt_tfr_areas.step())
            {
                tfr_area area{
                    d.stmt_tfr_areas.column_int(0),
                    d.stmt_tfr_areas.column_text(1),
                    d.stmt_tfr_areas.column_int(2),
                    d.stmt_tfr_areas.column_text(3),
                    d.stmt_tfr_areas.column_int(4),
                    d.stmt_tfr_areas.column_text(5),
                    d.stmt_tfr_areas.column_text(6),
                    d.stmt_tfr_areas.column_text(7),
                    {}};

                d.stmt_tfr_shape.reset();
                d.stmt_tfr_shape.bind(1, area.area_id);
                while (d.stmt_tfr_shape.step())
                {
                    area.points.push_back(
                        {d.stmt_tfr_shape.column_double(1),
                         d.stmt_tfr_shape.column_double(0)});
                }
                t.areas.push_back(std::move(area));
            }
            return t;
        });
    }

    std::vector<fss> nasr_database::query_fss(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_fss,
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

    std::vector<awos> nasr_database::query_awos(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_awos,
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

    std::vector<comm_outlet> nasr_database::query_comm_outlets(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        return d.query_bbox(d.stmt_comm_outlets,
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

    std::vector<boundary_segment> nasr_database::query_artcc_segments(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        std::vector<boundary_segment> results;
        d.bind_bbox(d.stmt_artcc_seg, bbox);
        auto current_seg = -1;
        while (d.stmt_artcc_seg.step())
        {
            auto seg_id = d.stmt_artcc_seg.column_int(0);
            if (seg_id != current_seg)
            {
                results.push_back({d.stmt_artcc_seg.column_text(1),
                                   d.stmt_artcc_seg.column_text(2), {}});
                current_seg = seg_id;
            }
            results.back().points.push_back(
                {d.stmt_artcc_seg.column_double(4),
                 d.stmt_artcc_seg.column_double(3)});
        }
        return results;
    }

    std::vector<boundary_segment> nasr_database::query_adiz_segments(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        std::vector<boundary_segment> results;
        d.bind_bbox(d.stmt_adiz_seg, bbox);
        auto current_seg = -1;
        while (d.stmt_adiz_seg.step())
        {
            auto seg_id = d.stmt_adiz_seg.column_int(0);
            if (seg_id != current_seg)
            {
                results.push_back({{}, {}, {}});
                current_seg = seg_id;
            }
            results.back().points.push_back(
                {d.stmt_adiz_seg.column_double(2),
                 d.stmt_adiz_seg.column_double(1)});
        }
        return results;
    }

    std::vector<tfr_segment> nasr_database::query_tfr_segments(const geo_bbox& bbox) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        std::vector<tfr_segment> results;
        d.bind_bbox(d.stmt_tfr_seg, bbox);
        auto current_seg = -1;
        while (d.stmt_tfr_seg.step())
        {
            auto seg_id = d.stmt_tfr_seg.column_int(0);
            if (seg_id != current_seg)
            {
                results.push_back({d.stmt_tfr_seg.column_int(1),
                                   d.stmt_tfr_seg.column_text(2),
                                   d.stmt_tfr_seg.column_int(3),
                                   d.stmt_tfr_seg.column_text(4),
                                   {}});
                current_seg = seg_id;
            }
            results.back().points.push_back(
                {d.stmt_tfr_seg.column_double(6),
                 d.stmt_tfr_seg.column_double(5)});
        }
        return results;
    }

    std::vector<airspace_segment> nasr_database::query_class_airspace_segments(
        const geo_bbox& bbox, const filter_list& class_filter) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        std::vector<airspace_segment> results;
        d.bind_bbox(d.stmt_cls_arsp_seg, bbox);
        d.bind_filter(d.stmt_cls_arsp_seg, 5, class_filter);
        auto current_seg = -1;
        auto skip_current = false;
        while (d.stmt_cls_arsp_seg.step())
        {
            auto seg_id = d.stmt_cls_arsp_seg.column_int(0);
            if (seg_id != current_seg)
            {
                auto arsp_id = d.stmt_cls_arsp_seg.column_int(1);
                skip_current = d.shadowed_arsp_ids.count(arsp_id) != 0;
                current_seg = seg_id;
                if (skip_current) continue;
                results.push_back(
                    {d.stmt_cls_arsp_seg.column_text(2),
                     d.stmt_cls_arsp_seg.column_text(3), {}});
            }
            if (skip_current) continue;
            results.back().points.push_back(
                {d.stmt_cls_arsp_seg.column_double(5),
                 d.stmt_cls_arsp_seg.column_double(4)});
        }
        return results;
    }

    std::vector<sua_segment> nasr_database::query_sua_segments(
        const geo_bbox& bbox, const filter_list& type_filter) const
    {
        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        std::vector<sua_segment> results;
        d.bind_bbox(d.stmt_sua_seg, bbox);
        d.bind_filter(d.stmt_sua_seg, 5, type_filter);
        auto current_seg = -1;
        while (d.stmt_sua_seg.step())
        {
            auto seg_id = d.stmt_sua_seg.column_int(0);
            if (seg_id != current_seg)
            {
                results.push_back({d.stmt_sua_seg.column_text(1),
                                   d.stmt_sua_seg.column_int(4),
                                   d.stmt_sua_seg.column_text(5),
                                   d.stmt_sua_seg.column_int(6),
                                   d.stmt_sua_seg.column_text(7),
                                   {}});
                current_seg = seg_id;
            }
            results.back().points.push_back(
                {d.stmt_sua_seg.column_double(3),
                 d.stmt_sua_seg.column_double(2)});
        }
        return results;
    }

    std::vector<search_hit> nasr_database::search(const std::string& query, int limit) const
    {
        // Build an FTS5 MATCH expression from the user query. Split on
        // whitespace; for each token keep only alphanumerics (FTS5 rejects
        // unescaped hyphens, apostrophes, etc.), then append '*' for prefix
        // matching. Tokens are space-separated (implicit AND).
        std::string expr;
        std::string tok;
        auto flush = [&]()
        {
            if(tok.empty()) return;
            if(!expr.empty()) expr.push_back(' ');
            expr.append(tok);
            expr.push_back('*');
            tok.clear();
        };
        for(char c : query)
        {
            if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9'))
                tok.push_back(c);
            else
                flush();
        }
        flush();

        std::vector<search_hit> hits;
        if(expr.empty()) return hits;

        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        d.stmt_search.reset();
        d.stmt_search.bind(1, expr);
        d.stmt_search.bind(2, limit);
        while(d.stmt_search.step())
        {
            hits.push_back(search_hit{
                d.stmt_search.column_text(0),
                d.stmt_search.column_int(1),
                d.stmt_search.column_text(2),
                d.stmt_search.column_text(3)});
        }
        return hits;
    }

    std::optional<geo_bbox> nasr_database::get_hit_bbox(const search_hit& hit) const
    {
        const auto& entity_type = hit.entity_type;
        auto entity_rowid = hit.entity_rowid;
        // Per-type SQL. Point entities return a degenerate bbox built from
        // the source table's lat/lon columns; areal entities read their
        // BASE_RTREE; line entities (AWY/MTR) aggregate over their segment
        // tables. Prepared on demand — search navigation happens at click
        // rate, so compile cost is negligible.
        const char* sql = nullptr;
        if(entity_type == "APT")
            sql = "SELECT LONG_DECIMAL, LONG_DECIMAL, LAT_DECIMAL, LAT_DECIMAL FROM APT_BASE WHERE rowid = ?1";
        else if(entity_type == "NAV")
            sql = "SELECT LONG_DECIMAL, LONG_DECIMAL, LAT_DECIMAL, LAT_DECIMAL FROM NAV_BASE WHERE rowid = ?1";
        else if(entity_type == "FIX")
            sql = "SELECT LONG_DECIMAL, LONG_DECIMAL, LAT_DECIMAL, LAT_DECIMAL FROM FIX_BASE WHERE rowid = ?1";
        else if(entity_type == "FSS")
            sql = "SELECT LONG_DECIMAL, LONG_DECIMAL, LAT_DECIMAL, LAT_DECIMAL FROM FSS_BASE WHERE rowid = ?1";
        else if(entity_type == "AWOS")
            sql = "SELECT LONG_DECIMAL, LONG_DECIMAL, LAT_DECIMAL, LAT_DECIMAL FROM AWOS WHERE rowid = ?1";
        else if(entity_type == "COM")
            sql = "SELECT LONG_DECIMAL, LONG_DECIMAL, LAT_DECIMAL, LAT_DECIMAL FROM COM WHERE rowid = ?1";
        else if(entity_type == "PJA")
            sql = "SELECT LON, LON, LAT, LAT FROM PJA_BASE WHERE rowid = ?1";
        else if(entity_type == "SUA")
            sql = "SELECT min_lon, max_lon, min_lat, max_lat FROM SUA_BASE_RTREE WHERE id = ?1";
        else if(entity_type == "CLS")
            sql = "SELECT min_lon, max_lon, min_lat, max_lat FROM CLS_ARSP_BASE_RTREE WHERE id = ?1";
        else if(entity_type == "ARTCC")
            sql = "SELECT min_lon, max_lon, min_lat, max_lat FROM ARTCC_BASE_RTREE WHERE id = ?1";
        else if(entity_type == "ADIZ")
            sql = "SELECT min_lon, max_lon, min_lat, max_lat FROM ADIZ_BASE_RTREE WHERE id = ?1";
        else if(entity_type == "AWY")
            sql = "SELECT MIN(FROM_LON), MAX(FROM_LON), MIN(FROM_LAT), MAX(FROM_LAT) "
                  "FROM AWY_SEG WHERE AWY_ID = (SELECT AWY_ID FROM AWY_BASE WHERE rowid = ?1)";
        else if(entity_type == "MTR")
            sql = "SELECT MIN(FROM_LON), MAX(FROM_LON), MIN(FROM_LAT), MAX(FROM_LAT) "
                  "FROM MTR_SEG WHERE MTR_ID = (SELECT ROUTE_ID FROM MTR_BASE WHERE rowid = ?1)";

        if(!sql) return std::nullopt;

        auto& d = *pimpl;
        std::lock_guard<std::mutex> lock(d.mutex);
        auto stmt = d.db.prepare(sql);
        stmt.bind(1, entity_rowid);
        if(!stmt.step()) return std::nullopt;

        geo_bbox bb{
            stmt.column_double(0),
            stmt.column_double(2),
            stmt.column_double(1),
            stmt.column_double(3)
        };
        if(bb.lon_min == 0 && bb.lon_max == 0 && bb.lat_min == 0 && bb.lat_max == 0)
            return std::nullopt;  // null/missing coords
        return bb;
    }

    std::vector<airport> nasr_database::lookup_airports(const std::string& id) const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& s = pimpl->stmt_lookup_airport;
        s.reset();
        s.bind(1, id);
        std::vector<airport> out;
        while(s.step())
        {
            out.push_back(airport{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5), s.column_text(6), s.column_text(7),
                s.column_double(8), s.column_double(9), s.column_double(10),
                s.column_text(11), s.column_text(12), s.column_text(13),
                s.column_text(14), s.column_text(15), s.column_text(16),
                s.column_text(17), s.column_text(18), s.column_text(19),
                s.column_text(20), s.column_text(21),
                s.column_text(22), s.column_text(23),
                s.column_int(24) != 0,
                s.column_text(25)});
        }
        return out;
    }

    std::vector<navaid> nasr_database::lookup_navaids(const std::string& id) const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& s = pimpl->stmt_lookup_navaid;
        s.reset();
        s.bind(1, id);
        std::vector<navaid> out;
        while(s.step())
        {
            out.push_back(navaid{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_text(4), s.column_text(5), s.column_text(6), s.column_text(7),
                s.column_text(8), s.column_text(9),
                s.column_double(10), s.column_double(11), s.column_double(12),
                s.column_text(13), s.column_text(14), s.column_text(15),
                s.column_text(16), s.column_text(17), s.column_text(18),
                s.column_text(19), s.column_text(20),
                s.column_int(21) != 0, s.column_int(22) != 0});
        }
        return out;
    }

    std::vector<fix> nasr_database::lookup_fixes(const std::string& id) const
    {
        std::lock_guard<std::mutex> lock(pimpl->mutex);
        auto& s = pimpl->stmt_lookup_fix;
        s.reset();
        s.bind(1, id);
        std::vector<fix> out;
        while(s.step())
        {
            out.push_back(fix{
                s.column_text(0), s.column_text(1), s.column_text(2), s.column_text(3),
                s.column_double(4), s.column_double(5),
                s.column_text(6), s.column_text(7), s.column_text(8),
                s.column_int(9) != 0, s.column_int(10) != 0});
        }
        return out;
    }

} // namespace nasrbrowse
