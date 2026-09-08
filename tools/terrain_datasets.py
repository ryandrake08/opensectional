"""Dataset adapters for terrain downloads and builds."""

import os
import shutil
import tempfile
import zipfile


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

    def source_tiles(self):
        """Return the SourceTile list this dataset needs downloaded."""
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
        "30": ("gmted2010", 30.0),
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

    def source_tiles(self):
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


DATASETS = {
    "gmted2010": Gmted2010Adapter("30"),
    "gmted2010-15": Gmted2010Adapter("15"),
    "gmted2010-75": Gmted2010Adapter("75"),
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
