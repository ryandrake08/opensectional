#!/usr/bin/env python3
"""Ingest FAA AIXM 5.0 SUA data into the NASR SQLite DB.

The source is a nested archive: aixm5.0.zip → SaaSubscriberFile.zip →
Saa_Sub_File.zip → per-SUA XML files.

Usage:
    python tools/build_aixm.py <aixm.zip> <output.db>
"""

import datetime
import io
import math
import sys
import xml.etree.ElementTree as ET
import zipfile
from collections import defaultdict

from build_common import (
    ALT_UNLIMITED_FT, handle_antimeridian, open_output_db, parse_altitude,
    read_meta, simplify_ring, subdivide_ring, write_meta,
)


# Per-SUA instrumentation for the geometry cleanup filters. Each key is
# a filter name; each value is a Counter {designator: hits}. Collected
# during build_sua; not currently printed but kept for ad-hoc diagnosis.
SUA_FILTER_STATS = defaultdict(lambda: defaultdict(int))


# WGS84 mean radius, used for great-circle destination math.
EARTH_RADIUS_M = 6371008.8


GML = "http://www.opengis.net/gml/3.2"
AIXM = "http://www.aixm.aero/schema/5.0"
SAA = "urn:us:gov:dot:faa:aim:saa"
SUA_NS = "urn:us:gov:dot:faa:aim:saa:sua"
XLINK = "http://www.w3.org/1999/xlink"


def _radius_to_meters(radius, uom):
    if uom == "NM":
        return radius * 1852.0
    if uom == "MI":
        return radius * 1609.344
    if uom == "FT":
        return radius * 0.3048
    if uom == "KM":
        return radius * 1000.0
    if uom == "M":
        return radius
    return radius * 1852.0  # default to NM


def destination_point(lat_deg, lon_deg, bearing_deg, distance_m):
    """Great-circle destination from origin.

    Bearing is degrees clockwise from true north. Returns (lon, lat) in
    degrees. Uses the spherical earth model with WGS84 mean radius.
    """
    d_R = distance_m / EARTH_RADIUS_M
    lat1 = math.radians(lat_deg)
    lon1 = math.radians(lon_deg)
    br = math.radians(bearing_deg)
    sin_lat2 = (math.sin(lat1) * math.cos(d_R)
                + math.cos(lat1) * math.sin(d_R) * math.cos(br))
    lat2 = math.asin(sin_lat2)
    lon2 = lon1 + math.atan2(
        math.sin(br) * math.sin(d_R) * math.cos(lat1),
        math.cos(d_R) - math.sin(lat1) * sin_lat2)
    return math.degrees(lon2), math.degrees(lat2)


def _math_angle_to_bearing(math_deg):
    # Current FAA AIXM-SAA data encodes arc angles in math convention:
    # 0° = east, 90° = north, CCW. Convert to compass bearing (0° = north, CW).
    return (90.0 - math_deg) % 360.0


def generate_circle(center_lon, center_lat, radius, uom, num_points=72):
    """Polygon approximating a circle of constant great-circle radius."""
    meters = _radius_to_meters(radius, uom)
    points = []
    for i in range(num_points):
        math_deg = 360.0 * i / num_points
        bearing = _math_angle_to_bearing(math_deg)
        points.append(destination_point(center_lat, center_lon, bearing, meters))
    points.append(points[0])
    return points


def generate_arc(center_lon, center_lat, radius, uom, start_deg, end_deg):
    """Points along an arc from start_deg to end_deg.

    Angles are degrees in math convention (0° = east, CCW), matching the
    arc-angle encoding seen in the FAA AIXM-SAA source. Each sample uses
    great-circle destination math so the distance is a true great-circle
    radius (not a planar offset that grows worse at high latitudes).
    """
    meters = _radius_to_meters(radius, uom)
    sweep = end_deg - start_deg
    num_points = max(int(abs(sweep) / 5.0), 2)
    points = []
    for i in range(num_points + 1):
        math_deg = start_deg + sweep * i / num_points
        bearing = _math_angle_to_bearing(math_deg)
        points.append(destination_point(center_lat, center_lon, bearing, meters))
    return points


def parse_ring_points(ring_elem):
    """Extract polygon points from a GML Ring element.

    Handles LineStringSegment, CircleByCenterPoint, and ArcByCenterPoint
    curve segments. Multiple curveMember elements within a Ring are
    concatenated to form the complete boundary.

    Arc endpoints are snapped to the declared corner coordinates from
    adjacent LineStringSegments: those <gml:pos> values are the AIXM's
    authoritative corner positions, while an arc's mathematical endpoint
    (center + radius * angle) is an approximation that drifts tens to
    thousands of meters from the declared corner due to DMS rounding and
    spherical geometry. Without this, arc-to-line joins introduce
    self-intersections, spurious spikes at corners, and floating-point
    sliver artifacts downstream.
    """
    # First pass: collect segments as descriptors; detect pure circle.
    raw = []  # ("line", [(lon,lat),...]) | ("arc", clon, clat, rad, uom, s, e)
    for curve_member in ring_elem.findall(f"{{{GML}}}curveMember"):
        curve = curve_member.find(f"{{{GML}}}Curve")
        if curve is None:
            continue
        segments = curve.find(f"{{{GML}}}segments")
        if segments is None:
            continue
        for seg in segments:
            tag = seg.tag.split("}")[-1]
            if tag == "LineStringSegment":
                pts = []
                for pos in seg.findall(f"{{{GML}}}pos"):
                    lon, lat = pos.text.strip().split()
                    pts.append((float(lon), float(lat)))
                if pts:
                    raw.append(("line", pts))
            elif tag == "CircleByCenterPoint":
                pos = seg.find(f".//{{{GML}}}pos")
                rad = seg.find(f"{{{GML}}}radius")
                if pos is not None and rad is not None:
                    lon, lat = pos.text.strip().split()
                    clon, clat = float(lon), float(lat)
                    radius_val = float(rad.text)
                    uom = rad.get("uom", "NM")
                    circle_pts = generate_circle(clon, clat, radius_val, uom)
                    return circle_pts, {
                        "lon": clon, "lat": clat,
                        "radius": radius_val, "uom": uom,
                    }
            elif tag == "ArcByCenterPoint":
                pos = seg.find(f".//{{{GML}}}pos")
                rad = seg.find(f"{{{GML}}}radius")
                sa = seg.find(f"{{{GML}}}startAngle")
                ea = seg.find(f"{{{GML}}}endAngle")
                if (pos is not None and rad is not None
                        and sa is not None and ea is not None):
                    lon, lat = pos.text.strip().split()
                    raw.append(("arc", float(lon), float(lat),
                                float(rad.text), rad.get("uom", "NM"),
                                float(sa.text), float(ea.text)))

    n = len(raw)

    def prev_corner(i):
        # Only snap to an immediately adjacent LineStringSegment (treating
        # the ring as a closed loop). For arc-to-arc transitions we leave
        # the computed position alone — snapping to a distant line's
        # endpoint would teleport the arc across the ring.
        prev = raw[(i - 1) % n]
        return prev[1][-1] if prev[0] == "line" else None

    def next_corner(i):
        nxt = raw[(i + 1) % n]
        return nxt[1][0] if nxt[0] == "line" else None

    # Second pass: emit points, snapping arc endpoints to declared corners.
    points = []
    for i, seg in enumerate(raw):
        if seg[0] == "line":
            for p in seg[1]:
                if not points or points[-1] != p:
                    points.append(p)
        else:
            _, clon, clat, radius, uom, start_deg, end_deg = seg
            arc_pts = generate_arc(clon, clat, radius, uom, start_deg, end_deg)
            if not arc_pts:
                continue
            pc = prev_corner(i)
            nc = next_corner(i)
            if pc is not None:
                arc_pts[0] = pc
            if nc is not None:
                arc_pts[-1] = nc
            if points and points[-1] == arc_pts[0]:
                arc_pts = arc_pts[1:]
            points.extend(arc_pts)

    return points, None


def _text(parent, tag):
    """Get text content of a child element, or empty string."""
    elem = parent.find(f".//{tag}")
    return elem.text.strip() if elem is not None and elem.text else ""


def _collect_additive_shapes(airspace):
    """Extract BASE + UNION ring point lists from an AIXM Airspace.

    Returns a list of (shape, upper_str, lower_str) for each additive
    geometry component with a resolvable horizontal projection. Used
    by the SUBTR-by-reference path when R-4812 et al. express the
    carved-out region as `<contributorAirspace xlink:href=#...>`.
    """
    ts = airspace.find(f".//{{{AIXM}}}AirspaceTimeSlice")
    if ts is None:
        return []
    out = []
    for gc in ts.findall(f"{{{AIXM}}}geometryComponent"):
        agc = gc.find(f"{{{AIXM}}}AirspaceGeometryComponent")
        if agc is None:
            continue
        op = agc.find(f"{{{AIXM}}}operation")
        if op is None or op.text not in ("BASE", "UNION"):
            continue
        vol = agc.find(f".//{{{AIXM}}}AirspaceVolume")
        if vol is None:
            continue
        upper = vol.find(f"{{{AIXM}}}upperLimit")
        lower = vol.find(f"{{{AIXM}}}lowerLimit")
        uref = vol.find(f"{{{AIXM}}}upperLimitReference")
        lref = vol.find(f"{{{AIXM}}}lowerLimitReference")
        cu = (f"{upper.text} {upper.get('uom','')} "
              f"{uref.text if uref is not None else ''}".strip()
              if upper is not None and upper.text else "")
        cl = (f"{lower.text} {lower.get('uom','')} "
              f"{lref.text if lref is not None else ''}".strip()
              if lower is not None and lower.text else "")
        hp = vol.find(f".//{{{AIXM}}}horizontalProjection")
        if hp is None:
            continue
        shape = []
        ring = hp.find(f".//{{{GML}}}Ring")
        if ring is not None:
            shape, _ = parse_ring_points(ring)
        else:
            lr = hp.find(f".//{{{GML}}}LinearRing")
            if lr is not None:
                for pos in lr.findall(f"{{{GML}}}pos"):
                    if pos.text is None:
                        continue
                    lon, lat = pos.text.strip().split()
                    shape.append((float(lon), float(lat)))
        if shape:
            out.append((shape, cu, cl))
    return out


def _parse_one_airspace(airspace, airspace_lookup=None):
    """Parse a single AIXM Airspace element.

    Returns a dict with designator, name, sua_type, metadata fields,
    and parts, or None if the element has no usable geometry.

    `airspace_lookup` maps gml:id → Airspace element and is used to
    resolve cross-airspace SUBTR references (`<contributorAirspace>
    <theAirspace xlink:href="#..."/></contributorAirspace>`).
    """
    if airspace_lookup is None:
        airspace_lookup = {}
    ts = airspace.find(f".//{{{AIXM}}}AirspaceTimeSlice")
    if ts is None:
        return None

    desig_elem = ts.find(f"{{{AIXM}}}designator")
    name_elem = ts.find(f"{{{AIXM}}}name")
    designator = desig_elem.text if desig_elem is not None else ""
    name = name_elem.text if name_elem is not None else ""

    # Get SUA type from extension
    sua_type = ""
    for ext in ts.iter(f"{{{SUA_NS}}}suaType"):
        sua_type = ext.text or ""
        break

    # SAA extension metadata (administrativeArea, city,
    # legalDefinitionType, conditionalExclusion).
    # `conditionalExclusion=YES` flags an SUA whose legal definition
    # contains a special exclusion clause (typically a corridor or
    # carve-out described in the legal note); ~63 SUAs corpus-wide.
    admin_area = _text(ts, f"{{{SAA}}}administrativeArea")
    saa_city = _text(ts, f"{{{SAA}}}city")
    conditional_exclusion = _text(ts, f"{{{SAA}}}conditionalExclusion")
    # Hours of NOTAM lead time required before the SUA may be
    # activated. Stored as the source string (uom is always HR).
    time_in_advance = _text(ts, f"{{{SAA}}}timeInAdvance")

    # AIXM metadata from AirspaceActivation and related elements
    activity = _text(ts, f"{{{AIXM}}}activity")
    status = _text(ts, f"{{{AIXM}}}statusActivation")
    working_hours = _text(ts, f"{{{AIXM}}}workingHours")
    military = _text(ts, f"{{{AIXM}}}military")
    compliant_icao = _text(ts, f"{{{AIXM}}}compliantICAO")

    # Legal definition note (annotation where propertyName=legalDefinitionType)
    legal_note = ""
    for annotation in ts.findall(f"{{{AIXM}}}annotation"):
        note_elem = annotation.find(f".//{{{AIXM}}}Note")
        if note_elem is None:
            continue
        prop = note_elem.find(f"{{{AIXM}}}propertyName")
        if prop is not None and prop.text == "legalDefinitionType":
            ln = note_elem.find(f".//{{{AIXM}}}note")
            if ln is not None and ln.text:
                legal_note = ln.text.strip()
                break

    # Collect geometry from all components, ordered by sequence
    upper_limit = ""
    lower_limit = ""
    min_alt_limit = ""
    max_alt_limit = ""
    additive = []   # BASE + UNION components (merged into outer rings)
    subtractive = []  # SUBTR components (altitude restriction zones)
    for gc in ts.findall(f"{{{AIXM}}}geometryComponent"):
        agc = gc.find(f"{{{AIXM}}}AirspaceGeometryComponent")
        if agc is None:
            continue
        op = agc.find(f"{{{AIXM}}}operation")
        if op is None or op.text not in ("BASE", "UNION", "SUBTR"):
            continue
        seq = agc.find(f"{{{AIXM}}}operationSequence")
        seq_num = int(seq.text) if seq is not None and seq.text else 0

        vol = agc.find(f".//{{{AIXM}}}AirspaceVolume")

        # Extract altitude limits from this component
        comp_upper = ""
        comp_lower = ""
        if vol is not None:
            upper = vol.find(f"{{{AIXM}}}upperLimit")
            lower = vol.find(f"{{{AIXM}}}lowerLimit")
            upper_ref = vol.find(f"{{{AIXM}}}upperLimitReference")
            lower_ref = vol.find(f"{{{AIXM}}}lowerLimitReference")
            if upper is not None:
                uom = upper.get("uom", "")
                ref = upper_ref.text if upper_ref is not None else ""
                comp_upper = f"{upper.text} {uom} {ref}".strip()
            if lower is not None:
                uom = lower.get("uom", "")
                ref = lower_ref.text if lower_ref is not None else ""
                comp_lower = f"{lower.text} {uom} {ref}".strip()

        if op.text == "BASE":
            upper_limit = comp_upper
            lower_limit = comp_lower
            # AIXM lets the BASE volume carry minimumLimit /
            # maximumLimit pair — terrain-clearance constraints
            # supplementing the MSL band (typical pattern: MOA
            # declared as "11000 MSL → FL180" with min 3000 SFC,
            # meaning ops must remain ≥ 3000 ft AGL inside that
            # MSL slab). Used by ~15 alpine SUAs in the corpus.
            min_lim_e = vol.find(f"{{{AIXM}}}minimumLimit") if vol is not None else None
            max_lim_e = vol.find(f"{{{AIXM}}}maximumLimit") if vol is not None else None
            min_ref_e = vol.find(f"{{{AIXM}}}minimumLimitReference") if vol is not None else None
            max_ref_e = vol.find(f"{{{AIXM}}}maximumLimitReference") if vol is not None else None
            if min_lim_e is not None and min_lim_e.text:
                min_uom = min_lim_e.get("uom", "")
                min_ref = min_ref_e.text if min_ref_e is not None else ""
                min_alt_limit = f"{min_lim_e.text} {min_uom} {min_ref}".strip()
            else:
                min_alt_limit = ""
            if max_lim_e is not None and max_lim_e.text:
                max_uom = max_lim_e.get("uom", "")
                max_ref = max_ref_e.text if max_ref_e is not None else ""
                max_alt_limit = f"{max_lim_e.text} {max_uom} {max_ref}".strip()
            else:
                max_alt_limit = ""

        shape = []
        circle_info = None
        hp = vol.find(f".//{{{AIXM}}}horizontalProjection") if vol is not None else None
        if hp is not None:
            ring = hp.find(f".//{{{GML}}}Ring")
            if ring is not None:
                shape, circle_info = parse_ring_points(ring)
            else:
                linear_ring = hp.find(f".//{{{GML}}}LinearRing")
                if linear_ring is not None:
                    for pos in linear_ring.findall(f"{{{GML}}}pos"):
                        if pos.text is None:
                            continue
                        lon, lat = pos.text.strip().split()
                        shape.append((float(lon), float(lat)))

        # SUBTR-by-reference: AIXM lets a SUBTR define its region by
        # pointing at another airspace in the same file rather than
        # listing a polygon. Resolve the xlink and emit one SUBTR per
        # additive component of the referenced airspace. Used by
        # R-4812 to subtract R-4804A and R-4810.
        if (op.text == "SUBTR" and not shape and vol is not None
                and airspace_lookup):
            ref_elem = vol.find(
                f".//{{{AIXM}}}contributorAirspace/"
                f"{{{AIXM}}}AirspaceVolumeDependency/"
                f"{{{AIXM}}}theAirspace")
            if ref_elem is not None:
                href = ref_elem.get(f"{{{XLINK}}}href", "")
                ref_id = href.lstrip("#")
                ref_airspace = airspace_lookup.get(ref_id)
                if ref_airspace is not None:
                    for ref_shape, ref_up, ref_lo in _collect_additive_shapes(
                            ref_airspace):
                        # Blank altitudes at the reference mean "inherit
                        # from the referenced airspace", which in practice
                        # always covers BASE's band for these cutouts.
                        eff_upper = comp_upper or ref_up
                        eff_lower = comp_lower or ref_lo
                        subtractive.append((seq_num, ref_shape,
                                            eff_upper, eff_lower))
                    # referenced-airspace SUBTRs handled; skip fallthrough
                    continue
        if shape:
            if op.text == "SUBTR":
                subtractive.append((seq_num, shape, comp_upper, comp_lower))
            else:
                additive.append((seq_num, shape, circle_info,
                                 comp_upper, comp_lower))

    if not additive:
        return None

    additive.sort(key=lambda c: c[0])

    # Partition additive components (BASE + UNIONs) by altitude band. A
    # UNION whose floor/ceiling differ from BASE (e.g. W-497B seq 3 at
    # 5000–UNL while BASE is GND–UNL) is a separate altitude stratum and
    # must not be geometrically merged with BASE — doing so loses the
    # altitude restriction AND creates bogus topology where the strata
    # don't share ring vertices. Components sharing an altitude band are
    # still unioned together (disjoint islands, edge-adjacent annex lobes).
    from shapely.geometry import Polygon as ShapelyPolygon
    from shapely.geometry import MultiPolygon as ShapelyMultiPolygon
    from shapely.ops import unary_union
    from shapely.validation import make_valid
    from shapely import set_precision
    # Snap to ~1 m grid so polygons that share edges (sampled with slightly
    # different floating-point vertices) produce a clean union instead of
    # sliver holes. Airspace boundaries are not defined to sub-meter
    # precision, so this is well below any meaningful threshold.
    SNAP = 1e-5
    # Drop outer polygon pieces below this absolute area in deg² —
    # 1e-5 ≈ 0.03 NM². Empirically, floating-point fragments from
    # `unary_union` on overlapping sampled circles fall in 1e-8…5e-6;
    # the smallest real disconnected airspace component in the corpus
    # (WARRIOR 1 LOW's eastern protrusion severed by a SUBTR) is
    # ~3.9e-4, well above. A prior ratio-based filter (< 0.1% of the
    # main piece) wrongly dropped that legitimate region.
    OUTER_SLIVER_AREA_MIN = 1e-5

    # Altitude bands are now (value, ref) tuples for each of (lower,
    # upper). Two bands are equal only when both value AND reference
    # match — otherwise we have no way to compare them without a DEM,
    # so they stay as distinct strata.
    base_lo_alt = parse_altitude(lower_limit)   # (value, ref)
    base_up_alt = parse_altitude(upper_limit)
    base_band = (base_lo_alt, base_up_alt)

    # Group by full (lower, upper) altitude band — same value AND
    # same reference. BASE's band comes first because BASE is seq 0.
    bands = {}  # band_key -> list of entries
    band_order = []
    for entry in additive:
        _, _, _, comp_up, comp_lo = entry
        key = (parse_altitude(comp_lo), parse_altitude(comp_up))
        if key not in bands:
            bands[key] = []
            band_order.append(key)
        bands[key].append(entry)

    def _union_band(entries):
        """Union and clean a set of same-altitude polygons.

        Returns (polys, raw_polys). polys is the flat list of valid
        Shapely Polygons after union+sliver-cleanup; raw_polys is the
        unchanged set_precision'd input polygons (needed for SUBTR
        clipping against the full BASE-band union).
        """
        raw = []
        for _, shape, _, _, _ in entries:
            if len(shape) < 3:
                continue
            p = make_valid(ShapelyPolygon(shape))
            if p.is_empty:
                continue
            p = set_precision(p, SNAP)
            if not p.is_empty:
                raw.append(p)
        if not raw:
            return [], []
        unioned = unary_union(raw) if len(raw) > 1 else raw[0]
        flat = []
        def _collect(g):
            if isinstance(g, ShapelyPolygon):
                if not g.is_empty:
                    flat.append(g)
            elif isinstance(g, ShapelyMultiPolygon):
                for sub in g.geoms:
                    _collect(sub)
            elif hasattr(g, "geoms"):
                for sub in g.geoms:
                    _collect(sub)
        _collect(unioned)
        return flat, raw

    # Classify each SUBTR by its altitude band relative to BASE:
    #
    #   full_cover  — SUBTR's lower≤BASE.lower AND upper≥BASE.upper, AND
    #                 both lower-ref and upper-ref match BASE's. Becomes
    #                 a true 2D hole in BASE.
    #   partial_cover — refs match BASE's on both sides but the band is
    #                 a proper subset. Goes through face-overlay logic
    #                 to produce active-slice strata.
    #   standalone — refs do not all match BASE's. Without a DEM we
    #                cannot compare an AGL value to an MSL value, so
    #                we emit the SUBTR as its own stratum (footprint
    #                clipped to BASE outline) without any merging,
    #                subset elimination, or active-slice math.
    base_lo_val, base_lo_ref = base_lo_alt
    base_up_val, base_up_ref = base_up_alt
    full_cover_subs = []
    partial_cover_subs = []
    standalone_subs = []
    for sub in subtractive:
        _, shape, sub_upper, sub_lower = sub
        sub_lo = parse_altitude(sub_lower)
        sub_up = parse_altitude(sub_upper)
        sub_lo_val, sub_lo_ref = sub_lo
        sub_up_val, sub_up_ref = sub_up
        refs_match = (sub_lo_ref == base_lo_ref and sub_up_ref == base_up_ref
                      and sub_lo_ref is not None and sub_up_ref is not None)
        if not refs_match:
            standalone_subs.append(sub)
            continue
        is_full = (
            sub_lo_val is not None and sub_up_val is not None
            and base_lo_val is not None and base_up_val is not None
            and sub_lo_val <= base_lo_val and sub_up_val >= base_up_val)
        (full_cover_subs if is_full else partial_cover_subs).append(sub)

    full_cover_hole_geom = None
    if full_cover_subs:
        sub_polys = []
        for _, shape, _, _ in full_cover_subs:
            if len(shape) < 3:
                continue
            sp = make_valid(ShapelyPolygon(shape))
            if not sp.is_empty:
                sub_polys.append(sp)
        if sub_polys:
            full_cover_hole_geom = unary_union(sub_polys) if len(sub_polys) > 1 else sub_polys[0]

    # Pre-compute raw BASE-band union so partial-cover SUBTRs can be
    # clipped against it before the main band loop runs.
    base_entries_pre = bands.get(base_band, [])
    base_raw_polys = []
    for _, shape, _, _, _ in base_entries_pre:
        if len(shape) < 3:
            continue
        p = make_valid(ShapelyPolygon(shape))
        if p.is_empty:
            continue
        p = set_precision(p, SNAP)
        if not p.is_empty:
            base_raw_polys.append(p)
    outer_union = None
    if base_raw_polys:
        outer_union = (unary_union(base_raw_polys) if len(base_raw_polys) > 1
                        else base_raw_polys[0])

    # Clip every partial-cover SUBTR to the BASE outline (minus
    # full-cover holes) and keep its altitude band. These are fed
    # through a full 2D face overlay after the band loop so the
    # emitted strata are 3D-disjoint: at each point in the plane, the
    # carved altitude range is the union of every overlapping SUBTR's
    # range, and active slices are the complement within BASE's band.
    partial_subs_clipped = []   # [{lo_ft, up_ft, sub_upper, sub_lower, poly}]
    partial_cover_union = None
    if partial_cover_subs and outer_union is not None:
        effective_outer = outer_union
        if full_cover_hole_geom is not None:
            effective_outer = effective_outer.difference(full_cover_hole_geom)
        partial_cover_subs.sort(key=lambda c: c[0])
        for _, shape, sub_upper, sub_lower in partial_cover_subs:
            if len(shape) < 3 or effective_outer.is_empty:
                continue
            sub_poly = make_valid(ShapelyPolygon(shape))
            if sub_poly.is_empty:
                continue
            clipped = sub_poly.intersection(effective_outer)
            if clipped.is_empty:
                print(f"  NOTE: {designator}: SUBTR zone lies entirely "
                      f"outside outer boundary, skipping", file=sys.stderr)
                continue
            if isinstance(clipped, ShapelyMultiPolygon):
                polys_ = list(clipped.geoms)
            elif isinstance(clipped, ShapelyPolygon):
                polys_ = [clipped]
            elif hasattr(clipped, "geoms"):
                polys_ = [g for g in clipped.geoms
                          if isinstance(g, ShapelyPolygon) and not g.is_empty]
                if not polys_:
                    print(f"  NOTE: {designator}: SUBTR clip produced no "
                          f"polygonal bits, skipping", file=sys.stderr)
                    continue
            else:
                print(f"  WARN: {designator}: SUBTR intersection yielded "
                      f"{type(clipped).__name__}, skipping", file=sys.stderr)
                continue
            unioned = (unary_union(polys_) if len(polys_) > 1 else polys_[0])
            # Snap the clipped SUBTR to the same grid BASE polys use so
            # subsequent face-overlay intersections land exactly on
            # BASE's edge rather than drifting off by a few nm of
            # floating-point noise.
            unioned = set_precision(unioned, SNAP)
            # Drop SUBTRs whose clipped footprint is a near-zero-area
            # sliver or self-retracing path — these come from source
            # polygons that graze BASE's boundary and become degenerate
            # after intersection, not airspace we want to render.
            if unioned.is_empty or unioned.area < 1e-6:
                print(f"  NOTE: {designator}: SUBTR clipped footprint "
                      f"degenerate (area={unioned.area:.2e}), skipping",
                      file=sys.stderr)
                SUA_FILTER_STATS["SUBTR clipped degenerate (area<1e-6)"][designator] += 1
                continue
            # All partial-cover SUBTRs already match BASE's ref on
            # both sides, so integer comparison is sound here.
            partial_subs_clipped.append({
                "sub_upper": sub_upper, "sub_lower": sub_lower,
                "lo_ft": parse_altitude(sub_lower)[0],
                "up_ft": parse_altitude(sub_upper)[0],
                "poly": unioned,
            })
        if partial_subs_clipped:
            partial_cover_union = unary_union(
                [s["poly"] for s in partial_subs_clipped])

    # Combined carve-out geometry subtracted from BASE band. Anything a
    # SUBTR covers (fully or partially) is removed from BASE so the
    # BASE stratum only spans the unrestricted remainder.
    base_carve = None
    if full_cover_hole_geom is not None and partial_cover_union is not None:
        base_carve = unary_union([full_cover_hole_geom, partial_cover_union])
    elif full_cover_hole_geom is not None:
        base_carve = full_cover_hole_geom
    elif partial_cover_union is not None:
        base_carve = partial_cover_union

    # strata: list of {"upper_limit", "lower_limit", "parts": [(ring, is_hole), ...]}
    # Each stratum is a separately-selectable altitude band within the SUA.
    strata = []
    result_circle_info = None

    for band_key in band_order:
        entries = bands[band_key]
        polys, raw_polys = _union_band(entries)
        if not polys:
            if band_key == base_band:
                print(f"  WARN: {designator}: BASE band union yielded no "
                      f"polygons, skipping", file=sys.stderr)
            continue

        # Apply the combined carve-out on the BASE band so BASE's
        # footprint is disjoint (in 3D) from every SUBTR's active
        # slices — shapely produces interior rings for fully-enclosed
        # SUBTRs and trims the exterior for partial-edge SUBTRs.
        if band_key == base_band and base_carve is not None:
            diffed = []
            for p in polys:
                d = p.difference(base_carve)
                if d.is_empty:
                    continue
                if isinstance(d, ShapelyPolygon):
                    diffed.append(d)
                elif isinstance(d, ShapelyMultiPolygon):
                    diffed.extend(g for g in d.geoms if not g.is_empty)
                elif hasattr(d, "geoms"):
                    for g in d.geoms:
                        if isinstance(g, ShapelyPolygon) and not g.is_empty:
                            diffed.append(g)
            polys = diffed
            if not polys:
                print(f"  WARN: {designator}: BASE band fully consumed "
                      f"by SUBTR carve-outs, skipping", file=sys.stderr)
                continue

        # Sliver filter, per-band. Absolute-area threshold: drop outer
        # polygon pieces smaller than ~0.003 NM². Real disconnected
        # airspace regions (e.g. a protrusion severed by a SUBTR)
        # remain well above this; floating-point fragments from
        # `unary_union` on overlapping sampled circles fall below.
        outer_parts = []
        for poly in polys:
            if poly.is_empty:
                continue
            if len(polys) > 1 and poly.area < OUTER_SLIVER_AREA_MIN:
                SUA_FILTER_STATS["OUTER_SLIVER_AREA_MIN (outer piece dropped)"][designator] += 1
                continue
            outer_parts.append((list(poly.exterior.coords), False))
            for interior in poly.interiors:
                outer_parts.append((list(interior.coords), True))

        # Use the first entry's altitude strings as representative — all
        # entries in this band share normalized altitude. (String form
        # may differ slightly, e.g. "GND FT SFC" vs "0 FT SFC"; the first
        # entry's strings are stable and what was in the source.)
        up_str = entries[0][3]
        lo_str = entries[0][4]
        strata.append({
            "upper_limit": up_str,
            "lower_limit": lo_str,
            "parts": list(outer_parts),
        })

        if band_key == base_band:
            # Preserve circle_info only if the whole SUA is exactly one
            # unchanged circle in the BASE band.
            if (len(additive) == 1 and additive[0][2] is not None
                    and len(outer_parts) == 1 and not outer_parts[0][1]):
                result_circle_info = additive[0][2]

    if not strata:
        return None

    # Face-overlay active-slice emission: partition the union of
    # partial-cover SUBTR footprints into faces where each face is
    # covered by a specific subset of SUBTRs. Within each face the
    # carved altitude range is the union of every member SUBTR's
    # range; active slices are [baseLo, baseUp] minus that union. All
    # faces whose active-band list contains a given band are merged
    # into one stratum so a band appears as a single continuous
    # polygon regardless of which SUBTRs produced it.
    if partial_subs_clipped and base_lo_val is not None and base_up_val is not None:
        def _merge_ranges(rs):
            rs = sorted((s, e) for s, e in rs if s is not None and e is not None and s < e)
            out = []
            for s, e in rs:
                if out and s <= out[-1][1]:
                    out[-1] = (out[-1][0], max(out[-1][1], e))
                else:
                    out.append((s, e))
            return out

        def _complement(rs, lo, hi):
            merged = _merge_ranges(rs)
            result = []
            cur = lo
            for s, e in merged:
                if e <= cur:
                    continue
                if s > cur:
                    result.append((cur, min(s, hi)))
                cur = max(cur, e)
                if cur >= hi:
                    break
            if cur < hi:
                result.append((cur, hi))
            return result

        # Area threshold below which a polygon is treated as a
        # degenerate artifact (a thin sliver from a near-miss clip, or
        # a self-retracing near-zero-area loop that shapely still
        # reports as valid). 1e-6 deg² ≈ 10,000 m² ≈ 0.003 NM²; real
        # SUA features are always orders of magnitude larger.
        FACE_AREA_MIN = 1e-6

        def _collect_polys(geom):
            if geom.is_empty:
                return []
            if isinstance(geom, ShapelyPolygon):
                if geom.area >= FACE_AREA_MIN:
                    return [geom]
                SUA_FILTER_STATS["FACE_AREA_MIN (overlay sliver)"][designator] += 1
                return []
            if isinstance(geom, ShapelyMultiPolygon):
                out = []
                for g in geom.geoms:
                    if g.is_empty:
                        continue
                    if g.area >= FACE_AREA_MIN:
                        out.append(g)
                    else:
                        SUA_FILTER_STATS["FACE_AREA_MIN (overlay sliver)"][designator] += 1
                return out
            if hasattr(geom, "geoms"):
                out = []
                for g in geom.geoms:
                    if not isinstance(g, ShapelyPolygon) or g.is_empty:
                        continue
                    if g.area >= FACE_AREA_MIN:
                        out.append(g)
                    else:
                        SUA_FILTER_STATS["FACE_AREA_MIN (overlay sliver)"][designator] += 1
                return out
            return []

        # Synthesize the altitude string for one bound of an active
        # slice. The slice's bounds come from BASE.lower/BASE.upper or
        # from SUBTR.lower/SUBTR.upper (partial-cover SUBTRs share
        # BASE's refs, so SUBTR.upper has base_up_ref and SUBTR.lower
        # has base_lo_ref). `is_lower` selects which side we're
        # synthesizing — for the slice's lower bound, the value is
        # either base_lo_val (use base_lo_ref) or a SUBTR.upper
        # (use base_up_ref). For the upper bound, the value is either
        # base_up_val (use base_up_ref) or a SUBTR.lower (use base_lo_ref).
        def _alt_to_str(ft, is_lower):
            if ft is None:
                return ""
            if is_lower:
                if ft == base_lo_val:
                    return lower_limit
                ref = base_up_ref  # came from a SUBTR.upper
            else:
                if ft == base_up_val:
                    return upper_limit
                ref = base_lo_ref  # came from a SUBTR.lower
            if ft == 0 and ref == "SFC":
                return "GND FT SFC"
            if ft >= ALT_UNLIMITED_FT:
                return "UNL OTHER"
            return f"{ft} FT {ref}"

        n = len(partial_subs_clipped)
        # Collect (lo, hi) active band -> list of face polygons across
        # the overlay. Brute-force 2^n subset enumeration; n is small
        # in practice (<=10 across the current corpus).
        band_to_polys = {}  # (lo_ft, hi_ft) -> [ShapelyPolygon ...]
        for mask in range(1, 1 << n):
            members = [i for i in range(n) if mask & (1 << i)]
            face = partial_subs_clipped[members[0]]["poly"]
            skip = False
            for i in members[1:]:
                face = face.intersection(partial_subs_clipped[i]["poly"])
                if face.is_empty:
                    skip = True
                    break
            if skip or face.is_empty:
                continue
            for j in range(n):
                if mask & (1 << j):
                    continue
                face = face.difference(partial_subs_clipped[j]["poly"])
                if face.is_empty:
                    break
            face_polys = _collect_polys(face)
            if not face_polys:
                continue
            carved = [(partial_subs_clipped[i]["lo_ft"],
                       partial_subs_clipped[i]["up_ft"]) for i in members]
            for band in _complement(carved, base_lo_val, base_up_val):
                band_to_polys.setdefault(band, []).extend(face_polys)

        for (lo_ft, hi_ft), polys_ in band_to_polys.items():
            if not polys_:
                continue
            merged = unary_union(polys_) if len(polys_) > 1 else polys_[0]
            stratum_parts = []
            for p in _collect_polys(merged):
                stratum_parts.append((list(p.exterior.coords), False))
                for interior in p.interiors:
                    stratum_parts.append((list(interior.coords), True))
            if not stratum_parts:
                continue
            lo_str = _alt_to_str(lo_ft, is_lower=True)
            up_str = _alt_to_str(hi_ft, is_lower=False)
            strata.append({
                "upper_limit": up_str,
                "lower_limit": lo_str,
                "parts": stratum_parts,
            })

    # Standalone SUBTRs (altitude reference does not match BASE's, so
    # we cannot compare values without terrain). Each is emitted as a
    # single stratum with its raw altitude band, footprint clipped to
    # the BASE outline. No subtraction from BASE, no overlay math —
    # the user sees both the BASE region and this overlapping stratum.
    if standalone_subs and outer_union is not None and not outer_union.is_empty:
        standalone_subs.sort(key=lambda c: c[0])
        for _, shape, sub_upper, sub_lower in standalone_subs:
            if len(shape) < 3:
                continue
            sub_poly = make_valid(ShapelyPolygon(shape))
            if sub_poly.is_empty:
                continue
            clipped = sub_poly.intersection(outer_union)
            if clipped.is_empty:
                print(f"  NOTE: {designator}: standalone SUBTR (mixed-ref) "
                      f"lies outside BASE, skipping", file=sys.stderr)
                continue
            if isinstance(clipped, ShapelyMultiPolygon):
                polys_ = [g for g in clipped.geoms if not g.is_empty]
            elif isinstance(clipped, ShapelyPolygon):
                polys_ = [clipped]
            elif hasattr(clipped, "geoms"):
                polys_ = [g for g in clipped.geoms
                          if isinstance(g, ShapelyPolygon) and not g.is_empty]
            else:
                continue
            stratum_parts = []
            for p in polys_:
                if p.is_empty or p.area < 1e-6:
                    continue
                stratum_parts.append((list(p.exterior.coords), False))
                for interior in p.interiors:
                    stratum_parts.append((list(interior.coords), True))
            if stratum_parts:
                strata.append({
                    "upper_limit": sub_upper,
                    "lower_limit": sub_lower,
                    "parts": stratum_parts,
                })
                SUA_FILTER_STATS["standalone SUBTR (mixed-ref)"][designator] += 1

    return {
        "designator": designator,
        "name": name,
        "sua_type": sua_type,
        "upper_limit": upper_limit,
        "lower_limit": lower_limit,
        "min_alt_limit": min_alt_limit,
        "max_alt_limit": max_alt_limit,
        "conditional_exclusion": conditional_exclusion,
        "time_in_advance": time_in_advance,
        "traffic_allowed": "",
        "admin_area": admin_area,
        "city": saa_city,
        "military": military,
        "activity": activity,
        "status": status,
        "working_hours": working_hours,
        "compliant_icao": compliant_icao,
        "legal_note": legal_note,
        "strata": strata,
        "circle_info": result_circle_info,
    }


def _parse_radio_channels(root, gml_id_map):
    """Parse RadioCommunicationChannel elements.

    Returns a dict mapping gml:id -> {mode, tx_freq, rx_freq}.
    """
    channels = {}
    for rcc in root.iter(f"{{{AIXM}}}RadioCommunicationChannel"):
        gid = rcc.get(f"{{{GML}}}id", "")
        ts = rcc.find(f".//{{{AIXM}}}RadioCommunicationChannelTimeSlice")
        if ts is None:
            continue
        mode = _text(ts, f"{{{AIXM}}}mode")
        tx = _text(ts, f"{{{AIXM}}}frequencyTransmission")
        rx = _text(ts, f"{{{AIXM}}}frequencyReception")
        # AIXM declares MHz in `uom` on these elements; the FAA
        # corpus is consistently MHz, so we keep the source text
        # as-is and don't read the uom.
        channels[gid] = {
            "mode": mode,
            "tx_freq": tx,
            "rx_freq": rx,
        }
    return channels


def _parse_units(root):
    """Parse Unit elements.

    Returns a dict mapping gml:id -> {name, type, military}.
    """
    units = {}
    for unit in root.iter(f"{{{AIXM}}}Unit"):
        gid = unit.get(f"{{{GML}}}id", "")
        ts = unit.find(f".//{{{AIXM}}}UnitTimeSlice")
        if ts is None:
            continue
        units[gid] = {
            "name": _text(ts, f"{{{AIXM}}}name"),
            "type": _text(ts, f"{{{AIXM}}}type"),
            "military": _text(ts, f"{{{AIXM}}}military"),
        }
    return units


def _resolve_href(href, lookup):
    """Resolve an xlink:href like '#Unit1' against a gml:id lookup dict."""
    if not href:
        return None
    key = href.lstrip("#")
    return lookup.get(key)


def _parse_services(root, units, channels):
    """Parse AirTrafficControlService and InformationService elements.

    Returns a list of service dicts with unit and channel info resolved.
    """
    services = []

    for svc in root.iter(f"{{{AIXM}}}AirTrafficControlService"):
        ts = svc.find(f".//{{{AIXM}}}AirTrafficControlServiceTimeSlice")
        if ts is None:
            continue
        stype = _text(ts, f"{{{AIXM}}}type")
        sp = ts.find(f"{{{AIXM}}}serviceProvider")
        sp_href = sp.get(f"{{{XLINK}}}href", "") if sp is not None else ""
        unit = _resolve_href(sp_href, units)
        services.append({
            "service_type": stype or "ACS",
            "unit_name": unit["name"] if unit else "",
            "unit_type": unit["type"] if unit else "",
        })

    for svc in root.iter(f"{{{AIXM}}}InformationService"):
        ts = svc.find(f".//{{{AIXM}}}InformationServiceTimeSlice")
        if ts is None:
            continue
        stype = _text(ts, f"{{{AIXM}}}type")
        sp = ts.find(f"{{{AIXM}}}serviceProvider")
        sp_href = sp.get(f"{{{XLINK}}}href", "") if sp is not None else ""
        unit = _resolve_href(sp_href, units)
        services.append({
            "service_type": stype or "INFO",
            "unit_name": unit["name"] if unit else "",
            "unit_type": unit["type"] if unit else "",
        })

    return services


def _parse_timesheets(root):
    """Parse Timesheet entries from AirspaceUsage elements.

    Returns a list of schedule dicts with day, start/end times, timezone,
    and DST flag.
    """
    schedules = []

    for usage_ts in root.iter(f"{{{AIXM}}}AirspaceUsageTimeSlice"):
        # DST flag from the AirspaceUsage extension
        dst_flag = ""
        for ext in usage_ts.findall(f"{{{AIXM}}}extension"):
            dst = ext.find(f".//{{{SAA}}}daylightSavings")
            if dst is not None and dst.text:
                dst_flag = dst.text.strip()

        for timesheet in usage_ts.iter(f"{{{AIXM}}}Timesheet"):
            day = _text(timesheet, f"{{{AIXM}}}day")
            day_til = _text(timesheet, f"{{{AIXM}}}dayTil")
            start_time = _text(timesheet, f"{{{AIXM}}}startTime")
            end_time = _text(timesheet, f"{{{AIXM}}}endTime")
            # Event-relative activations: a Timesheet may name a
            # solar event (SR=sunrise, SS=sunset) instead of (or in
            # addition to) a clock time, with an optional offset in
            # minutes ("60 min before sunrise", etc.). The render
            # path joins event + offset into "SR-60" / "SS+60" etc.
            start_event = _text(timesheet, f"{{{AIXM}}}startEvent")
            end_event = _text(timesheet, f"{{{AIXM}}}endEvent")
            start_event_offset = _text(
                timesheet, f"{{{AIXM}}}startTimeRelativeEvent")
            end_event_offset = _text(
                timesheet, f"{{{AIXM}}}endTimeRelativeEvent")
            time_ref = _text(timesheet, f"{{{AIXM}}}timeReference")

            # UTC offset from TimesheetExtension
            time_offset = ""
            for ext in timesheet.findall(f"{{{AIXM}}}extension"):
                offset_el = ext.find(f".//{{{SAA}}}timeOffset")
                if offset_el is not None and offset_el.text:
                    time_offset = offset_el.text.strip()

            schedules.append({
                "day": day,
                "day_til": day_til,
                "start_time": start_time,
                "end_time": end_time,
                "start_event": start_event,
                "end_event": end_event,
                "start_event_offset": start_event_offset,
                "end_event_offset": end_event_offset,
                "time_ref": time_ref,
                "time_offset": time_offset,
                "dst_flag": dst_flag,
            })

    return schedules


def _parse_freq_allocations(root, channels):
    """Resolve per-airspace channel allocations.

    Returns `(by_airspace, fallback)` where:
      `by_airspace` — dict {airspace_gml_id → [freq_dict, ...]} from
                      `RadioCommunicationChannelAllocation` blocks.
                      Each freq_dict carries `comm_allowed` and
                      `charted` flags from the allocation.
      `fallback`    — list of freq_dicts for channels that no
                      allocation references; legacy "file-scope"
                      channels that fall back to all SUAs.
    """
    # Per-airspace mapping: airspace gml:id → list of freq dicts.
    # `RadioCommunicationChannelAllocation` blocks pin each channel
    # to a specific airspace via `<associatedAirspace xlink:href>`,
    # along with `communicationAllowed` (CIVIL/MIL) and `charted`
    # (YES/NO) flags. In multi-airspace files (e.g. R-4812) this is
    # the only correct way to determine which channels belong to
    # which airspace.
    by_airspace = {}      # gml_id → [freq_dict, ...]
    seen_per_airspace = {}  # gml_id → set of channel hrefs seen
    fallback = []         # channels with no allocation block at all

    for alloc in root.iter(
            f"{{{SAA}}}RadioCommunicationChannelAllocation"):
        airspace_ref = alloc.find(f"{{{SAA}}}associatedAirspace")
        if airspace_ref is None:
            continue
        airspace_href = airspace_ref.get(f"{{{XLINK}}}href", "")
        airspace_gid = airspace_href.lstrip("#")
        if not airspace_gid:
            continue
        details = alloc.find(f"{{{SAA}}}allocatedChannelDetails")
        if details is None:
            continue
        for sch in details.iter(f"{{{SAA}}}SaaRadioCommunicationChannel"):
            comm = _text(sch, f"{{{SAA}}}communicationAllowed")
            charted = _text(sch, f"{{{SAA}}}charted")
            sectors = _text(sch, f"{{{SAA}}}sectors")
            ch_ref = sch.find(f"{{{SAA}}}associatedChannel")
            if ch_ref is None:
                continue
            ch_href = ch_ref.get(f"{{{XLINK}}}href", "")
            ch = _resolve_href(ch_href, channels)
            if ch is None:
                continue
            seen = seen_per_airspace.setdefault(airspace_gid, set())
            if ch_href in seen:
                continue
            seen.add(ch_href)
            entry = ch.copy()
            entry["comm_allowed"] = comm
            entry["charted"] = charted
            entry["sectors"] = sectors
            by_airspace.setdefault(airspace_gid, []).append(entry)

    # Channels not bound by any allocation: keep around for SUAs
    # that have no per-airspace allocation but still reference
    # channels via InformationService.radioCommunication. These
    # are file-scope rather than airspace-scope.
    referenced = set()
    for href in (e.get(f"{{{XLINK}}}href", "")
                 for alloc in root.iter(
                     f"{{{SAA}}}RadioCommunicationChannelAllocation")
                 for e in alloc.iter(f"{{{SAA}}}associatedChannel")):
        referenced.add(href.lstrip("#"))
    for gid, ch in channels.items():
        if gid not in referenced:
            entry = ch.copy()
            entry["comm_allowed"] = ""
            entry["charted"] = ""
            entry["sectors"] = ""
            fallback.append(entry)

    return by_airspace, fallback


def parse_aixm_sua(xml_data):
    """Parse AIXM 5.0 SUA XML data.

    xml_data is bytes or a file-like object. A single file may contain
    multiple Airspace elements.
    Returns a list of parsed airspace dicts (may be empty).

    The AIXM document has sibling hasMember elements at the root level:
    OrganisationAuthority, Airspace, Unit (controlling/ARTCC), AirspaceUsage,
    RadioCommunicationChannel, AirTrafficControlService, InformationService.
    """
    root = ET.fromstring(xml_data)

    # Extract controlling authority from OrganisationAuthority elements
    controlling_auth = ""
    for oa_ts in root.iter(f"{{{AIXM}}}OrganisationAuthorityTimeSlice"):
        oa_name = _text(oa_ts, f"{{{AIXM}}}name")
        oa_type = _text(oa_ts, f"{{{AIXM}}}type")
        if oa_type == "NTL_AUTH" and oa_name:
            controlling_auth = oa_name
            break

    # Extract metadata from Unit elements (military status, ICAO compliance)
    military = ""
    compliant_icao = ""
    units = _parse_units(root)
    for unit in units.values():
        if unit["type"] == "MILOPS" and not military:
            military = unit["military"]
        elif unit["type"] == "ARTCC" and not compliant_icao:
            compliant_icao = _text(root, f"{{{AIXM}}}compliantICAO")

    # Extract metadata from AirspaceUsage (activity, status, working
    # hours, traffic permitted). `trafficAllowed` lives nested in
    # AirspaceLayerUsage; values observed are "ALL" (open to civil
    # + military) and "MIL" (military-only inside the SUA when
    # active).
    activity = ""
    status = ""
    working_hours = ""
    traffic_allowed = ""
    for usage_ts in root.iter(f"{{{AIXM}}}AirspaceUsageTimeSlice"):
        if not activity:
            activity = _text(usage_ts, f"{{{AIXM}}}activity")
        if not status:
            status = _text(usage_ts, f"{{{AIXM}}}statusActivation")
        if not working_hours:
            working_hours = _text(usage_ts, f"{{{AIXM}}}workingHours")
        if not traffic_allowed:
            traffic_allowed = _text(usage_ts, f"{{{AIXM}}}trafficAllowed")

    # Parse radio channels, services, and timesheets
    channels = _parse_radio_channels(root, {})
    services = _parse_services(root, units, channels)
    timesheets = _parse_timesheets(root)
    freqs_by_airspace, freqs_fallback = _parse_freq_allocations(root, channels)

    # A file's canonical Airspace is the one not referenced by any
    # <theAirspace xlink:href="#..."> within the file. Other <Airspace>
    # elements are neighbors inlined by reference (e.g. R-4812's file
    # inlines R-4804A and R-4810) and have their own canonical files.
    referenced_ids = set()
    for ref in root.iter(f"{{{AIXM}}}theAirspace"):
        href = ref.get(f"{{{XLINK}}}href", "")
        if href.startswith("#"):
            referenced_ids.add(href[1:])

    # Index all airspaces in the file by gml:id so cross-airspace SUBTR
    # references (contributorAirspace xlink) can be resolved.
    airspace_lookup = {
        a.get(f"{{{GML}}}id"): a
        for a in root.iter(f"{{{AIXM}}}Airspace")
        if a.get(f"{{{GML}}}id")
    }

    # Source-data assumption checks. Every entry asserts a value the
    # rest of the pipeline relies on; if a future data drop ever
    # introduces a different value, we want a loud warning instead
    # of silently mis-interpreting the source.
    for elem in root.iter(f"{{{AIXM}}}altitudeInterpretation"):
        # `BETWEEN` means lower ≤ altitude ≤ upper. Anything else
        # would require new combination logic.
        v = (elem.text or "").strip()
        if v and v != "BETWEEN":
            print(f"  WARN: unsupported altitudeInterpretation={v!r} "
                  f"— pipeline assumes BETWEEN", file=sys.stderr)
    for elem in root.iter(f"{{{SAA}}}saaType"):
        # Every Airspace in the SAA Subscriber File carries `SUA`.
        # ATCAA-typed airspaces ship in a separate FAA distribution
        # we don't ingest. Warn if we ever see a different value
        # so we know the data scope has changed.
        v = (elem.text or "").strip()
        if v and v != "SUA":
            print(f"  WARN: unsupported saa:saaType={v!r} "
                  f"— pipeline only ingests SUAs", file=sys.stderr)
    for elem in root.iter(f"{{{SAA}}}intermittent"):
        # Lives in TimesheetExtension. Every observed value is "NO";
        # if a future drop introduces "YES" we'd want to surface it
        # in the info-box (the NOTAM is sometimes time-stamped to
        # an event that may or may not occur).
        v = (elem.text or "").strip()
        if v and v != "NO":
            print(f"  WARN: unsupported saa:intermittent={v!r} "
                  f"— add display path", file=sys.stderr)
    for elem in root.iter(f"{{{AIXM}}}dependency"):
        # Inside `AirspaceVolumeDependency`. Only `HORZ_PROJECTION`
        # is observed; that's what `_collect_additive_shapes` handles
        # via the xlink-resolution path (R-4812).
        v = (elem.text or "").strip()
        if v and v != "HORZ_PROJECTION":
            print(f"  WARN: unsupported AirspaceVolumeDependency dependency="
                  f"{v!r} — pipeline only handles HORZ_PROJECTION",
                  file=sys.stderr)
    for elem in root.iter(f"{{{AIXM}}}Surface"):
        # Coordinates are stored as raw lon/lat with no projection
        # step. Anything other than CRS84 (WGS84 lon/lat) would put
        # geometry in the wrong place.
        v = (elem.get("srsName") or "").strip()
        if v and v.upper() != "URN:OGC:DEF:CRS:OGC:1.3:CRS84":
            print(f"  WARN: unsupported Surface srsName={v!r} "
                  f"— pipeline assumes WGS84 lon/lat (CRS84)",
                  file=sys.stderr)
    for tag in ("ArcByCenterPoint", "CircleByCenterPoint"):
        # `parse_ring_points` samples a single arc per element; the
        # multi-arc case would need a different traversal.
        for elem in root.iter(f"{{{GML}}}{tag}"):
            v = (elem.get("numArc") or "").strip()
            if v and v != "1":
                print(f"  WARN: gml:{tag} numArc={v!r} — pipeline "
                      f"assumes a single arc per element", file=sys.stderr)

    results = []
    for airspace in root.iter(f"{{{AIXM}}}Airspace"):
        if airspace.get(f"{{{GML}}}id") in referenced_ids:
            continue
        result = _parse_one_airspace(airspace, airspace_lookup)
        if result is not None:
            result["controlling_authority"] = controlling_auth
            result["military"] = result["military"] or military
            result["compliant_icao"] = result["compliant_icao"] or compliant_icao
            result["activity"] = result["activity"] or activity
            result["status"] = result["status"] or status
            result["working_hours"] = result["working_hours"] or working_hours
            result["traffic_allowed"] = (result.get("traffic_allowed") or
                                         traffic_allowed)
            # Per-airspace channel allocation: pull this airspace's
            # bound channels first; if none exist, fall back to any
            # file-scope channels (channels declared but never bound).
            ag = airspace.get(f"{{{GML}}}id", "")
            airspace_freqs = list(freqs_by_airspace.get(ag, []))
            if not airspace_freqs:
                airspace_freqs = list(freqs_fallback)
            result["freqs"] = airspace_freqs
            result["services"] = services
            result["schedules"] = timesheets
            results.append(result)
    return results


def build_sua(conn, aixm_zf):
    """Import SUA polygon boundaries from AIXM 5.0 XML files.

    aixm_zf is the outer AIXM ZIP. The XML files are nested:
    aixm5.0.zip / SaaSubscriberFile.zip / Saa_Sub_File.zip / *.xml
    """
    # Navigate nested ZIPs to reach the XML files
    mid_name = next(n for n in aixm_zf.namelist() if n.endswith('.zip'))
    mid_zf = zipfile.ZipFile(io.BytesIO(aixm_zf.read(mid_name)))
    inner_name = next(n for n in mid_zf.namelist() if n.endswith('.zip'))
    inner_zf = zipfile.ZipFile(io.BytesIO(mid_zf.read(inner_name)))

    xml_files = sorted(n for n in inner_zf.namelist() if n.endswith('.xml'))
    if not xml_files:
        print("  No XML files found in AIXM archive")
        return

    conn.execute("DROP TABLE IF EXISTS SUA_BASE")
    # MIN_ALT_LIMIT / MAX_ALT_LIMIT carry the AIXM minimumLimit /
    # maximumLimit pair from the BASE volume — terrain-clearance
    # constraints (typically "min N ft AGL") supplementing the MSL
    # band. Empty for SUAs that don't declare them (most).
    # CONDITIONAL_EXCLUSION = "YES"/"NO"/"" — flag from
    # saa:AirspaceExtension marking SUAs whose legal definition
    # contains a special exclusion clause.
    # TRAFFIC_ALLOWED = "ALL"/"MIL"/"" — from
    # AirspaceLayerUsage; "MIL" means civil aircraft are not
    # permitted inside the SUA when active.
    # TIME_IN_ADVANCE_HR — NOTAM lead time required before SUA
    # activation, in hours (raw source string, e.g. "6.0", "24.0").
    conn.execute("""
        CREATE TABLE SUA_BASE (
            SUA_ID INTEGER PRIMARY KEY,
            DESIGNATOR TEXT,
            NAME TEXT,
            SUA_TYPE TEXT,
            UPPER_LIMIT TEXT,
            LOWER_LIMIT TEXT,
            MIN_ALT_LIMIT TEXT,
            MAX_ALT_LIMIT TEXT,
            CONDITIONAL_EXCLUSION TEXT,
            TRAFFIC_ALLOWED TEXT,
            TIME_IN_ADVANCE_HR TEXT,
            CONTROLLING_AUTHORITY TEXT,
            ADMIN_AREA TEXT,
            CITY TEXT,
            MILITARY TEXT,
            ACTIVITY TEXT,
            STATUS TEXT,
            WORKING_HOURS TEXT,
            ICAO_COMPLIANT TEXT,
            LEGAL_NOTE TEXT
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_STRATUM")
    # `*_FT_VAL` is feet (or hundreds-of-feet for STD/FL); `*_FT_REF`
    # is one of MSL/SFC/STD/OTHER. Two altitudes may be compared
    # numerically only when their refs match.
    conn.execute("""
        CREATE TABLE SUA_STRATUM (
            STRATUM_ID INTEGER PRIMARY KEY,
            SUA_ID INTEGER,
            STRATUM_ORDER INTEGER,
            UPPER_LIMIT TEXT,
            LOWER_LIMIT TEXT,
            UPPER_FT_VAL INTEGER,
            UPPER_FT_REF TEXT,
            LOWER_FT_VAL INTEGER,
            LOWER_FT_REF TEXT
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_SHP")
    conn.execute("""
        CREATE TABLE SUA_SHP (
            SUA_ID INTEGER,
            STRATUM_ID INTEGER,
            PART_NUM INTEGER,
            IS_HOLE INTEGER,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_CIRCLE")
    conn.execute("""
        CREATE TABLE SUA_CIRCLE (
            SUA_ID INTEGER,
            STRATUM_ID INTEGER,
            PART_NUM INTEGER,
            CENTER_LON REAL,
            CENTER_LAT REAL,
            RADIUS_NM REAL
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_FREQ")
    # Frequencies stored as raw source text (matches the convention
    # used by NAV_BASE.FREQ, ILS_BASE.LOC_FREQ, FRQ.FREQ, etc.).
    # We never compare or do arithmetic on them in this app — only
    # display — so a string round-trips without any precision loss.
    # COMM_ALLOWED: "CIVIL"/"MIL"/"" (from the per-airspace
    # RadioCommunicationChannelAllocation block).
    # CHARTED: "YES"/"NO"/"" — whether the channel is published on
    # the chart for this airspace.
    conn.execute("""
        CREATE TABLE SUA_FREQ (
            SUA_ID INTEGER,
            FREQ_SEQ INTEGER,
            MODE TEXT,
            TX_FREQ TEXT,
            RX_FREQ TEXT,
            COMM_ALLOWED TEXT,
            CHARTED TEXT,
            SECTORS TEXT,
            PRIMARY KEY (SUA_ID, FREQ_SEQ)
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_SCHEDULE")
    # DAY_TIL: end day of an inclusive day-of-week range (e.g. day=MON,
    # day_til=TUE means "Mon through Tue"). Empty when a single day.
    # START_EVENT/END_EVENT: solar event token (SR/SS) when the
    # activation is anchored to sunrise/sunset rather than (or in
    # addition to) a clock time. *_EVENT_OFFSET: minutes offset
    # relative to that event (e.g. -60 = "60 min before sunrise").
    conn.execute("""
        CREATE TABLE SUA_SCHEDULE (
            SUA_ID INTEGER,
            SCHED_SEQ INTEGER,
            DAY_OF_WEEK TEXT,
            DAY_TIL TEXT,
            START_TIME TEXT,
            END_TIME TEXT,
            START_EVENT TEXT,
            END_EVENT TEXT,
            START_EVENT_OFFSET TEXT,
            END_EVENT_OFFSET TEXT,
            TIME_REF TEXT,
            TIME_OFFSET TEXT,
            DST_FLAG TEXT,
            PRIMARY KEY (SUA_ID, SCHED_SEQ)
        )
    """)

    conn.execute("DROP TABLE IF EXISTS SUA_SERVICE")
    conn.execute("""
        CREATE TABLE SUA_SERVICE (
            SUA_ID INTEGER,
            SVC_SEQ INTEGER,
            SERVICE_TYPE TEXT,
            UNIT_NAME TEXT,
            UNIT_TYPE TEXT,
            PRIMARY KEY (SUA_ID, SVC_SEQ)
        )
    """)

    epsilon = 0.0001
    total_before = 0
    total_after = 0
    base_rows = []
    stratum_rows = []
    shp_rows = []
    circle_rows = []
    freq_rows = []
    schedule_rows = []
    service_rows = []
    skipped = 0
    next_stratum_id = 1

    for xml_name in xml_files:
        results = parse_aixm_sua(inner_zf.read(xml_name))
        if not results:
            skipped += 1
            continue

        for result in results:
            sua_id = len(base_rows) + 1
            base_rows.append((
                sua_id,
                result["designator"],
                result["name"],
                result["sua_type"],
                result["upper_limit"],
                result["lower_limit"],
                result["min_alt_limit"],
                result["max_alt_limit"],
                result["conditional_exclusion"],
                result["traffic_allowed"],
                result["time_in_advance"],
                result["controlling_authority"],
                result["admin_area"],
                result["city"],
                result["military"],
                result["activity"],
                result["status"],
                result["working_hours"],
                result["compliant_icao"],
                result["legal_note"],
            ))

            # A pure-circle SUA: one BASE stratum, one circle part.
            ci = result.get("circle_info")
            circle_stratum_id = None
            if ci is not None:
                r = ci["radius"]
                uom = ci["uom"]
                if uom == "KM":
                    r /= 1.852
                elif uom == "M":
                    r /= 1852.0
                elif uom == "FT":
                    r *= 0.3048 / 1852.0
                elif uom == "MI":
                    r *= 1609.344 / 1852.0
                elif uom != "NM":
                    r /= 1.852  # assume KM if unknown
                # Allocate the BASE stratum upfront so the circle part
                # can reference it. Matching band will find the same id.
                circle_stratum_id = next_stratum_id
                next_stratum_id += 1
                _ulim_v, _ulim_r = parse_altitude(result["upper_limit"])
                _llim_v, _llim_r = parse_altitude(result["lower_limit"])
                stratum_rows.append((
                    circle_stratum_id, sua_id, 0,
                    result["upper_limit"], result["lower_limit"],
                    _ulim_v, _ulim_r, _llim_v, _llim_r,
                ))
                circle_rows.append((sua_id, circle_stratum_id, 0,
                                    ci["lon"], ci["lat"], r))

            for stratum_order, stratum in enumerate(result["strata"]):
                if stratum_order == 0 and circle_stratum_id is not None:
                    # BASE stratum already registered as the circle's parent.
                    stratum_id = circle_stratum_id
                else:
                    stratum_id = next_stratum_id
                    next_stratum_id += 1
                    _ulim_v, _ulim_r = parse_altitude(stratum["upper_limit"])
                    _llim_v, _llim_r = parse_altitude(stratum["lower_limit"])
                    stratum_rows.append((
                        stratum_id, sua_id, stratum_order,
                        stratum["upper_limit"], stratum["lower_limit"],
                        _ulim_v, _ulim_r, _llim_v, _llim_r,
                    ))
                part_num = 0
                for ring, is_hole in stratum["parts"]:
                    for copy in handle_antimeridian(ring):
                        total_before += len(copy)
                        _n_pre_simp = len(copy)
                        simplified = simplify_ring(copy, epsilon)
                        if len(simplified) != _n_pre_simp:
                            SUA_FILTER_STATS["simplify_ring (Douglas-Peucker)"][result["designator"]] += (_n_pre_simp - len(simplified))
                        # Drop degenerate parts (fewer than 3 distinct
                        # vertices) — typically a SUBTR zone that
                        # simplified down to a filament.
                        unique_pts = len({(lon, lat) for lon, lat in simplified})
                        if unique_pts < 3:
                            SUA_FILTER_STATS["unique_pts<3 (degenerate part dropped)"][result["designator"]] += 1
                            continue
                        total_after += len(simplified)
                        for point_seq, (lon, lat) in enumerate(simplified):
                            shp_rows.append((sua_id, stratum_id, part_num,
                                             1 if is_hole else 0,
                                             point_seq, lon, lat))
                        part_num += 1

            # Frequencies
            for seq, freq in enumerate(result.get("freqs", []), 1):
                freq_rows.append((
                    sua_id, seq,
                    freq["mode"],
                    freq["tx_freq"],
                    freq["rx_freq"],
                    freq.get("comm_allowed", ""),
                    freq.get("charted", ""),
                    freq.get("sectors", ""),
                ))

            # Schedules
            for seq, sched in enumerate(result.get("schedules", []), 1):
                schedule_rows.append((
                    sua_id, seq,
                    sched["day"],
                    sched["day_til"],
                    sched["start_time"],
                    sched["end_time"],
                    sched["start_event"],
                    sched["end_event"],
                    sched["start_event_offset"],
                    sched["end_event_offset"],
                    sched["time_ref"],
                    sched["time_offset"],
                    sched["dst_flag"],
                ))

            # Services
            for seq, svc in enumerate(result.get("services", []), 1):
                service_rows.append((
                    sua_id, seq,
                    svc["service_type"],
                    svc["unit_name"],
                    svc["unit_type"],
                ))

    conn.executemany(
        "INSERT INTO SUA_BASE VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        base_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_STRATUM VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        stratum_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_SHP VALUES (?, ?, ?, ?, ?, ?, ?)",
        shp_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_CIRCLE VALUES (?, ?, ?, ?, ?, ?)",
        circle_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_FREQ VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        freq_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_SCHEDULE VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        schedule_rows,
    )
    conn.executemany(
        "INSERT INTO SUA_SERVICE VALUES (?, ?, ?, ?, ?)",
        service_rows,
    )

    print(f"  Parsed {len(xml_files)} files, {skipped} skipped")
    print(f"  SUA_BASE: {len(base_rows)} airspaces")
    print(f"  SUA_STRATUM: {len(stratum_rows)} strata")
    print(f"  SUA_SHP: {len(shp_rows)} shape points")
    print(f"  SUA_CIRCLE: {len(circle_rows)} circle airspaces")
    print(f"  SUA_FREQ: {len(freq_rows)} frequency entries")
    print(f"  SUA_SCHEDULE: {len(schedule_rows)} schedule entries")
    print(f"  SUA_SERVICE: {len(service_rows)} service entries")
    if total_before > 0:
        print(f"  Simplified: {total_before} -> {total_after} points "
              f"({100 * total_after / total_before:.1f}%)")

    # R-tree on SUA-level bounding boxes (for listing all SUAs in view).
    conn.execute("DROP TABLE IF EXISTS SUA_BASE_RTREE")
    conn.execute("""
        CREATE VIRTUAL TABLE SUA_BASE_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO SUA_BASE_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT SUA_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM SUA_SHP
        GROUP BY SUA_ID
    """)

    # R-tree on stratum-level bounding boxes (for per-stratum picking).
    conn.execute("DROP TABLE IF EXISTS SUA_STRATUM_RTREE")
    conn.execute("""
        CREATE VIRTUAL TABLE SUA_STRATUM_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO SUA_STRATUM_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT STRATUM_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM SUA_SHP
        GROUP BY STRATUM_ID
    """)
    # Circle strata need their bbox too — computed from center+radius.
    # 1 NM ≈ 1/60 deg latitude; lon is approx scaled by cos(lat).
    for row in conn.execute(
            "SELECT s.STRATUM_ID, c.CENTER_LON, c.CENTER_LAT, c.RADIUS_NM "
            "FROM SUA_STRATUM s JOIN SUA_CIRCLE c ON c.STRATUM_ID = s.STRATUM_ID "
            "WHERE s.STRATUM_ID NOT IN (SELECT id FROM SUA_STRATUM_RTREE)"
            ).fetchall():
        sid, lon, lat, rad_nm = row
        dlat = rad_nm / 60.0
        dlon = rad_nm / (60.0 * max(0.01, math.cos(math.radians(lat))))
        conn.execute(
            "INSERT INTO SUA_STRATUM_RTREE (id, min_lon, max_lon, min_lat, max_lat) "
            "VALUES (?, ?, ?, ?, ?)",
            (sid, lon - dlon, lon + dlon, lat - dlat, lat + dlat))

    conn.execute("CREATE INDEX idx_sua_shp ON SUA_SHP(SUA_ID)")
    conn.execute("CREATE INDEX idx_sua_shp_stratum ON SUA_SHP(STRATUM_ID)")
    conn.execute("CREATE INDEX idx_sua_stratum_sua ON SUA_STRATUM(SUA_ID)")
    conn.execute("CREATE INDEX idx_sua_circle_stratum ON SUA_CIRCLE(STRATUM_ID)")
    conn.execute("CREATE INDEX idx_sua_freq ON SUA_FREQ(SUA_ID)")
    conn.execute("CREATE INDEX idx_sua_schedule ON SUA_SCHEDULE(SUA_ID)")
    conn.execute("CREATE INDEX idx_sua_service ON SUA_SERVICE(SUA_ID)")

    # Subdivided segments for rendering (tighter R-tree bboxes).
    # Keyed by stratum so the renderer can style per-stratum later.
    # Circles are not subdivided — they use SUA_CIRCLE directly.
    conn.execute("DROP TABLE IF EXISTS SUA_SEG")
    conn.execute("""
        CREATE TABLE SUA_SEG (
            SEG_ID INTEGER,
            STRATUM_ID INTEGER,
            SUA_ID INTEGER,
            SUA_TYPE TEXT,
            UPPER_FT_VAL INTEGER,
            UPPER_FT_REF TEXT,
            LOWER_FT_VAL INTEGER,
            LOWER_FT_REF TEXT,
            POINT_SEQ INTEGER,
            LON_DECIMAL REAL,
            LAT_DECIMAL REAL
        )
    """)

    # Build lookup from (STRATUM_ID, PART_NUM) to ring points.
    sua_rings = {}
    for row in conn.execute(
            "SELECT STRATUM_ID, PART_NUM, LON_DECIMAL, LAT_DECIMAL "
            "FROM SUA_SHP ORDER BY STRATUM_ID, PART_NUM, POINT_SEQ"):
        sua_rings.setdefault((row[0], row[1]), []).append((row[2], row[3]))

    # Stratum metadata (sua_id, upper_ft_val, upper_ft_ref,
    # lower_ft_val, lower_ft_ref).
    stratum_meta = {}
    for row in conn.execute(
            "SELECT STRATUM_ID, SUA_ID, UPPER_FT_VAL, UPPER_FT_REF, "
            "LOWER_FT_VAL, LOWER_FT_REF FROM SUA_STRATUM"):
        stratum_meta[row[0]] = (row[1], row[2], row[3], row[4], row[5])

    # SUA type lookup.
    sua_types = {}
    for sid, stype in conn.execute("SELECT SUA_ID, SUA_TYPE FROM SUA_BASE"):
        sua_types[sid] = stype

    # Circle parts are rendered by SUA_CIRCLE, not as polyline segments.
    circle_parts = set()
    for row in conn.execute("SELECT STRATUM_ID, PART_NUM FROM SUA_CIRCLE"):
        circle_parts.add((row[0], row[1]))

    seg_data = []
    seg_id = 0
    for (stratum_id, part_num), ring in sorted(sua_rings.items()):
        if (stratum_id, part_num) in circle_parts:
            continue
        meta = stratum_meta.get(stratum_id)
        if meta is None:
            continue
        sid, upper_v, upper_r, lower_v, lower_r = meta
        stype = sua_types.get(sid, "")
        for chunk in subdivide_ring(ring):
            seg_id += 1
            for point_seq, (lon, lat) in enumerate(chunk):
                seg_data.append((seg_id, stratum_id, sid, stype,
                                 upper_v, upper_r, lower_v, lower_r,
                                 point_seq, lon, lat))

    conn.executemany(
        "INSERT INTO SUA_SEG VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", seg_data)
    print(f"  SUA_SEG: {seg_id} segments, {len(seg_data)} points")

    conn.execute("DROP TABLE IF EXISTS SUA_SEG_RTREE")
    conn.execute("""
        CREATE VIRTUAL TABLE SUA_SEG_RTREE USING rtree(
            id, min_lon, max_lon, min_lat, max_lat
        )
    """)
    conn.execute("""
        INSERT INTO SUA_SEG_RTREE (id, min_lon, max_lon, min_lat, max_lat)
        SELECT SEG_ID,
               MIN(LON_DECIMAL), MAX(LON_DECIMAL),
               MIN(LAT_DECIMAL), MAX(LAT_DECIMAL)
        FROM SUA_SEG
        GROUP BY SEG_ID
    """)

    conn.execute("CREATE INDEX idx_sua_seg ON SUA_SEG(SEG_ID)")

    write_aixm_meta(conn, aixm_zf)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <aixm.zip> <output.db>")
        sys.exit(1)
    aixm_zip_path, db_path = sys.argv[1], sys.argv[2]

    conn = open_output_db(db_path)
    print("Importing SUA from AIXM 5.0...")
    with zipfile.ZipFile(aixm_zip_path) as aixm_zf:
        build_sua(conn, aixm_zf)
    conn.commit()
    conn.close()


def write_aixm_meta(conn, aixm_zf):
    """The SUA AIXM bundle ships in the same FAA NASR subscription as
    the CSV. The FAA repackages it for each NASR cycle but doesn't bump
    inner-file timestamps when the SUA data hasn't actually changed, so
    those timestamps lag the cycle. Inherit effective/expires from the
    nasr META row when present (build_all runs build_nasr first); fall
    back to the inner ZIP's build date for standalone re-runs."""
    eff_iso, expires_iso = read_meta(conn, "nasr")

    if eff_iso is None:
        inner = next((n for n in aixm_zf.namelist()
                      if n.endswith('SaaSubscriberFile.zip')), None)
        if inner is not None:
            info = aixm_zf.getinfo(inner)
            try:
                eff_iso = datetime.date(*info.date_time[:3]).isoformat()
            except ValueError:
                eff_iso = None
        if eff_iso is not None:
            expires_iso = (datetime.date.fromisoformat(eff_iso)
                           + datetime.timedelta(days=28)).isoformat()

    info_text = "SUA AIXM 5.0"
    if eff_iso is not None:
        eff = datetime.date.fromisoformat(eff_iso)
        info_text = f"SUA AIXM 5.0 {eff.strftime('%d %b %Y')}"

    write_meta(conn, "aixm",
               effective=eff_iso, expires=expires_iso, info=info_text)


if __name__ == "__main__":
    main()
