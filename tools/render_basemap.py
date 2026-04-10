#!/usr/bin/env python3
"""
Render a minimal two-color basemap from Natural Earth data as z/x/y PNG tiles.

Usage:
    python3 render_basemap.py /path/to/natural_earth_vector.gpkg.zip tiles/ [--zoom 0-7]

Produces tiles/{z}/{x}/{y}.png compatible with nasrbrowse's tile_renderer.

On first run, reprojects the source GeoPackage from EPSG:4326 to EPSG:3857
and saves it alongside the original as *_3857.gpkg. Subsequent runs reuse
the preprocessed file automatically.
"""

import argparse
import math
import multiprocessing
import os
import sys
import time

import fiona
from fiona.crs import CRS
from PIL import Image, ImageDraw, ImageFont
from pyproj import Transformer
from shapely import STRtree
from shapely.geometry import box, mapping, shape
from shapely.ops import transform as shapely_transform

# ---------------------------------------------------------------------------
# Style configuration
# ---------------------------------------------------------------------------

TILE_SIZE = 256

# Colors (RGBA)
COLOR_OCEAN      = (200, 220, 240, 255)   # light blue
COLOR_LAND       = (235, 235, 230, 255)   # light warm gray
COLOR_LAKE       = (200, 220, 240, 255)   # same as ocean
COLOR_URBAN      = (220, 218, 213, 255)   # slightly darker gray
COLOR_RIVER      = (170, 200, 225, 255)   # blue-gray
COLOR_ROAD       = (190, 185, 180, 255)   # warm gray
COLOR_RAILROAD   = (175, 170, 165, 255)   # darker warm gray
COLOR_BORDER_0   = (140, 130, 130, 255)   # country borders — dark gray
COLOR_BORDER_1   = (175, 170, 165, 255)   # state borders — lighter
COLOR_LABEL      = (80, 80, 80, 255)      # dark gray text
COLOR_LABEL_HALO = (255, 255, 255, 200)   # white halo

# Line widths per zoom level (zoom -> width in pixels)
LINE_WIDTHS = {
    'river':    {0: 0.5, 3: 0.8, 5: 1.0, 7: 1.5},
    'road':     {0: 0.5, 4: 0.8, 6: 1.0, 7: 1.5},
    'railroad': {0: 0.5, 5: 0.8, 7: 1.0},
    'border_0': {0: 1.0, 4: 1.5, 7: 2.0},
    'border_1': {0: 0.5, 3: 0.8, 5: 1.0, 7: 1.5},
}

# scalerank thresholds: only show features with scalerank <= threshold at zoom z
SCALERANK_THRESHOLDS = {
    'river':    {0: 2, 1: 3, 2: 4, 3: 5, 4: 6, 5: 7, 6: 8, 7: 9, 8: 12},
    'road':     {0: 3, 1: 3, 2: 3, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7, 8: 12},
    'railroad': {0: 4, 1: 4, 2: 4, 3: 4, 4: 5, 5: 6, 6: 7, 7: 8, 8: 12},
    'lake':     {0: 1, 1: 2, 2: 3, 3: 4, 4: 5, 5: 6, 6: 8, 7: 10, 8: 12},
    'urban':    {0: 0, 1: 0, 2: 0, 3: 1, 4: 2, 5: 3, 6: 5, 7: 7, 8: 12},
    'places':   {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7, 8: 8},
}

# Label font sizes by zoom
LABEL_FONT_SIZES = {
    'country': {0: 0, 5: 12, 6: 14, 7: 16},
    'state':   {0: 0, 6: 10, 7: 12},
    'city':    {0: 0, 7: 10, 8: 11},
}

# Natural Earth layers used — multi-scale: pick resolution by zoom
LAYER_SETS = {
    'low':  {  # z0-2: use 110m
        'land':     'ne_110m_land',
        'ocean':    'ne_110m_ocean',
        'lakes':    'ne_110m_lakes',
        'rivers':   'ne_110m_rivers_lake_centerlines',
        'border_0': 'ne_110m_admin_0_boundary_lines_land',
        'border_1': 'ne_110m_admin_1_states_provinces_lines',
        'roads':    None,
        'railroads': None,
        'urban':    None,
    },
    'mid':  {  # z3-5: use 50m
        'land':     'ne_50m_land',
        'ocean':    'ne_50m_ocean',
        'lakes':    'ne_50m_lakes',
        'rivers':   'ne_50m_rivers_lake_centerlines',
        'border_0': 'ne_50m_admin_0_boundary_lines_land',
        'border_1': 'ne_50m_admin_1_states_provinces_lines',
        'roads':    'ne_10m_roads',  # no 50m roads
        'railroads': None,
        'urban':    'ne_50m_urban_areas',
    },
    'high': {  # z6-8: use 10m
        'land':     'ne_10m_land',
        'ocean':    'ne_10m_ocean',
        'lakes':    'ne_10m_lakes',
        'rivers':   'ne_10m_rivers_lake_centerlines',
        'border_0': 'ne_10m_admin_0_boundary_lines_land',
        'border_1': 'ne_10m_admin_1_states_provinces_lines',
        'roads':    'ne_10m_roads',
        'railroads':'ne_10m_railroads',
        'urban':    'ne_10m_urban_areas',
    },
}

# Label layers are always 10m (only centroids, small dataset)
LABEL_LAYERS = {
    'countries': 'ne_10m_admin_0_countries',
    'states':    'ne_10m_admin_1_states_provinces',
    'places':    'ne_10m_populated_places_simple',
}

# All unique layer names we need to reproject
ALL_LAYERS = set()
for layer_set in LAYER_SETS.values():
    for name in layer_set.values():
        if name is not None:
            ALL_LAYERS.add(name)
for name in LABEL_LAYERS.values():
    ALL_LAYERS.add(name)

# Web Mercator limits: ±85.06° latitude
_MERCATOR_MAX_LAT = 85.06


# ---------------------------------------------------------------------------
# Source file resolution: .zip or .gpkg
# ---------------------------------------------------------------------------

def resolve_source(path):
    """Resolve the input path to a Fiona-readable source path and a
    filesystem path for mtime checks.

    Accepts:
      - A .gpkg file path
      - A .zip file containing a .gpkg (read directly via GDAL /vsizip/)

    Returns (fiona_path, fs_path) where fiona_path can be opened by Fiona
    and fs_path is the real file on disk for mtime comparisons.
    """
    if path.lower().endswith('.zip'):
        # Find the .gpkg inside the zip
        import zipfile
        with zipfile.ZipFile(path) as zf:
            gpkg_names = [n for n in zf.namelist()
                          if n.lower().endswith('.gpkg')]
            if not gpkg_names:
                print(f"Error: no .gpkg found inside {path}", file=sys.stderr)
                sys.exit(1)
            gpkg_name = gpkg_names[0]
        fiona_path = f"zip://{os.path.abspath(path)}!{gpkg_name}"
        return fiona_path, path
    else:
        return path, path


# ---------------------------------------------------------------------------
# Preprocessing: reproject GeoPackage to EPSG:3857
# ---------------------------------------------------------------------------

def get_3857_path(fs_path):
    """Derive the path for the 3857-projected GeoPackage, placed alongside
    the source file (whether .zip or .gpkg)."""
    # Strip all extensions (handles .gpkg.zip, .gpkg, etc.)
    base = fs_path
    while True:
        base, ext = os.path.splitext(base)
        if not ext:
            break
    return base + '_3857.gpkg'


def is_already_3857(gpkg_path):
    """Check if a GeoPackage is already in EPSG:3857 by inspecting one layer."""
    try:
        layers = fiona.listlayers(gpkg_path)
        if not layers:
            return False
        for layer in ALL_LAYERS:
            if layer in layers:
                with fiona.open(gpkg_path, layer=layer) as src:
                    crs = src.crs
                    return crs and CRS(crs).to_epsg() == 3857
        return False
    except Exception:
        return False


def needs_preprocessing(fs_path, target_path):
    """Check whether we need to create or update the 3857 GeoPackage."""
    if not os.path.isfile(target_path):
        return True
    # Rebuild if source is newer than target
    if os.path.getmtime(fs_path) > os.path.getmtime(target_path):
        return True
    # Verify target is actually 3857
    if not is_already_3857(target_path):
        return True
    return False


def preprocess_to_3857(fiona_source, target_path):
    """Reproject all needed layers from source (4326) to target (3857) GeoPackage.
    fiona_source can be a .gpkg path or a zip:// URI."""
    transformer = Transformer.from_crs("EPSG:4326", "EPSG:3857", always_xy=True)
    to_3857 = transformer.transform

    # Clip box to avoid Mercator singularity at poles
    clip_box = box(-180, -_MERCATOR_MAX_LAT, 180, _MERCATOR_MAX_LAT)

    source_layers = set(fiona.listlayers(fiona_source))
    target_crs = CRS.from_epsg(3857)

    # Remove stale target if it exists
    if os.path.isfile(target_path):
        os.remove(target_path)

    layers_to_convert = sorted(ALL_LAYERS & source_layers)
    print(f"Preprocessing: reprojecting {len(layers_to_convert)} layers "
          f"to EPSG:3857...")

    for i, layer_name in enumerate(layers_to_convert, 1):
        t0 = time.time()
        count = 0

        with fiona.open(fiona_source, layer=layer_name) as src:
            # Use "Unknown" geometry type to accept both Polygon and
            # MultiPolygon (Fiona is strict about type matching).
            out_schema = src.schema.copy()
            out_schema['geometry'] = 'Unknown'

            with fiona.open(target_path, 'w', driver='GPKG',
                            layer=layer_name, schema=out_schema,
                            crs=target_crs) as dst:
                for feat in src:
                    try:
                        geom = shape(feat['geometry'])
                        if geom.is_empty or not geom.is_valid:
                            continue

                        # Clip to Mercator-safe latitude range
                        clipped = geom.intersection(clip_box)
                        if clipped.is_empty:
                            continue

                        # Reproject to 3857
                        projected = shapely_transform(to_3857, clipped)
                        if projected.is_empty:
                            continue

                        feat_out = {
                            'geometry': mapping(projected),
                            'properties': feat['properties'],
                        }
                        dst.write(feat_out)
                        count += 1
                    except Exception:
                        continue

        elapsed = time.time() - t0
        print(f"  [{i}/{len(layers_to_convert)}] {layer_name}: "
              f"{count} features ({elapsed:.1f}s)")

    print(f"Saved: {target_path}")


def ensure_3857_gpkg(input_path):
    """Return path to a 3857-projected GeoPackage, creating it if needed.

    Accepts a .gpkg or .zip file. The 3857 GeoPackage is placed alongside
    the input file. Reads directly from zip archives without extracting.
    """
    fiona_source, fs_path = resolve_source(input_path)

    target_path = get_3857_path(fs_path)

    # If the target already exists and is up to date, use it
    if not needs_preprocessing(fs_path, target_path):
        print(f"Using existing EPSG:3857 GeoPackage: {target_path}")
        return target_path

    # Check if the source itself is already 3857 (e.g., user passed the
    # _3857.gpkg directly)
    if not input_path.lower().endswith('.zip') and is_already_3857(input_path):
        print(f"Source is already EPSG:3857: {input_path}")
        return input_path

    preprocess_to_3857(fiona_source, target_path)
    return target_path


# ---------------------------------------------------------------------------
# Tile math
# ---------------------------------------------------------------------------

def tile_bounds_3857(z, x, y):
    """Return (xmin, ymin, xmax, ymax) in EPSG:3857 for tile z/x/y."""
    n = 2 ** z
    xmin = (x / n - 0.5) * 2 * math.pi * 6378137
    xmax = ((x + 1) / n - 0.5) * 2 * math.pi * 6378137
    ymax = (0.5 - y / n) * 2 * math.pi * 6378137
    ymin = (0.5 - (y + 1) / n) * 2 * math.pi * 6378137
    return (xmin, ymin, xmax, ymax)


def lookup_by_zoom(table, zoom):
    """Look up a value from a {zoom: value} dict, using the highest key <= zoom."""
    result = None
    for z in sorted(table.keys()):
        if z <= zoom:
            result = table[z]
    return result


def get_line_width(kind, zoom):
    """Look up line width for a feature kind at a given zoom."""
    return lookup_by_zoom(LINE_WIDTHS.get(kind, {0: 1.0}), zoom) or 1.0


def get_scalerank_threshold(kind, zoom):
    """Max scalerank to show for a feature kind at a given zoom."""
    return lookup_by_zoom(SCALERANK_THRESHOLDS.get(kind, {}), zoom) or 12


def get_font_size(kind, zoom):
    """Look up font size for a label kind at a given zoom."""
    return lookup_by_zoom(LABEL_FONT_SIZES.get(kind, {}), zoom) or 0


def get_layer_set(zoom):
    """Pick the appropriate resolution layer set for a zoom level."""
    if zoom <= 2:
        return LAYER_SETS['low']
    elif zoom <= 5:
        return LAYER_SETS['mid']
    else:
        return LAYER_SETS['high']




# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------

def make_coord_transform(bbox_3857):
    """Return a function that maps (x_3857, y_3857) -> (px, py) in tile pixels."""
    xmin, ymin, xmax, ymax = bbox_3857
    sx = TILE_SIZE / (xmax - xmin)
    sy = TILE_SIZE / (ymax - ymin)

    def to_pixel(x, y, _z=None):
        px = (x - xmin) * sx
        py = (ymax - y) * sy  # y is flipped
        return (px, py)

    return to_pixel


def clip_to_pixels(geom, tile_box_3857, to_pixel):
    """Clip a 3857 geometry to the tile bbox and transform to pixel coords.

    Returns a Shapely geometry in pixel coordinates, or None if empty.
    """
    try:
        clipped = geom.intersection(tile_box_3857)
    except Exception:
        return None
    if clipped.is_empty:
        return None
    pixel_geom = shapely_transform(to_pixel, clipped)
    return pixel_geom


def draw_polygon(draw, geom, fill_color):
    """Draw a Shapely polygon (or multi-polygon) onto a PIL ImageDraw."""
    if geom is None or geom.is_empty:
        return
    polys = [geom] if geom.geom_type == 'Polygon' else (
        list(geom.geoms) if geom.geom_type == 'MultiPolygon' else [])
    for poly in polys:
        exterior = list(poly.exterior.coords)
        if len(exterior) < 3:
            continue
        draw.polygon(exterior, fill=fill_color)
        # Cut out holes
        for hole in poly.interiors:
            hole_coords = list(hole.coords)
            if len(hole_coords) >= 3:
                draw.polygon(hole_coords, fill=COLOR_OCEAN)


def draw_line(draw, geom, color, width):
    """Draw a Shapely line (or multi-line) onto a PIL ImageDraw."""
    if geom is None or geom.is_empty:
        return
    lines = []
    if geom.geom_type == 'LineString':
        lines = [geom]
    elif geom.geom_type == 'MultiLineString':
        lines = list(geom.geoms)
    elif geom.geom_type == 'GeometryCollection':
        for g in geom.geoms:
            if g.geom_type in ('LineString', 'MultiLineString'):
                draw_line(draw, g, color, width)
        return
    for line in lines:
        coords = list(line.coords)
        if len(coords) < 2:
            continue
        draw.line(coords, fill=color, width=max(1, int(round(width))))


def draw_label(draw, text, x, y, font, color=COLOR_LABEL, halo_color=COLOR_LABEL_HALO):
    """Draw text with a 1px halo for readability."""
    if not text:
        return
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            if dx == 0 and dy == 0:
                continue
            draw.text((x + dx, y + dy), text, fill=halo_color, font=font, anchor='mm')
    draw.text((x, y), text, fill=color, font=font, anchor='mm')


# ---------------------------------------------------------------------------
# Feature cache — load once, reuse across tiles at the same zoom
# ---------------------------------------------------------------------------

class FeatureCache:
    """Loads and caches features from an EPSG:3857 GeoPackage, providing
    bbox queries via Shapely's STRtree spatial index.

    Each layer is loaded once (unfiltered) and cached. Scalerank filtering
    is applied at query time as a post-filter, avoiding redundant reads
    of the same layer with different thresholds."""

    def __init__(self, gpkg_path):
        self.gpkg_path = gpkg_path
        self._cache = {}  # layer_name -> (geoms, props, strtree)

    def load(self, layer_name):
        """Load all features from a layer.
        Returns (geometries_list, properties_list, STRtree)."""
        if layer_name is None:
            return [], [], None

        if layer_name in self._cache:
            return self._cache[layer_name]

        geoms = []
        props = []
        with fiona.open(self.gpkg_path, layer=layer_name) as src:
            for feat in src:
                try:
                    geom = shape(feat['geometry'])
                    if geom.is_valid and not geom.is_empty:
                        geoms.append(geom)
                        props.append(feat['properties'])
                except Exception:
                    continue

        strtree = STRtree(geoms) if geoms else None
        result = (geoms, props, strtree)
        self._cache[layer_name] = result
        return result

    def query(self, layer_name, bbox_3857, scalerank_max=None,
              scalerank_field='scalerank'):
        """Return (geoms, props) that intersect the given 3857 bbox,
        optionally filtered by scalerank."""
        geoms, props, strtree = self.load(layer_name)
        if not geoms or strtree is None:
            return [], []

        tile_box = box(*bbox_3857)
        indices = strtree.query(tile_box)

        if scalerank_max is not None:
            result_geoms = []
            result_props = []
            for i in indices:
                sr = props[i].get(scalerank_field)
                if sr is None or sr <= scalerank_max:
                    result_geoms.append(geoms[i])
                    result_props.append(props[i])
            return result_geoms, result_props

        return [geoms[i] for i in indices], [props[i] for i in indices]

    def preload_for_zooms(self, zooms):
        """Pre-load all layers that render_tile will need for the given
        zoom levels, so that the cache is fully populated before forking."""
        loaded = set()
        for z in zooms:
            layers = get_layer_set(z)
            for name in layers.values():
                if name and name not in loaded:
                    self.load(name)
                    loaded.add(name)
            for name in LABEL_LAYERS.values():
                if name not in loaded:
                    self.load(name)
                    loaded.add(name)

    def clear(self):
        """Drop all cached data."""
        self._cache.clear()


# ---------------------------------------------------------------------------
# Tile renderer
# ---------------------------------------------------------------------------

def render_tile(z, x, y, cache, font_cache):
    """Render a single tile and return a PIL Image."""
    bbox_3857 = tile_bounds_3857(z, x, y)

    # Expand query bbox slightly for features that straddle tile edges
    dx = (bbox_3857[2] - bbox_3857[0]) * 0.02
    dy = (bbox_3857[3] - bbox_3857[1]) * 0.02
    query_bbox = (bbox_3857[0] - dx, bbox_3857[1] - dy,
                  bbox_3857[2] + dx, bbox_3857[3] + dy)

    to_pixel = make_coord_transform(bbox_3857)
    tile_box = box(*bbox_3857)

    # Padded box for label containment — labels whose anchor is just outside
    # the tile still need to be drawn so their text isn't clipped at the edge.
    # Pad by 50% of tile extent (~128px worth) to cover long label text.
    label_pad_x = (bbox_3857[2] - bbox_3857[0]) * 0.5
    label_pad_y = (bbox_3857[3] - bbox_3857[1]) * 0.5
    label_box = box(bbox_3857[0] - label_pad_x, bbox_3857[1] - label_pad_y,
                    bbox_3857[2] + label_pad_x, bbox_3857[3] + label_pad_y)

    layers = get_layer_set(z)

    img = Image.new('RGBA', (TILE_SIZE, TILE_SIZE), COLOR_OCEAN)
    draw = ImageDraw.Draw(img)

    # --- Polygons ---

    # Land
    geoms, _ = cache.query(layers['land'], query_bbox)
    for geom in geoms:
        pg = clip_to_pixels(geom, tile_box, to_pixel)
        draw_polygon(draw, pg, COLOR_LAND)

    # Lakes
    sr_max = get_scalerank_threshold('lake', z)
    geoms, _ = cache.query(layers['lakes'], query_bbox, scalerank_max=sr_max)
    for geom in geoms:
        pg = clip_to_pixels(geom, tile_box, to_pixel)
        draw_polygon(draw, pg, COLOR_LAKE)

    # Urban areas
    if layers.get('urban'):
        sr_max = get_scalerank_threshold('urban', z)
        geoms, _ = cache.query(layers['urban'], query_bbox,
                               scalerank_max=sr_max)
        for geom in geoms:
            pg = clip_to_pixels(geom, tile_box, to_pixel)
            draw_polygon(draw, pg, COLOR_URBAN)

    # --- Lines ---

    # Rivers
    sr_max = get_scalerank_threshold('river', z)
    width = get_line_width('river', z)
    geoms, _ = cache.query(layers['rivers'], query_bbox, scalerank_max=sr_max)
    for geom in geoms:
        pg = clip_to_pixels(geom, tile_box, to_pixel)
        draw_line(draw, pg, COLOR_RIVER, width)

    # Roads
    if layers.get('roads'):
        sr_max = get_scalerank_threshold('road', z)
        width = get_line_width('road', z)
        geoms, _ = cache.query(layers['roads'], query_bbox,
                               scalerank_max=sr_max)
        for geom in geoms:
            pg = clip_to_pixels(geom, tile_box, to_pixel)
            draw_line(draw, pg, COLOR_ROAD, width)

    # Railroads
    if layers.get('railroads'):
        sr_max = get_scalerank_threshold('railroad', z)
        width = get_line_width('railroad', z)
        geoms, _ = cache.query(layers['railroads'], query_bbox,
                               scalerank_max=sr_max)
        for geom in geoms:
            pg = clip_to_pixels(geom, tile_box, to_pixel)
            draw_line(draw, pg, COLOR_RAILROAD, width)

    # Country borders
    width = get_line_width('border_0', z)
    geoms, _ = cache.query(layers['border_0'], query_bbox,
                           scalerank_field='FEATURECLA')
    for geom in geoms:
        pg = clip_to_pixels(geom, tile_box, to_pixel)
        draw_line(draw, pg, COLOR_BORDER_0, width)

    # State borders
    width = get_line_width('border_1', z)
    geoms, _ = cache.query(layers['border_1'], query_bbox,
                           scalerank_field='FEATURECLA')
    for geom in geoms:
        pg = clip_to_pixels(geom, tile_box, to_pixel)
        draw_line(draw, pg, COLOR_BORDER_1, width)

    # --- Labels ---
    # Use the wider label bbox for queries so we find features whose
    # anchor is outside the tile but whose text extends into it.
    label_query_bbox = label_box.bounds

    # Country labels
    country_font_size = get_font_size('country', z)
    if country_font_size > 0:
        font = font_cache.get(country_font_size)
        sr_max = get_scalerank_threshold('places', z)
        geoms, props = cache.query(LABEL_LAYERS['countries'], label_query_bbox,
                                   scalerank_max=sr_max, scalerank_field='scalerank')
        for geom, prop in zip(geoms, props):
            centroid = geom.representative_point()
            if label_box.contains(centroid):
                px, py = to_pixel(centroid.x, centroid.y)
                name = prop.get('NAME', prop.get('ADMIN', ''))
                if name:
                    draw_label(draw, name.upper(), px, py, font)

    # State labels
    state_font_size = get_font_size('state', z)
    if state_font_size > 0:
        font = font_cache.get(state_font_size)
        geoms, props = cache.query(LABEL_LAYERS['states'], label_query_bbox,
                                   scalerank_max=3)
        for geom, prop in zip(geoms, props):
            centroid = geom.representative_point()
            if label_box.contains(centroid):
                px, py = to_pixel(centroid.x, centroid.y)
                name = prop.get('name', '')
                if name:
                    draw_label(draw, name, px, py, font)

    # City labels
    city_font_size = get_font_size('city', z)
    if city_font_size > 0:
        font = font_cache.get(city_font_size)
        sr_max = get_scalerank_threshold('places', z)
        geoms, props = cache.query(LABEL_LAYERS['places'], label_query_bbox,
                                   scalerank_max=sr_max)
        for geom, prop in zip(geoms, props):
            if label_box.contains(geom):
                px, py = to_pixel(geom.x, geom.y)
                name = prop.get('name', '')
                if name:
                    draw_label(draw, name, px, py, font)

    return img.convert('P', palette=Image.ADAPTIVE, colors=256)


# ---------------------------------------------------------------------------
# Font cache
# ---------------------------------------------------------------------------

class FontCache:
    """Cache PIL fonts by size."""

    # Default font path relative to the project root (thirdparty/fonts/)
    _BUNDLED_FONT = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                                 'thirdparty', 'fonts', 'NotoSans-Regular.ttf')

    def __init__(self, font_path=None):
        self._font_path = font_path or self._BUNDLED_FONT
        self._fonts = {}

    def get(self, size):
        if size not in self._fonts:
            try:
                self._fonts[size] = ImageFont.truetype(self._font_path, size)
            except (OSError, IOError):
                self._fonts[size] = ImageFont.load_default()
        return self._fonts[size]


# ---------------------------------------------------------------------------
# Worker process support
# ---------------------------------------------------------------------------

# Module-level globals shared with worker processes via fork COW.
# _shared_cache is pre-loaded in the parent; workers inherit it read-only.
_shared_cache = None
_worker_font_cache = None


def _worker_init(font_path):
    """Initialize per-worker FontCache. The FeatureCache is inherited
    from the parent process via fork (copy-on-write)."""
    global _worker_font_cache
    _worker_font_cache = FontCache(font_path)


def _render_and_save(args):
    """Render a single tile and save it to disk. Called in worker process."""
    z, x, y, output_dir = args
    img = render_tile(z, x, y, _shared_cache, _worker_font_cache)
    tile_dir = os.path.join(output_dir, str(z), str(x))
    os.makedirs(tile_dir, exist_ok=True)
    tile_path = os.path.join(tile_dir, f"{y}.png")
    img.save(tile_path, 'PNG', optimize=True)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_zoom_range(s):
    """Parse a zoom range like '0-8' or '5' into (min_zoom, max_zoom)."""
    if '-' in s:
        parts = s.split('-', 1)
        return int(parts[0]), int(parts[1])
    z = int(s)
    return z, z


# Resolution tier boundaries
_TIER_RANGES = {
    'low':  (0, 2),
    'mid':  (3, 5),
    'high': (6, 8),
}


def _get_tier(z):
    """Return the resolution tier name for a zoom level."""
    if z <= 2:
        return 'low'
    elif z <= 5:
        return 'mid'
    else:
        return 'high'


def is_solid_color(img_path):
    """Check if a PNG tile is a single solid color."""
    with Image.open(img_path) as img:
        # Convert to RGB so extrema is always per-channel tuples,
        # regardless of whether the source is palette or RGB mode.
        # Go via RGBA first so palette transparency bytes are handled
        # cleanly (avoids PIL UserWarning).
        extrema = img.convert('RGBA').convert('RGB').getextrema()
        return all(lo == hi for lo, hi in extrema)


def prune_solid_tiles(output_dir, min_zoom, max_zoom):
    """Delete solid-color tiles that are redundant due to overzoom fallback.

    Sweeps from max_zoom down to min_zoom+1. A tile is prunable if:
      1. It is a single solid color, AND
      2. All 4 of its children are missing (pruned earlier or at max zoom)

    This ensures we never delete a tile that a missing child would need
    to fall back through.

    z=min_zoom tiles are never pruned (they are the root fallback).
    """
    pruned = 0
    pruned_bytes = 0

    for z in range(max_zoom, min_zoom, -1):
        z_pruned = 0
        n = 2 ** z
        for x in range(n):
            x_dir = os.path.join(output_dir, str(z), str(x))
            if not os.path.isdir(x_dir):
                continue
            for y in range(n):
                tile_path = os.path.join(x_dir, f"{y}.png")
                if not os.path.isfile(tile_path):
                    continue

                # Check that all 4 children are missing (at max_zoom,
                # no children exist, so this is trivially true)
                if z < max_zoom:
                    cx, cy = x * 2, y * 2
                    children_exist = False
                    for dx in (0, 1):
                        for dy in (0, 1):
                            child = os.path.join(output_dir, str(z + 1),
                                                 str(cx + dx),
                                                 f"{cy + dy}.png")
                            if os.path.isfile(child):
                                children_exist = True
                                break
                        if children_exist:
                            break
                    if children_exist:
                        continue

                if is_solid_color(tile_path):
                    pruned_bytes += os.path.getsize(tile_path)
                    os.remove(tile_path)
                    z_pruned += 1

        pruned += z_pruned
        if z_pruned > 0:
            print(f"    z{z}: pruned {z_pruned:,} solid tiles")

    # Clean up empty directories
    for z in range(max_zoom, min_zoom, -1):
        z_dir = os.path.join(output_dir, str(z))
        if not os.path.isdir(z_dir):
            continue
        for x_name in os.listdir(z_dir):
            x_dir = os.path.join(z_dir, x_name)
            if os.path.isdir(x_dir) and not os.listdir(x_dir):
                os.rmdir(x_dir)

    return pruned, pruned_bytes


def main():
    parser = argparse.ArgumentParser(
        description='Render basemap tiles from Natural Earth GeoPackage')
    parser.add_argument('gpkg',
                        help='Path to natural_earth_vector.gpkg or .gpkg.zip')
    parser.add_argument('output', help='Output directory for tiles (e.g. tiles/)')
    parser.add_argument('--zoom', default='0-7',
                        help='Zoom range, e.g. "0-7" or "5" (default: 0-7)')
    parser.add_argument('--font', default=None,
                        help='Path to .ttf font file (default: thirdparty/fonts/NotoSans-Regular.ttf)')
    parser.add_argument('--workers', type=int, default=os.cpu_count(),
                        help=f'Number of parallel workers (default: {os.cpu_count()})')
    parser.add_argument('--prune', action='store_true',
                        help='Delete solid-color tiles that are redundant via overzoom')
    args = parser.parse_args()

    min_zoom, max_zoom = parse_zoom_range(args.zoom)
    num_workers = max(1, args.workers)

    if not os.path.isfile(args.gpkg):
        print(f"Error: GeoPackage not found: {args.gpkg}", file=sys.stderr)
        sys.exit(1)

    # Ensure we have an EPSG:3857 GeoPackage
    gpkg_3857 = ensure_3857_gpkg(args.gpkg)

    # Count total tiles
    total_tiles = sum(2 ** (2 * z) for z in range(min_zoom, max_zoom + 1))
    print(f"Rendering z{min_zoom}-{max_zoom}: {total_tiles:,} tiles "
          f"with {num_workers} workers")

    rendered = 0
    t_start = time.time()

    # Group zoom levels by resolution tier and process each tier with a
    # fresh pool so workers don't accumulate stale caches across tiers.
    all_zooms = list(range(min_zoom, max_zoom + 1))
    tier_groups = []
    current_tier = None
    current_group = []
    for z in all_zooms:
        tier = _get_tier(z)
        if tier != current_tier:
            if current_group:
                tier_groups.append((current_tier, current_group))
            current_tier = tier
            current_group = [z]
        else:
            current_group.append(z)
    if current_group:
        tier_groups.append((current_tier, current_group))

    for tier_name, zooms in tier_groups:
        # Pre-load features in the parent process. Workers inherit this
        # via fork copy-on-write, avoiding redundant GeoPackage reads.
        global _shared_cache
        _shared_cache = FeatureCache(gpkg_3857)
        print(f"  Loading {tier_name}-resolution features for "
              f"z{zooms[0]}-{zooms[-1]}...")
        load_start = time.time()
        _shared_cache.preload_for_zooms(zooms)
        load_elapsed = time.time() - load_start
        print(f"    Pre-loaded in {load_elapsed:.1f}s")

        # Build tile list for all zoom levels in this tier
        tile_args = []
        for z in zooms:
            n = 2 ** z
            for x in range(n):
                for y in range(n):
                    tile_args.append((z, x, y, args.output))

        tier_start = time.time()

        # Use single-process path when workers=1 (easier debugging)
        if num_workers == 1:
            _worker_init(args.font)
            z_start = time.time()
            current_z = tile_args[0][0] if tile_args else None
            for ta in tile_args:
                if ta[0] != current_z:
                    z_elapsed = time.time() - z_start
                    z_count = 2 ** (2 * current_z)
                    tiles_per_sec = z_count / z_elapsed if z_elapsed > 0 else 0
                    print(f"    z{current_z}: {z_count:,} tiles in "
                          f"{z_elapsed:.1f}s ({tiles_per_sec:.1f} tiles/s)")
                    current_z = ta[0]
                    z_start = time.time()
                _render_and_save(ta)
                rendered += 1
            if current_z is not None:
                z_elapsed = time.time() - z_start
                z_count = 2 ** (2 * current_z)
                tiles_per_sec = z_count / z_elapsed if z_elapsed > 0 else 0
                print(f"    z{current_z}: {z_count:,} tiles in "
                      f"{z_elapsed:.1f}s ({tiles_per_sec:.1f} tiles/s)")
        else:
            # Fork-based pool: workers inherit _shared_cache via COW
            ctx = multiprocessing.get_context('fork')
            with ctx.Pool(num_workers, initializer=_worker_init,
                          initargs=(args.font,)) as pool:
                for _ in pool.imap_unordered(_render_and_save, tile_args,
                                             chunksize=16):
                    rendered += 1

                tier_elapsed = time.time() - tier_start
                tier_count = len(tile_args)
                tiles_per_sec = tier_count / tier_elapsed if tier_elapsed > 0 else 0

                for z in zooms:
                    z_count = 2 ** (2 * z)
                    print(f"    z{z}: {z_count:,} tiles")
                print(f"    {tier_name} tier: {tier_count:,} tiles in "
                      f"{tier_elapsed:.1f}s ({tiles_per_sec:.1f} tiles/s)")

    # Prune redundant solid-color tiles
    if args.prune and max_zoom > min_zoom:
        print("Pruning solid-color tiles (reverse sweep)...")
        pruned, pruned_bytes = prune_solid_tiles(args.output, min_zoom,
                                                  max_zoom)
        if pruned > 0:
            mb = pruned_bytes / (1024 * 1024)
            print(f"  Pruned {pruned:,} tiles ({mb:.1f} MB)")
        else:
            print("  No tiles pruned")

    elapsed = time.time() - t_start
    overall_rate = rendered / elapsed if elapsed > 0 else 0
    print(f"Done: {rendered:,} tiles in {elapsed:.1f}s "
          f"({overall_rate:.1f} tiles/s)")


if __name__ == '__main__':
    main()
