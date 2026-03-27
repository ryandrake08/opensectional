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
        std::string airspace_class;  // "B", "C", "D", "E", or "" (none)
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

    // MOA/SUA airspace (from MAA tables)
    struct airspace
    {
        std::string maa_id;
        std::string maa_type;
        std::string maa_name;
        std::string max_alt;
        std::string min_alt;
        std::vector<airspace_point> points;
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

    struct obstacle
    {
        double lat;
        double lon;
        int agl_ht;
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
        const std::vector<airspace>& query_airspaces(double lon_min, double lat_min,
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
    };

} // namespace nasrbrowse
