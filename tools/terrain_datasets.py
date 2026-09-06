"""Per-dataset adapter registry for the terrain ingester.

Every fact specific to one DEM source -- its download URLs and tiling
scheme, native units, native vertical datum, native post spacing,
DSM/DTM model, no-data representation, and licence text -- lives in
that dataset's adapter class here, registered in `DATASETS`. Nothing
outside this module names a dataset: `download_terrain.py` and
`build_terrain.py` take a `--dataset` argument and look it up through
`get_adapter()`, so adding a dataset means adding one adapter class
and a registry entry, with no change to the shared build pipeline.
"""

import math

import numpy as np
import rasterio


class SourceTile:
    """One source raster a dataset publishes, as `tiles_for_bbox` sees it.

    `url` is where to download it and `local_name` is the filename to
    save it under. `bounds` is its (lon_min, lat_min, lon_max, lat_max)
    coverage in degrees.
    """

    def __init__(self, url, local_name, bounds):
        self.url = url
        self.local_name = local_name
        self.bounds = bounds


class SourceRaster:
    """One decoded source raster, in the dataset's *native* datum and units.

    `elevation_m` is a float64 (rows, cols) array with NaN marking
    no-data. `transform` is a rasterio `Affine` mapping pixel
    (col, row) to (lon, lat) -- every supported dataset ships
    unprojected, in geographic (EPSG:4326) coordinates. Datum
    conversion to the client's EGM2008 is the build pipeline's job,
    not the adapter's: this is the raw decode step only.
    """

    def __init__(self, elevation_m, transform):
        self.elevation_m = elevation_m
        self.transform = transform


class DatasetAdapter:
    """Base class for one DEM source. Concrete adapters set every
    attribute below and implement both methods; there is no usable
    default.
    """

    #: Short identifier used on the command line and in manifest.json.
    name = None

    #: Human-readable name, and the licence/attribution text carried
    #: into manifest.json verbatim.
    display_name = None
    attribution = None

    #: The dataset's native vertical datum, before conversion to the
    #: client's EGM2008 (a conversion the build pipeline applies, not
    #: this adapter).
    native_vertical_datum = None

    #: True for a digital surface model (includes canopy/buildings),
    #: False for a bare-earth digital terrain model.
    is_surface_model = None

    #: Native post spacing at the equator, in metres. Informational --
    #: it does not gate which output zoom levels a build may target.
    native_post_spacing_m = None

    #: The Terrarium precision this dataset's data justifies writing
    #: (terrain_common.VERTICAL_PRECISION_WHOLE_METRE or
    #: _SUBMETRE). A source stored natively in whole metres has
    #: nothing to gain from populating the sub-metre channel.
    vertical_precision_m = None

    def tiles_for_bbox(self, lon_min, lat_min, lon_max, lat_max):
        """Return the SourceTile list needed to cover a bbox in degrees.

        `lon_min > lon_max` means the bbox crosses the antimeridian
        (e.g. the Aleutian Islands).
        """
        raise NotImplementedError

    def open_source_raster(self, local_path):
        """Decode one downloaded file (named by a SourceTile.local_name
        this adapter returned) into a SourceRaster."""
        raise NotImplementedError


class Gmted2010Adapter(DatasetAdapter):
    """USGS/NGA GMTED2010, maximum-statistic tiles at 30 arc-seconds.

    Distributed as 30 deg x 20 deg GeoTIFF tiles on a fixed 12x9 grid
    covering the globe, one tile set per (statistic, resolution) pair.
    This adapter uses the max-statistic, 30-arc-second product: the
    coarsest resolution the format offers, matching this dataset's
    role as a small, fast-to-process bootstrap and global fallback,
    and the maximum-per-cell statistic, matching the client's
    max-pooled pyramid (see terrain_common) all the way down to the
    source.
    """

    name = "gmted2010"
    display_name = "GMTED2010 (USGS/NGA, maximum statistic, 30 arc-second)"
    attribution = (
        "Danielson, J.J., and Gesch, D.B., 2011, Global multi-resolution "
        "terrain elevation data 2010 (GMTED2010): U.S. Geological Survey "
        "Open-File Report 2011-1073, 26 p. Public domain."
    )
    native_vertical_datum = "EGM96"
    is_surface_model = True
    native_post_spacing_m = 40075017.0 / 360.0 / 120.0  # 30 arc-seconds at the equator
    vertical_precision_m = 1.0  # terrain_common.VERTICAL_PRECISION_WHOLE_METRE; source is whole metres

    # USGS has moved this endpoint before (2011, then 2016); verify it
    # is still live before relying on it in download_terrain.py.
    SOURCE_BASE_URL = "https://edcintl.cr.usgs.gov/downloads/sciweb1/shared/topo/downloads/GMTED/Global_tiles_GMTED"
    TILE_DATE = "20101117"
    STATISTIC = "max"
    RESOLUTION_CODE = "300"  # 30 arc-second

    TILE_WIDTH_DEG = 30
    TILE_HEIGHT_DEG = 20
    GRID_LON_MIN = -180
    GRID_LAT_MIN = -90

    # The GeoTIFF's own `nodata` tag is authoritative when present;
    # this is the fallback for a file that omits one.
    NATIVE_NODATA = -32768

    @staticmethod
    def _snap_down(value, origin, step):
        return origin + math.floor((value - origin) / step) * step

    def _tile_code(self, sw_lon, sw_lat):
        lat_hemisphere = "N" if sw_lat >= 0 else "S"
        lon_hemisphere = "E" if sw_lon >= 0 else "W"
        return f"{abs(sw_lat):02d}{lat_hemisphere}{abs(sw_lon):03d}{lon_hemisphere}"

    def _tile_filename(self, sw_lon, sw_lat):
        code = self._tile_code(sw_lon, sw_lat)
        return f"{code}_{self.TILE_DATE}_gmted_{self.STATISTIC}{self.RESOLUTION_CODE}.tif"

    def _tiles_for_simple_bbox(self, lon_min, lat_min, lon_max, lat_max):
        tiles = []
        lat0 = self._snap_down(lat_min, self.GRID_LAT_MIN, self.TILE_HEIGHT_DEG)
        lon0 = self._snap_down(lon_min, self.GRID_LON_MIN, self.TILE_WIDTH_DEG)
        lat = lat0
        while lat < lat_max:
            lon = lon0
            while lon < lon_max:
                sw_lon, sw_lat = int(lon), int(lat)
                filename = self._tile_filename(sw_lon, sw_lat)
                tiles.append(SourceTile(
                    url=f"{self.SOURCE_BASE_URL}/{filename}",
                    local_name=filename,
                    bounds=(sw_lon, sw_lat, sw_lon + self.TILE_WIDTH_DEG, sw_lat + self.TILE_HEIGHT_DEG),
                ))
                lon += self.TILE_WIDTH_DEG
            lat += self.TILE_HEIGHT_DEG
        return tiles

    def tiles_for_bbox(self, lon_min, lat_min, lon_max, lat_max):
        if lat_min > lat_max:
            raise ValueError(f"lat_min ({lat_min}) > lat_max ({lat_max})")
        if lon_min > lon_max:
            # Antimeridian-crossing bbox (e.g. the Aleutian Islands):
            # split at +/-180 and merge, without double-counting a
            # tile that spans the seam.
            west = self._tiles_for_simple_bbox(lon_min, lat_min, 180.0, lat_max)
            east = self._tiles_for_simple_bbox(-180.0, lat_min, lon_max, lat_max)
            seen = {t.local_name for t in west}
            return west + [t for t in east if t.local_name not in seen]
        return self._tiles_for_simple_bbox(lon_min, lat_min, lon_max, lat_max)

    def open_source_raster(self, local_path):
        with rasterio.open(local_path) as dataset:
            elevation_m = dataset.read(1).astype(np.float64)
            nodata = dataset.nodata if dataset.nodata is not None else float(self.NATIVE_NODATA)
            elevation_m[elevation_m == nodata] = np.nan
            return SourceRaster(elevation_m=elevation_m, transform=dataset.transform)


DATASETS = {
    "gmted2010": Gmted2010Adapter(),
}


def get_adapter(name):
    """Look up a dataset adapter by its registered name.

    Raises ValueError listing the known names if `name` isn't
    registered -- every terrain tool's --dataset argument surfaces
    this directly to the user.
    """
    try:
        return DATASETS[name]
    except KeyError:
        known = ", ".join(sorted(DATASETS))
        raise ValueError(f"unknown dataset {name!r} (known datasets: {known})") from None
