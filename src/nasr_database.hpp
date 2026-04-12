#pragma once
#include "geo_types.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nasrbrowse
{
    // Column order follows APT_BASE table (minus SITE_NO), with computed columns at end
    struct airport
    {
        std::string site_type_code;    // "A"=airport, "H"=heliport, "C"=seaplane, "U"=ultralight, "G"=glider, "B"=balloon
        std::string state_code;
        std::string arpt_id;
        std::string city;
        std::string country_code;
        std::string arpt_name;
        std::string ownership_type_code; // "PU"=public, "PR"=private, "MA"/"MR"/"MN"=military
        std::string facility_use_code; // "PU"=public use, "PR"=private use
        double lat;
        double lon;
        double elev;
        std::string mag_varn;
        std::string mag_hemis;
        std::string tpa;               // traffic pattern altitude (feet AGL)
        std::string resp_artcc_id;
        std::string fss_id;
        std::string notam_id;
        std::string activation_date;
        std::string arpt_status;       // "O"=operational, "CI"=closed indef, "CP"=closed perm
        std::string fuel_types;
        std::string lgt_sked;
        std::string bcn_lgt_sked;
        std::string twr_type_code;     // "ATCT*"=towered, "NON-ATCT"=untowered
        std::string icao_id;
        bool hard_surface;
        std::string airspace_class;    // "B", "C", "D", "E", or "" (computed from JOIN)
    };

    // Column order follows NAV_BASE table
    struct navaid
    {
        std::string nav_id;
        std::string nav_type;
        std::string state_code;
        std::string city;
        std::string country_code;
        std::string nav_status;
        std::string name;
        std::string oper_hours;
        std::string high_alt_artcc_id;
        std::string low_alt_artcc_id;
        double lat;
        double lon;
        double elev;
        std::string mag_varn;
        std::string mag_varn_hemis;
        std::string freq;
        std::string chan;
        std::string pwr_output;
        std::string simul_voice_flag;
        std::string voice_call;
        std::string restriction_flag;
    };

    // Column order follows FIX_BASE table
    struct fix
    {
        std::string fix_id;
        std::string state_code;
        std::string country_code;
        std::string icao_region_code;
        double lat;
        double lon;
        std::string use_code;
        std::string artcc_id_high;
        std::string artcc_id_low;
    };

    // Column order follows AWY_SEG table
    struct airway_segment
    {
        std::string awy_id;
        std::string awy_location;
        int point_seq;
        std::string from_point;
        std::string to_point;
        double from_lat;
        double from_lon;
        double to_lat;
        double to_lon;
        std::string gap_flag;
        std::string min_enroute_alt;
        std::string mag_course_dist;
    };

    // Column order follows MTR_SEG table
    struct mtr_segment
    {
        std::string mtr_id;
        std::string from_point;
        std::string to_point;
        double from_lat;
        double from_lon;
        double to_lat;
        double to_lon;
    };

    struct polygon_ring
    {
        std::vector<airspace_point> points;
        bool is_hole;
    };

    // Column order follows MAA_BASE table
    struct maa
    {
        std::string maa_id;
        std::string type;   // "AEROBATIC PRACTICE", "GLIDER", etc.
        std::string name;
        double lat;         // 0 if shape-defined
        double lon;
        double radius_nm;   // 0 if point-only or shape-defined
        std::string max_alt;
        std::string min_alt;
        std::vector<airspace_point> shape;  // empty if point/radius
    };

    // Column order follows RWY_SEG table
    struct runway
    {
        std::string site_no;
        std::string rwy_id;
        double end1_lat;
        double end1_lon;
        double end2_lat;
        double end2_lon;
    };

    // A polygon ring within a special use airspace, with its own altitude limits
    struct sua_ring
    {
        std::vector<airspace_point> points;
        std::string upper_limit;
        std::string lower_limit;
        bool is_circle = false;
        double circle_lon = 0;
        double circle_lat = 0;
        double circle_radius_nm = 0;
    };

    // Column order follows SUA_BASE table
    struct sua
    {
        int sua_id;
        std::string designator;
        std::string name;
        std::string sua_type;  // "MOA", "RA", "WA", "AA", "PA", "NSA"
        std::string upper_limit;
        std::string lower_limit;
        std::string controlling_authority;
        std::string admin_area;
        std::string city;
        std::string military;
        std::string activity;
        std::string status;
        std::string working_hours;
        std::string icao_compliant;
        std::string legal_note;
        std::vector<sua_ring> parts;
    };

    // Column order follows PJA_BASE table
    struct pja
    {
        std::string pja_id;
        std::string name;
        double lat;
        double lon;
        double radius_nm;
        std::string max_altitude;
    };

    // Column order follows ADIZ_BASE table
    struct adiz
    {
        int adiz_id;
        std::string name;
        std::string location;
        std::string working_hours;
        std::string military;
        std::vector<std::vector<airspace_point>> parts;
    };

    // Subdivided polyline segments for rendering (tight R-tree bbox)
    struct boundary_segment
    {
        std::string altitude;  // ARTCC only: "LOW", "HIGH", "UNLIMITED"
        std::vector<airspace_point> points;
    };

    struct airspace_segment
    {
        std::string airspace_class;  // "B", "C", "D", "E"
        std::string local_type;      // "CLASS_B", "CLASS_C", etc.
        std::vector<airspace_point> points;
    };

    struct sua_segment
    {
        std::string sua_type;  // "MOA", "RA", "WA", etc.
        std::vector<airspace_point> points;
    };

    // Render-only narrow row (render path doesn't need polygon rings or
    // the 12 unused text columns from SUA_BASE).
    struct sua_circle
    {
        std::string sua_type;
        double center_lon;
        double center_lat;
        double radius_nm;
    };

    // Column order follows OBS_BASE table
    struct obstacle
    {
        std::string oas_num;
        std::string verify_status;
        std::string country;
        std::string state;
        std::string city;
        double lat;
        double lon;
        std::string obstacle_type;
        int quantity;
        int agl_ht;
        int amsl_ht;
        std::string lighting;  // "R","D","H","M","S","F","C","W","L","N","U"
        std::string horiz_acc;
        std::string vert_acc;
        std::string marking;
        std::string faa_study;
        std::string action;
        std::string jdate;
    };

    // Column order follows FSS_BASE table
    struct fss
    {
        std::string fss_id;
        std::string name;
        std::string fss_fac_type;
        std::string voice_call;
        std::string city;
        std::string state_code;
        std::string country_code;
        double lat;
        double lon;
        std::string opr_hours;
        std::string fac_status;
        std::string phone_no;
        std::string toll_free_no;
    };

    // Column order follows AWOS table
    struct awos
    {
        std::string id;
        std::string type;       // AWOS-1, AWOS-2, AWOS-3, ASOS, etc.
        std::string state_code;
        std::string city;
        std::string country_code;
        std::string commissioned_date;
        std::string navaid_flag;
        double lat;
        double lon;
        double elev;
        std::string phone_no;
        std::string second_phone_no;
        std::string site_no;    // links to airport
        std::string site_type_code;
        std::string remark;
    };

    // Column order follows COM table
    struct comm_outlet
    {
        std::string comm_loc_id;
        std::string comm_type;      // RCO, RCAG, RCO1
        std::string nav_id;
        std::string nav_type;
        std::string city;
        std::string state_code;
        std::string country_code;
        std::string outlet_name;
        double lat;
        double lon;
        std::string facility_id;    // operating FSS or ARTCC
        std::string facility_name;
        std::string opr_hrs;
        std::string comm_status_code;
        std::string remark;
    };

    // Column order follows ARTCC_BASE table
    struct artcc
    {
        int artcc_id;
        std::string location_id;  // "ZLA", "ZNY", etc.
        std::string name;
        std::string altitude;     // "LOW", "HIGH", "UNLIMITED"
        std::vector<airspace_point> points;
    };

    // Column order follows CLS_ARSP_BASE table
    struct class_airspace
    {
        int arsp_id;
        std::string name;
        std::string airspace_class;  // "B", "C", "D", "E"
        std::string local_type;      // "CLASS_B", "CLASS_C", "CLASS_D", "CLASS_E2", etc.
        std::string ident;
        std::string sector;
        std::string upper_desc;      // "AA", "TI", "TNI", "ANI"
        std::string upper_val;
        std::string lower_desc;
        std::string lower_val;
        std::string wkhr_code;
        std::string wkhr_rmk;
        std::vector<polygon_ring> parts;
    };

    class nasr_database
    {
        struct impl;
        std::unique_ptr<impl> pimpl;

    public:
        nasr_database(const char* db_path);
        ~nasr_database();

        // Query features within a geographic bounding box (lon/lat degrees).
        // Results are valid until the next query call on the same type.
        // Optional filter: when provided, restricts rows to those whose
        // relevant attribute is in the given value list (nullopt = no filter).
        using filter_list = std::optional<std::vector<std::string>>;

        const std::vector<airport>& query_airports(const geo_bbox& bbox,
                                                    const filter_list& class_filter = std::nullopt);
        const std::vector<navaid>& query_navaids(const geo_bbox& bbox);
        const std::vector<fix>& query_fixes(const geo_bbox& bbox);
        const std::vector<airway_segment>& query_airways(const geo_bbox& bbox);
        const std::vector<mtr_segment>& query_mtrs(const geo_bbox& bbox);
        const std::vector<maa>& query_maas(const geo_bbox& bbox);
        const std::vector<class_airspace>& query_class_airspace(const geo_bbox& bbox);
        const std::vector<runway>& query_runways(const geo_bbox& bbox);
        const std::vector<sua>& query_sua(const geo_bbox& bbox,
                                           const filter_list& type_filter = std::nullopt);
        const std::vector<sua_circle>& query_sua_circles(const geo_bbox& bbox,
                                                          const filter_list& type_filter = std::nullopt);
        const std::vector<obstacle>& query_obstacles(const geo_bbox& bbox);
        const std::vector<artcc>& query_artcc(const geo_bbox& bbox);
        const std::vector<pja>& query_pjas(const geo_bbox& bbox);
        const std::vector<adiz>& query_adiz(const geo_bbox& bbox);
        const std::vector<fss>& query_fss(const geo_bbox& bbox);
        const std::vector<awos>& query_awos(const geo_bbox& bbox);
        const std::vector<comm_outlet>& query_comm_outlets(const geo_bbox& bbox);

        // Subdivided segment queries for rendering (tight R-tree bboxes)
        const std::vector<boundary_segment>& query_artcc_segments(const geo_bbox& bbox);
        const std::vector<boundary_segment>& query_adiz_segments(const geo_bbox& bbox);
        const std::vector<airspace_segment>& query_class_airspace_segments(
            const geo_bbox& bbox, const filter_list& class_filter = std::nullopt);
        const std::vector<sua_segment>& query_sua_segments(
            const geo_bbox& bbox, const filter_list& type_filter = std::nullopt);
    };

} // namespace nasrbrowse
