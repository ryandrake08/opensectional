#include "xnotam_parser.hpp"

#include <cstdlib>
#include <pugixml.hpp>
#include <string>

namespace osect
{
    namespace
    {
        // FAA's "unlimited altitude" sentinel — mirrors
        // ALT_UNLIMITED_FT in tools/build_common.py.
        constexpr int ALT_UNLIMITED_FT = 99999;

        // Parse XNOTAM coordinate strings like "25.94179199N",
        // "080.86666667W". Returns true on success and populates
        // *lon / *lat. Mirrors _parse_xnotam_coord in build_tfr.py.
        bool parse_coord(const std::string& lat_str,
                         const std::string& lon_str,
                         double* lon, double* lat)
        {
            if(lat_str.size() < 2 || lon_str.size() < 2) return false;
            const char lat_hem = lat_str.back();
            const char lon_hem = lon_str.back();
            char* end = nullptr;
            double v_lat = std::strtod(lat_str.c_str(), &end);
            if(end == lat_str.c_str()) return false;
            double v_lon = std::strtod(lon_str.c_str(), &end);
            if(end == lon_str.c_str()) return false;
            if(lat_hem == 'S') v_lat = -v_lat;
            else if(lat_hem != 'N') return false;
            if(lon_hem == 'W') v_lon = -v_lon;
            else if(lon_hem != 'E') return false;
            *lat = v_lat;
            *lon = v_lon;
            return true;
        }

        // Parse XNOTAM altitude triple (codeDistVer, valDistVer,
        // uomDistVer) into (value_ft, ref). Mirrors
        // _parse_xnotam_altitude in build_tfr.py.
        //
        //   code: "ALT" (MSL), "HEI" (AGL/SFC), "STD" (flight level)
        //   uom:  "FT" or "FL"
        void parse_altitude(const std::string& code,
                            const std::string& val_str,
                            const std::string& uom,
                            int* out_val, std::string* out_ref)
        {
            *out_val = 0;
            out_ref->clear();
            if(val_str.empty()) return;
            char* end = nullptr;
            const long val = std::strtol(val_str.c_str(), &end, 10);
            if(end == val_str.c_str()) return;

            if(uom == "FL")
            {
                *out_val = static_cast<int>(val) * 100;
                *out_ref = "STD";
                return;
            }
            if(code == "HEI")
            {
                *out_val = static_cast<int>(val);
                *out_ref = "SFC";
                return;
            }
            if(val >= ALT_UNLIMITED_FT)
            {
                *out_val = ALT_UNLIMITED_FT;
                *out_ref = "OTHER";
                return;
            }
            *out_val = static_cast<int>(val);
            *out_ref = "MSL";
        }

        // Convenience: child element text or empty string. Avoids
        // fishing through .child(...).text() at every call site.
        std::string child_text(const pugi::xml_node& parent, const char* name)
        {
            return parent.child(name).text().get();
        }
    }

    std::optional<tfr> parse_xnotam(const std::string& xml)
    {
        pugi::xml_document doc;
        const auto load_result = doc.load_buffer(
            xml.data(), xml.size());
        if(!load_result)
            throw xnotam_parse_error(
                std::string("XNOTAM XML parse failed: ") +
                load_result.description());

        const auto root = doc.child("XNOTAM-Update");
        if(!root) return std::nullopt;
        const auto group = root.child("Group");
        if(!group) return std::nullopt;
        const auto add = group.child("Add");
        if(!add) return std::nullopt;
        const auto notam = add.child("Not");
        if(!notam) return std::nullopt;

        tfr out{};
        out.tfr_id = 0;  // assigned by the caller (tfr_source)
        out.notam_id = child_text(notam.child("NotUid"), "txtLocalName");
        out.date_effective = child_text(notam, "dateEffective");
        out.date_expire = child_text(notam, "dateExpire");
        out.facility = child_text(notam, "codeFacility");
        out.description = child_text(notam, "txtDescrUSNS");

        const auto tfr_not = notam.child("TfrNot");
        if(!tfr_not) return std::nullopt;
        out.tfr_type = child_text(tfr_not, "codeType");

        for(const auto& area_group : tfr_not.children("TFRAreaGroup"))
        {
            const auto ase = area_group.child("aseTFRArea");
            if(!ase) continue;

            tfr_area area{};
            area.area_id = 0;
            area.area_name = child_text(ase, "txtName");

            parse_altitude(
                child_text(ase, "codeDistVerUpper"),
                child_text(ase, "valDistVerUpper"),
                child_text(ase, "uomDistVerUpper"),
                &area.upper_ft_val, &area.upper_ft_ref);
            parse_altitude(
                child_text(ase, "codeDistVerLower"),
                child_text(ase, "valDistVerLower"),
                child_text(ase, "uomDistVerLower"),
                &area.lower_ft_val, &area.lower_ft_ref);

            const auto sched = ase.child("ScheduleGroup");
            if(sched)
            {
                area.date_effective = child_text(sched, "dateEffective");
                area.date_expire = child_text(sched, "dateExpire");
            }

            // Pre-tessellated polygon from abdMergedArea — same
            // shape build_tfr.py reads. Areas without enough vertices
            // for a real polygon are dropped.
            const auto merged = area_group.child("abdMergedArea");
            if(!merged) continue;
            for(const auto& avx : merged.children("Avx"))
            {
                const auto lat_str = child_text(avx, "geoLat");
                const auto lon_str = child_text(avx, "geoLong");
                if(lat_str.empty() || lon_str.empty()) continue;
                double lon = 0.0;
                double lat = 0.0;
                if(!parse_coord(lat_str, lon_str, &lon, &lat)) continue;
                area.points.push_back({lat, lon});
            }
            if(area.points.size() < 3) continue;
            out.areas.push_back(std::move(area));
        }

        if(out.areas.empty()) return std::nullopt;
        return out;
    }
}
