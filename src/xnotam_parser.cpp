#include "xnotam_parser.hpp"
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <pugixml.hpp>
#include <sdl/log.hpp>
#include <string>
#include <unordered_set>
#include <vector>

namespace osect
{
    namespace
    {
        // ---- catch-all unknown-path logger ----
        //
        // On every successful parse, the walker visits each text-bearing
        // leaf element and looks up its full path in the two sets
        // below. Anything not covered (and not under one of the
        // ignored_prefixes) gets log_info'd once per process so future
        // FAA schema additions are visible. We do not scan XML
        // attributes — new fields land as elements in practice and
        // skipping attributes avoids namespace / xmlns false positives.
        //
        // The two sets carry classification — they're the
        // documentation of "what does the parser do with each path"
        // and are co-located with the parser so they can't drift:
        //
        //   consumed_paths()  — paths parse_xnotam actively reads. If
        //                       you remove a value from here while the
        //                       parser still reads it, you've broken
        //                       the audit trail. New consumers add
        //                       their path here in the same change.
        //   ignored_paths()   — paths classified as not worth
        //                       surfacing: envelope metadata, FAA
        //                       internal tracking IDs, per-area flags,
        //                       alternate representations of data we
        //                       already get a better form of.
        //
        // ignored_prefixes() handles variable-shape subtrees that
        // would be tedious to enumerate leaf-by-leaf (Aac airspace
        // hierarchy, NotUidMod reissue metadata, aseShapes FRD
        // geometry).

        const std::unordered_set<std::string>& consumed_paths()
        {
            // Every entry is read directly in parse_xnotam(). Order
            // mirrors the parser body for grep-ability.
            static const std::unordered_set<std::string> s = {
                "XNOTAM-Update/Group/Add/Not/NotUid/txtLocalName",
                "XNOTAM-Update/Group/Add/Not/NotUid/dateIssued",
                "XNOTAM-Update/Group/Add/Not/dateEffective",
                "XNOTAM-Update/Group/Add/Not/dateExpire",
                "XNOTAM-Update/Group/Add/Not/codeFacility",
                "XNOTAM-Update/Group/Add/Not/codeTimeZone",
                "XNOTAM-Update/Group/Add/Not/codeExpirationTimeZone",
                "XNOTAM-Update/Group/Add/Not/txtDescrUSNS",
                "XNOTAM-Update/Group/Add/Not/AffLocGroup/txtNameCity",
                "XNOTAM-Update/Group/Add/Not/AffLocGroup/txtNameUSState",
                "XNOTAM-Update/Group/Add/Not/codeCoordFacility",
                "XNOTAM-Update/Group/Add/Not/txtNameCoordFacility",
                "XNOTAM-Update/Group/Add/Not/codeCoordFacilityType",
                "XNOTAM-Update/Group/Add/Not/txtAddrCoordPhone",
                "XNOTAM-Update/Group/Add/Not/valFreqCoord",
                "XNOTAM-Update/Group/Add/Not/txtNamePOC",
                "XNOTAM-Update/Group/Add/Not/txtNamePOCOrg",
                "XNOTAM-Update/Group/Add/Not/txtAddrPOCPhone",
                "XNOTAM-Update/Group/Add/Not/valFreqPOC",
                "XNOTAM-Update/Group/Add/Not/TfrNot/codeType",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/InstructionsGroup/txtInstr",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/txtName",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/codeDistVerUpper",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/valDistVerUpper",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/uomDistVerUpper",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/codeDistVerLower",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/valDistVerLower",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/uomDistVerLower",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/dayCode",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/ScheduleGroup/dateEffective",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/ScheduleGroup/dateExpire",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/ScheduleGroup/startTime",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/ScheduleGroup/endTime",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/ScheduleGroup/isTimeSeparate",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/abdMergedArea/Avx/geoLat",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/abdMergedArea/Avx/geoLong",
            };
            return s;
        }

        const std::unordered_set<std::string>& ignored_paths()
        {
            // Classified as not worth surfacing. Removing one of these
            // (or moving it to consumed/reserved) is fine — the catch-
            // all will re-flag it on the next production refresh and
            // we can reclassify with new context.
            static const std::unordered_set<std::string> s = {
                // NotUid bookkeeping.
                "XNOTAM-Update/Group/Add/Not/NotUid/txtNameAcctFac",
                "XNOTAM-Update/Group/Add/Not/NotUid/dateIndexYear",
                "XNOTAM-Update/Group/Add/Not/NotUid/noSeqNo",
                "XNOTAM-Update/Group/Add/Not/NotUid/codeGUID",
                "XNOTAM-Update/Group/Add/Not/NotUid/noUSNSWorkNo",

                // Not — encoding hints, alternate description forms,
                // and per-NOTAM flags we don't surface.
                "XNOTAM-Update/Group/Add/Not/codeDailyOper",
                "XNOTAM-Update/Group/Add/Not/codeFreeformText",
                "XNOTAM-Update/Group/Add/Not/txtDescrModern",
                "XNOTAM-Update/Group/Add/Not/txtDescrTraditional",
                "XNOTAM-Update/Group/Add/Not/txtDescrPurpose",
                "XNOTAM-Update/Group/Add/Not/txtDescrMod",
                "XNOTAM-Update/Group/Add/Not/codeFacilityStateOverride",

                // TfrNot — template metadata.
                "XNOTAM-Update/Group/Add/Not/TfrNot/codeCtrlFacilityType",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TemplateType",

                // TFRAreaGroup — per-area flags.
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/codeAuthATC",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/codeIncFRD",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/codeLclTime",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/codeShpPrt",

                // aseTFRArea — exclusion codes (we use the inclusion
                // codes), working-hour code, airspace UID, per-area flag.
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/codeExclVerLower",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/codeExclVerUpper",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/isScheduledTfrArea",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/AseUid/codeId",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/AseUid/codeType",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseTFRArea/Att/codeWorkHr",

                // abdMergedArea — per-vertex metadata and remarks.
                // (geoLat/geoLong sit in consumed_paths; these are the
                // sibling fields we ignore.)
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/abdMergedArea/Avx/codeType",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/abdMergedArea/Avx/codeDatum",
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/abdMergedArea/txtRmk",
            };
            return s;
        }

        const std::vector<std::string>& ignored_prefixes()
        {
            static const std::vector<std::string> v = {
                // Airspace activity codes — only meaningful for nested
                // airspace, variable shape.
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/Aac/",
                // Predefined airspace references inside the merged
                // polygon, variable shape.
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/abdMergedArea/AbdUid/",
                // Alternate geometry repr (Fix-Radial-Distance) —
                // abdMergedArea is the pre-tessellated form we render
                // from instead.
                "XNOTAM-Update/Group/Add/Not/TfrNot/TFRAreaGroup/aseShapes/",
                // Reissue/modification metadata — prior NOTAM's
                // identifying triple under a parallel NotUidMod
                // container. Not pilot-actionable.
                "XNOTAM-Update/Group/Add/Not/NotUidMod/",
            };
            return v;
        }

        struct unknown_log
        {
            std::mutex mtx;
            std::unordered_set<std::string> seen;
        };

        unknown_log& global_unknown_log()
        {
            static unknown_log inst;
            return inst;
        }

        bool path_is_known(const std::string& path)
        {
            if(consumed_paths().count(path) || ignored_paths().count(path))
            {
                return true;
            }
            const auto& prefs = ignored_prefixes();
            return std::any_of(prefs.begin(), prefs.end(),
                               [&](const std::string& pref) {
                                   return path.size() >= pref.size() &&
                                          path.compare(0, pref.size(), pref) == 0;
                               });
        }

        void report_unknown(const std::string& path, const std::string& notam_id)
        {
            if(path_is_known(path))
            {
                return;
            }
            auto& log = global_unknown_log();
            {
                std::lock_guard<std::mutex> lk(log.mtx);
                if(!log.seen.insert(path).second)
                {
                    return; // already logged this session
                }
            }
            sdl::log_info("xnotam unknown path: " + path + " (notam=" + notam_id + ")");
        }

        // Recursively walk every element. For each element with no
        // element children and non-empty text, treat it as a leaf and
        // report its full path. Elements with children recurse; the
        // path is mutated in place and restored on the way back up.
        void walk_unknown(const pugi::xml_node& node, std::string& path, const std::string& notam_id)
        {
            bool any_child = false;
            for(const auto& child : node.children())
            {
                if(child.type() != pugi::node_element)
                {
                    continue;
                }
                any_child = true;
                const auto orig_size = path.size();
                path.push_back('/');
                path.append(child.name());
                walk_unknown(child, path, notam_id);
                path.resize(orig_size);
            }
            if(!any_child)
            {
                const char* text = node.text().get();
                if(*text == 0)
                {
                    return; // empty leaf — placeholder, ignore
                }
                report_unknown(path, notam_id);
            }
        }

        // FAA's "unlimited altitude" sentinel — mirrors
        // ALT_UNLIMITED_FT in tools/build_common.py.
        constexpr int ALT_UNLIMITED_FT = 99999;

        // Parse XNOTAM coordinate strings like "25.94179199N",
        // "080.86666667W". Returns true on success and populates
        // *lon / *lat. Mirrors _parse_xnotam_coord in build_tfr.py.
        bool parse_coord(const std::string& lat_str, const std::string& lon_str, double* lon, double* lat)
        {
            if(lat_str.size() < 2 || lon_str.size() < 2)
            {
                return false;
            }
            const char lat_hem = lat_str.back();
            const char lon_hem = lon_str.back();
            char* end = nullptr;
            double v_lat = std::strtod(lat_str.c_str(), &end);
            if(end == lat_str.c_str())
            {
                return false;
            }
            double v_lon = std::strtod(lon_str.c_str(), &end);
            if(end == lon_str.c_str())
            {
                return false;
            }
            if(lat_hem == 'S')
            {
                v_lat = -v_lat;
            }
            else if(lat_hem != 'N')
            {
                return false;
            }
            if(lon_hem == 'W')
            {
                v_lon = -v_lon;
            }
            else if(lon_hem != 'E')
            {
                return false;
            }
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
        void parse_altitude(const std::string& code, const std::string& val_str, const std::string& uom, int* out_val,
                            std::string* out_ref)
        {
            *out_val = 0;
            out_ref->clear();
            if(val_str.empty())
            {
                return;
            }
            char* end = nullptr;
            const long val = std::strtol(val_str.c_str(), &end, 10);
            if(end == val_str.c_str())
            {
                return;
            }

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
        const auto load_result = doc.load_buffer(xml.data(), xml.size());
        if(!load_result)
        {
            throw xnotam_parse_error(std::string("XNOTAM XML parse failed: ") + load_result.description());
        }

        const auto root = doc.child("XNOTAM-Update");
        if(!root)
        {
            return std::nullopt;
        }
        const auto group = root.child("Group");
        if(!group)
        {
            return std::nullopt;
        }
        const auto add = group.child("Add");
        if(!add)
        {
            return std::nullopt;
        }
        const auto notam = add.child("Not");
        if(!notam)
        {
            return std::nullopt;
        }

        tfr out{};
        out.tfr_id = 0; // assigned by the caller (tfr_source)
        out.notam_id            = child_text(notam.child("NotUid"), "txtLocalName");
        out.date_issued         = child_text(notam.child("NotUid"), "dateIssued");
        out.date_effective      = child_text(notam, "dateEffective");
        out.date_expire         = child_text(notam, "dateExpire");
        out.facility            = child_text(notam, "codeFacility");
        out.description         = child_text(notam, "txtDescrUSNS");
        out.city                = child_text(notam.child("AffLocGroup"), "txtNameCity");
        out.state               = child_text(notam.child("AffLocGroup"), "txtNameUSState");
        out.coord_facility      = child_text(notam, "codeCoordFacility");
        out.coord_facility_name = child_text(notam, "txtNameCoordFacility");
        out.coord_facility_type = child_text(notam, "codeCoordFacilityType");
        out.coord_phone         = child_text(notam, "txtAddrCoordPhone");
        out.coord_freq          = child_text(notam, "valFreqCoord");
        out.poc_name            = child_text(notam, "txtNamePOC");
        out.poc_org             = child_text(notam, "txtNamePOCOrg");
        out.poc_phone           = child_text(notam, "txtAddrPOCPhone");
        out.poc_freq            = child_text(notam, "valFreqPOC");
        out.time_zone           = child_text(notam, "codeTimeZone");
        out.expire_time_zone    = child_text(notam, "codeExpirationTimeZone");

        const auto tfr_not = notam.child("TfrNot");
        if(!tfr_not)
        {
            return std::nullopt;
        }
        out.tfr_type = child_text(tfr_not, "codeType");

        for(const auto& area_group : tfr_not.children("TFRAreaGroup"))
        {
            const auto ase = area_group.child("aseTFRArea");
            if(!ase)
            {
                continue;
            }

            tfr_area area{};
            area.area_id = 0;
            area.area_name = child_text(ase, "txtName");

            parse_altitude(child_text(ase, "codeDistVerUpper"), child_text(ase, "valDistVerUpper"),
                           child_text(ase, "uomDistVerUpper"), &area.upper_ft_val, &area.upper_ft_ref);
            parse_altitude(child_text(ase, "codeDistVerLower"), child_text(ase, "valDistVerLower"),
                           child_text(ase, "uomDistVerLower"), &area.lower_ft_val, &area.lower_ft_ref);

            area.day_code = child_text(ase, "dayCode");
            const auto sched = ase.child("ScheduleGroup");
            if(sched)
            {
                area.date_effective = child_text(sched, "dateEffective");
                area.date_expire = child_text(sched, "dateExpire");
                area.start_time = child_text(sched, "startTime");
                area.end_time = child_text(sched, "endTime");
                area.is_time_separate = child_text(sched, "isTimeSeparate");
            }

            // Pilot instructions — the FAA emits one <txtInstr> per
            // paragraph under <InstructionsGroup>. Join with newlines
            // so popup rendering can display each paragraph on its
            // own line; empty paragraphs are skipped.
            const auto instr_group = area_group.child("InstructionsGroup");
            if(instr_group)
            {
                std::string joined;
                for(const auto& txt : instr_group.children("txtInstr"))
                {
                    const std::string s = txt.text().get();
                    if(s.empty())
                    {
                        continue;
                    }
                    if(!joined.empty())
                    {
                        joined.push_back('\n');
                    }
                    joined += s;
                }
                area.instructions = std::move(joined);
            }

            // Pre-tessellated polygon from abdMergedArea — same
            // shape build_tfr.py reads. Areas without enough vertices
            // for a real polygon are dropped.
            const auto merged = area_group.child("abdMergedArea");
            if(!merged)
            {
                continue;
            }
            for(const auto& avx : merged.children("Avx"))
            {
                const auto lat_str = child_text(avx, "geoLat");
                const auto lon_str = child_text(avx, "geoLong");
                if(lat_str.empty() || lon_str.empty())
                {
                    continue;
                }
                double lon = 0.0;
                double lat = 0.0;
                if(!parse_coord(lat_str, lon_str, &lon, &lat))
                {
                    continue;
                }
                area.points.push_back({lat, lon});
            }
            if(area.points.size() < 3)
            {
                continue;
            }
            out.areas.push_back(std::move(area));
        }

        if(out.areas.empty())
        {
            return std::nullopt;
        }

        // Successful parse — walk the tree once and surface any
        // text-bearing leaf whose path we don't recognize. Cancellation
        // and other nullopt paths are skipped because their envelopes
        // differ enough to swamp the log with non-actionable warnings.
        {
            std::string path = root.name();
            walk_unknown(root, path, out.notam_id);
        }

        return out;
    }
}
