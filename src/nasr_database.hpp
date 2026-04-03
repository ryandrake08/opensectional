#pragma once
#include <memory>
#include <string>
#include <vector>

namespace nasrbrowse
{
    struct airport
    {
        std::string arpt_id;
        std::string arpt_name;
        std::string site_type_code;    // "A"=airport, "H"=heliport, "C"=seaplane, "U"=ultralight, "G"=glider, "B"=balloon
        std::string twr_type_code;     // "ATCT*"=towered, "NON-ATCT"=untowered
        std::string ownership_type_code; // "PU"=public, "PR"=private, "MA"/"MR"/"MN"=military
        std::string facility_use_code; // "PU"=public use, "PR"=private use
        std::string arpt_status;       // "O"=operational, "CI"=closed indef, "CP"=closed perm
        std::string icao_id;
        bool hard_surface;
        double lat;
        double lon;
        double elev;
        std::string airspace_class;    // "B", "C", "D", "E", or "" (none)
        std::string mag_varn;
        std::string mag_hemis;
        std::string tpa;               // traffic pattern altitude (feet AGL)
        std::string resp_artcc_id;
        std::string fss_id;
        std::string notam_id;
        std::string activation_date;
        std::string fuel_types;
        std::string lgt_sked;
        std::string bcn_lgt_sked;
    };

    struct navaid
    {
        std::string nav_id;
        std::string nav_type;
        std::string name;
        std::string freq;
        double lat;
        double lon;
    };

    struct fix
    {
        std::string fix_id;
        std::string use_code;
        double lat;
        double lon;
    };

    struct airway_segment
    {
        std::string awy_id;
        std::string from_point;
        std::string to_point;
        double from_lat;
        double from_lon;
        double to_lat;
        double to_lon;
    };

    struct airspace_point
    {
        double lat;
        double lon;
    };

    struct polygon_ring
    {
        std::vector<airspace_point> points;
        bool is_hole;
    };

    // Miscellaneous activity area (aerobatic, glider, hang glider, etc.)
    struct maa
    {
        std::string maa_id;
        std::string type;   // "AEROBATIC PRACTICE", "GLIDER", etc.
        std::string name;
        double lat;         // 0 if shape-defined
        double lon;
        double radius_nm;   // 0 if point-only or shape-defined
        std::vector<airspace_point> shape;  // empty if point/radius
    };

    struct runway
    {
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

    // Special use airspace (MOA, Restricted, Warning, Alert, etc. from AIXM)
    // Overall altitude limits are on parts[0]; SUBTR zones may differ.
    struct sua
    {
        int sua_id;
        std::string designator;
        std::string name;
        std::string sua_type;  // "MOA", "RA", "WA", "AA", "PA", "NSA"
        std::vector<sua_ring> parts;
    };

    struct pja
    {
        std::string pja_id;
        std::string name;
        double lat;
        double lon;
        double radius_nm;
    };

    struct adiz
    {
        int adiz_id;
        std::string name;
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

    struct obstacle
    {
        std::string oas_num;
        double lat;
        double lon;
        int agl_ht;
        std::string lighting;  // "R","D","H","M","S","F","C","W","L","N","U"
    };

    struct fss
    {
        std::string fss_id;
        std::string name;
        std::string voice_call;
        std::string city;
        std::string state_code;
        double lat;
        double lon;
        std::string opr_hours;
        std::string fac_status;
    };

    struct awos
    {
        std::string id;
        std::string type;       // AWOS-1, AWOS-2, AWOS-3, ASOS, etc.
        std::string city;
        std::string state_code;
        double lat;
        double lon;
        double elev;
        std::string phone_no;
        std::string site_no;    // links to airport
    };

    struct comm_outlet
    {
        std::string comm_type;      // RCO, RCAG, RCO1
        std::string outlet_name;
        std::string facility_id;    // operating FSS or ARTCC
        std::string facility_name;
        double lat;
        double lon;
    };

    // ARTCC (Air Route Traffic Control Center) boundary
    struct artcc
    {
        int artcc_id;
        std::string location_id;  // "ZLA", "ZNY", etc.
        std::string name;
        std::string altitude;     // "LOW", "HIGH", "UNLIMITED"
        std::vector<airspace_point> points;
    };

    // Class B/C/D/E airspace (from shapefile)
    struct class_airspace
    {
        int arsp_id;
        std::string name;
        std::string airspace_class;  // "B", "C", "D", "E"
        std::string local_type;      // "CLASS_B", "CLASS_C", "CLASS_D", "CLASS_E2", etc.
        std::string upper_desc;      // "AA", "TI", "TNI", "ANI"
        std::string upper_val;
        std::string lower_desc;
        std::string lower_val;
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
        const std::vector<airport>& query_airports(double lon_min, double lat_min,
                                                   double lon_max, double lat_max);
        const std::vector<navaid>& query_navaids(double lon_min, double lat_min,
                                                 double lon_max, double lat_max);
        const std::vector<fix>& query_fixes(double lon_min, double lat_min,
                                            double lon_max, double lat_max);
        const std::vector<airway_segment>& query_airways(double lon_min, double lat_min,
                                                         double lon_max, double lat_max);
        const std::vector<airway_segment>& query_mtrs(double lon_min, double lat_min,
                                                       double lon_max, double lat_max);
        const std::vector<maa>& query_maas(double lon_min, double lat_min,
                                            double lon_max, double lat_max);
        const std::vector<class_airspace>& query_class_airspace(double lon_min, double lat_min,
                                                                double lon_max, double lat_max);
        const std::vector<runway>& query_runways(double lon_min, double lat_min,
                                                  double lon_max, double lat_max);
        const std::vector<sua>& query_sua(double lon_min, double lat_min,
                                          double lon_max, double lat_max);
        const std::vector<obstacle>& query_obstacles(double lon_min, double lat_min,
                                                      double lon_max, double lat_max);
        const std::vector<artcc>& query_artcc(double lon_min, double lat_min,
                                               double lon_max, double lat_max);
        const std::vector<pja>& query_pjas(double lon_min, double lat_min,
                                            double lon_max, double lat_max);
        const std::vector<adiz>& query_adiz(double lon_min, double lat_min,
                                             double lon_max, double lat_max);
        const std::vector<fss>& query_fss(double lon_min, double lat_min,
                                           double lon_max, double lat_max);
        const std::vector<awos>& query_awos(double lon_min, double lat_min,
                                             double lon_max, double lat_max);
        const std::vector<comm_outlet>& query_comm_outlets(double lon_min, double lat_min,
                                                            double lon_max, double lat_max);

        // Subdivided segment queries for rendering (tight R-tree bboxes)
        const std::vector<boundary_segment>& query_artcc_segments(
            double lon_min, double lat_min, double lon_max, double lat_max);
        const std::vector<boundary_segment>& query_adiz_segments(
            double lon_min, double lat_min, double lon_max, double lat_max);
        const std::vector<airspace_segment>& query_class_airspace_segments(
            double lon_min, double lat_min, double lon_max, double lat_max);
        const std::vector<sua_segment>& query_sua_segments(
            double lon_min, double lat_min, double lon_max, double lat_max);
    };

} // namespace nasrbrowse
