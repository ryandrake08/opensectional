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
        std::string site_type_code;
        std::string twr_type_code;
        std::string icao_id;
        double lat;
        double lon;
        double elev;
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

    // MOA/SUA airspace (from MAA tables)
    struct airspace
    {
        std::string maa_id;
        std::string maa_type;
        std::string maa_name;
        std::vector<airspace_point> shape;
    };

    // Class B/C/D/E airspace (from shapefile)
    struct class_airspace
    {
        int arsp_id;
        std::string name;
        std::string airspace_class;  // "B", "C", "D", "E"
        std::string local_type;      // "CLASS_B", "CLASS_C", "CLASS_D", "CLASS_E2", etc.
        std::string upper_val;
        std::string lower_val;
        // Each part is a ring (outer boundary or hole)
        std::vector<std::vector<airspace_point>> parts;
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
    };

} // namespace nasrbrowse
