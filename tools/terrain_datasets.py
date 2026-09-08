"""Dataset adapters for terrain downloads and builds."""

import os
import re
import shutil
import tempfile
import urllib.request
import zipfile

import http_retry


class SourceTile:
    """A source raster URL and its local filename."""

    def __init__(self, url, local_name):
        self.url = url
        self.local_name = local_name


class DatasetAdapter:
    """Base class for a DEM source."""

    #: Short identifier used on the command line and in manifest.json.
    name = None

    #: Metadata written to manifest.json.
    display_name = None
    source_version = None
    attribution = None

    #: Source vertical datum.
    native_vertical_datum = None

    #: Source no-data value.
    native_nodata = None

    #: True for a digital surface model (includes canopy/buildings),
    #: False for a bare-earth digital terrain model.
    is_surface_model = None

    #: Native equatorial post spacing in metres.
    native_post_spacing_m = None

    #: Terrarium output precision in metres.
    vertical_precision_m = None

    #: When True, download_terrain.py re-checks an already-present file
    #: against the server's Content-Length before skipping it. When
    #: False, the file's existence alone is taken as "already done" --
    #: correct for a dataset of many immutable per-release files where a
    #: HEAD per file would dominate a resumed run.
    verify_download_size = True

    def source_tiles(self, bbox=None):
        """Return the SourceTile list this dataset needs downloaded.

        `bbox` is `(west, south, east, north)` in degrees, or None for
        the dataset's full coverage. An adapter whose source is a single
        global file raises if given a bbox rather than silently ignoring
        it.
        """
        raise NotImplementedError

    def source_paths(self, source_dir):
        """Return GDAL-openable source paths in `source_dir`."""
        raise NotImplementedError


class Gmted2010Adapter(DatasetAdapter):
    """USGS/NGA GMTED2010, maximum statistic. Constructed for one of the
    three native resolutions: 30, 15 or 7.5 arc-seconds.

    All three resolutions share the same distribution (one whole-globe
    ArcGrid archive), int16-metre storage, EGM96 datum and DSM model —
    only the grid file, post spacing and registry name differ.
    """

    source_version = "20101117"
    attribution = (
        "Danielson, J.J., and Gesch, D.B., 2011, Global multi-resolution "
        "terrain elevation data 2010 (GMTED2010): U.S. Geological Survey "
        "Open-File Report 2011-1073, 26 p. Public domain."
    )
    native_vertical_datum = "EGM96"
    is_surface_model = True
    vertical_precision_m = 1.0
    native_nodata = -32768

    SOURCE_BASE_URL = "https://edcintl.cr.usgs.gov/downloads/sciweb1/shared/topo/downloads/GMTED/Grid_ZipFiles"
    EXTRACTION_MARKER = ".osect-extracted"

    #: File-name resolution code -> (registry name, arc-seconds per post).
    RESOLUTIONS = {
        "30": ("gmted2010-30", 30.0),
        "15": ("gmted2010-15", 15.0),
        "75": ("gmted2010-75", 7.5),
    }

    def __init__(self, resolution_code):
        self.name, arcsec = self.RESOLUTIONS[resolution_code]
        self.display_name = f"GMTED2010 (USGS/NGA, maximum statistic, {arcsec:g} arc-second)"
        # metres per degree at the equator / arc-seconds per degree * arc-seconds per post
        self.native_post_spacing_m = 40075017.0 / 360.0 / 3600.0 * arcsec
        self.GRID_ZIP_NAME = f"mx{resolution_code}_grd.zip"
        self.GRID_INTERNAL_NAME = f"mx{resolution_code}_grd"

    def source_tiles(self, bbox=None):
        if bbox is not None:
            raise ValueError(
                f"{self.name} is a single whole-globe archive; --bbox is not supported "
                "(there is no smaller subset to fetch)"
            )
        return [SourceTile(url=f"{self.SOURCE_BASE_URL}/{self.GRID_ZIP_NAME}", local_name=self.GRID_ZIP_NAME)]

    def source_paths(self, source_dir):
        extracted_dir = os.path.join(source_dir, self.GRID_INTERNAL_NAME)
        marker_path = os.path.join(extracted_dir, self.EXTRACTION_MARKER)
        if os.path.isfile(marker_path):
            return [extracted_dir]
        zip_path = os.path.join(source_dir, self.GRID_ZIP_NAME)
        if not os.path.exists(zip_path):
            return []

        staging_dir = tempfile.mkdtemp(prefix=f".{self.GRID_INTERNAL_NAME}-", dir=source_dir)
        try:
            with zipfile.ZipFile(zip_path) as zf:
                zf.extractall(staging_dir)
            staged_dataset = os.path.join(staging_dir, self.GRID_INTERNAL_NAME)
            if not os.path.isdir(staged_dataset):
                raise ValueError(f"{zip_path} does not contain {self.GRID_INTERNAL_NAME}/")
            with open(os.path.join(staged_dataset, self.EXTRACTION_MARKER), "w"):
                pass
            if os.path.isdir(extracted_dir):
                shutil.rmtree(extracted_dir)
            os.replace(staged_dataset, extracted_dir)
        finally:
            shutil.rmtree(staging_dir, ignore_errors=True)
        return [extracted_dir]


#: A Copernicus COG directory name, e.g.
#: "Copernicus_DSM_COG_30_N39_00_W120_00_DEM". The lat/lon fields name
#: the tile's south-west corner; every tile spans exactly 1x1 degree.
_COP_TILE_RE = re.compile(r"Copernicus_DSM_COG_\d+_([NS])(\d{2})_00_([EW])(\d{3})_00_DEM")


def _parse_copernicus_cell(tile_name):
    """(south_lat, west_lon) integer degrees for a Copernicus tile name,
    or None if the name doesn't match the expected pattern."""
    m = _COP_TILE_RE.fullmatch(tile_name)
    if m is None:
        return None
    ns, lat, ew, lon = m.groups()
    south = int(lat) if ns == "N" else -int(lat)
    west = int(lon) if ew == "E" else -int(lon)
    return south, west


def _cell_overlaps_bbox(cell, bbox):
    """Whether a 1-degree (south_lat, west_lon) cell overlaps
    `(west, south, east, north)`. A bbox with west > east wraps the
    antimeridian."""
    south, west = cell
    b_west, b_south, b_east, b_north = bbox
    if not (south + 1 > b_south and south < b_north):
        return False
    if b_west <= b_east:
        return west + 1 > b_west and west < b_east
    return west + 1 > b_west or west < b_east


class CopernicusGloAdapter(DatasetAdapter):
    """ESA Copernicus DEM, global surface model. Constructed for one of
    the two publicly released resolutions: GLO-90 (3 arc-second) or
    GLO-30 (1 arc-second).

    Both are distributed as one Cloud-Optimized GeoTIFF per 1x1 degree
    land tile on AWS Open Data, share float32-metre storage, the EGM2008
    vertical datum (the client's target -- no conversion) and the DSM
    model. Only the bucket, filename resolution code, post spacing and
    the WorldDEM-30/90 attribution differ.
    """

    source_version = "2021_1"
    native_vertical_datum = "EGM2008"
    is_surface_model = True
    #: The COGs carry no no-data value; ocean is simply absent (no tile)
    #: and land voids are filled at the source.
    native_nodata = None
    #: float32 source -- keep the sub-metre digits rather than rounding.
    vertical_precision_m = 1.0 / 256
    #: Per-release immutable COGs: presence on disk means "done".
    verify_download_size = False

    TILE_LIST_NAME = "tileList.txt"

    #: filename resolution code -> (registry name, product label,
    #: S3 bucket host, arc-seconds per post).
    RESOLUTIONS = {
        "30": ("copernicus-glo90", "GLO-90", "copernicus-dem-90m", 3.0),
        "10": ("copernicus-glo30", "GLO-30", "copernicus-dem-30m", 1.0),
    }

    def __init__(self, resolution_code):
        self.resolution_code = resolution_code
        self.name, product, bucket_host, arcsec = self.RESOLUTIONS[resolution_code]
        self.display_name = f"Copernicus DEM {product} (ESA, {arcsec:g} arc-second)"
        # metres per degree at the equator / arc-seconds per degree * arc-seconds per post
        self.native_post_spacing_m = 40075017.0 / 360.0 / 3600.0 * arcsec
        self.bucket_url = f"https://{bucket_host}.s3.amazonaws.com"
        world_dem = "WorldDEM-90" if resolution_code == "30" else "WorldDEM-30"
        self.attribution = (
            f"produced using Copernicus {world_dem} © DLR e.V. 2010-2014 and "
            "© Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS "
            "by the European Union and ESA; all rights reserved"
        )

    def _fetch_tile_list(self):
        """The bucket's list of tile directory names, one per line."""
        def fetch():
            url = f"{self.bucket_url}/{self.TILE_LIST_NAME}"
            with urllib.request.urlopen(url, timeout=http_retry.TIMEOUT_S) as response:
                return response.read().decode().split()
        return http_retry.retry(fetch, label=self.TILE_LIST_NAME)

    def source_tiles(self, bbox=None):
        tiles = []
        for tile_name in self._fetch_tile_list():
            cell = _parse_copernicus_cell(tile_name)
            if cell is None:
                continue
            if bbox is not None and not _cell_overlaps_bbox(cell, bbox):
                continue
            tiles.append(SourceTile(
                url=f"{self.bucket_url}/{tile_name}/{tile_name}.tif",
                local_name=f"{tile_name}.tif",
            ))
        return tiles

    def source_paths(self, source_dir):
        if not os.path.isdir(source_dir):
            return []
        # The resolution code is part of the filename (30 for GLO-90, 10
        # for GLO-30). Match only this adapter's so a directory holding
        # both resolutions never silently builds with the wrong data.
        prefix = f"Copernicus_DSM_COG_{self.resolution_code}_"
        return sorted(
            os.path.join(source_dir, name)
            for name in os.listdir(source_dir)
            if name.startswith(prefix) and name.endswith("_DEM.tif")
        )


DATASETS = {
    "gmted2010-30": Gmted2010Adapter("30"),
    "gmted2010-15": Gmted2010Adapter("15"),
    "gmted2010-75": Gmted2010Adapter("75"),
    "copernicus-glo90": CopernicusGloAdapter("30"),
    "copernicus-glo30": CopernicusGloAdapter("10"),
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
