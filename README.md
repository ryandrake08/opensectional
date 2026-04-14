# NASRBrowse

A desktop application for visualizing FAA NASR (National Airspace System Resource) data on an interactive map. Displays airports, navaids, fixes, airways, and airspace boundaries as vector overlays on georeferenced FAA raster chart tiles.

## Quick Start

**Requires the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home)** (for `dxc` and `spirv-cross` shader tools) with `VULKAN_SDK` set. See [Shader Compiler Toolchain](#shader-compiler-toolchain) for details.

```bash
# 1. Install dependencies (macOS with MacPorts)
sudo port install cmake SDL3 SDL3_image SDL3_ttf sqlite3

# 2. Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 3. Set up Python venv for data build tools
cd tools && python3 -m venv env && env/bin/pip install -r requirements.txt && cd ..

# 4. Download FAA data (prints build command when done)
tools/env/bin/python3 tools/download_nasr_data.py nasr_data

# 5. Build the NASR database (use the command printed by the download script)

# 6. Run. With no options, nasrbrowse looks for nasr.db, basemap/, and
#    nasrbrowse.ini next to the executable (installer layout) or in the
#    current working directory (dev).
./build/nasrbrowse

# Override any asset path explicitly:
./build/nasrbrowse -d nasr.db -b basemap -c nasrbrowse.ini

# Verbosity: -v (warnings), -vv (info), -vvv (debug)
./build/nasrbrowse -vv

# Full usage:
./build/nasrbrowse --help
```

## Dependencies

| Library | Purpose | Integration |
|---------|---------|-------------|
| SDL3 | Window, input, GPU rendering | System package |
| SDL3_image | Tile image loading (PNG) | System package |
| SDL3_ttf | Text rendering | System package |
| Dear ImGui | UI widgets (layer controls, FPS) | Vendored (thirdparty/) |
| SQLite3 | NASR database queries | System package |
| GLM | Matrix/vector math | Vendored (thirdparty/) |

### macOS (MacPorts)

```bash
sudo port install cmake SDL3 SDL3_image SDL3_ttf sqlite3
```

### macOS (Homebrew)

```bash
brew install cmake sdl3 sdl3_image sdl3_ttf sqlite3
```

### Linux (Debian/Ubuntu)

```bash
sudo apt install cmake libsdl3-dev libsdl3-image-dev libsdl3-ttf-dev libsqlite3-dev xxd
```

### Windows (cross-compiled from Linux)

Cross-compile using MinGW-w64. The toolchain and dependency build script are included:

```bash
# Install MinGW toolchain (Debian/Ubuntu)
sudo apt install g++-mingw-w64-x86-64

# Build cross-compiled dependencies (SDL3, SDL3_image, SDL3_ttf, SQLite3)
./tools/build-mingw-deps.sh

# Build
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw -j
```

The resulting `build-mingw/nasrbrowse.exe` is self-contained (all dependencies statically linked, font embedded). The target machine needs a Vulkan-capable GPU with up-to-date drivers.

## Installers

Once `nasr.db`, `basemap/`, and `nasrbrowse.ini` exist in the source tree (see [Data Preparation](#data-preparation)), CPack produces end-user installers that bundle the application together with all three assets.

### macOS (DragNDrop DMG)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && cpack
```

Produces `NASRBrowse-0.1.0-Darwin.dmg`. `nasrbrowse.app` bundles the Homebrew SDL3 dylibs into `Contents/Frameworks/` via `fixup_bundle` at install time, so the DMG is self-contained.

The installer is unsigned; to distribute outside your own machine, sign and notarize after `cpack`:

```bash
codesign --deep --force --options runtime --sign "Developer ID Application: Your Name (TEAMID)" \
    build/_CPack_Packages/Darwin/DragNDrop/NASRBrowse-0.1.0-Darwin/ALL_IN_ONE/nasrbrowse.app
xcrun notarytool submit build/NASRBrowse-0.1.0-Darwin.dmg \
    --apple-id <your-apple-id> --team-id TEAMID --password <app-specific-password> --wait
xcrun stapler staple build/NASRBrowse-0.1.0-Darwin.dmg
```

### Windows (NSIS)

Cross-compile from Linux, then run `cpack`:

```bash
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw -j
cd build-mingw && cpack -G NSIS
```

Produces `NASRBrowse-0.1.0-win64.exe`. Installs into `Program Files\NASRBrowse\`, creates a Start Menu shortcut, and registers an uninstaller. Bundles the MinGW C++ runtime DLLs (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`); SDL3/SDL3_image/SDL3_ttf/sqlite3 are statically linked by `build-mingw-deps.sh`.

NSIS (`makensis`) must be installed on the build host. Debian/Ubuntu: `sudo apt install nsis`.

### App icon

`nasrbrowse.png` (1024×1024) is the app icon source. The build generates `nasrbrowse.icns` (macOS, via `sips` + `iconutil`) or `nasrbrowse.ico` (Windows, via ImageMagick `magick`) into the build directory and hands it to the installer. If the required tool is missing, the icon step is skipped silently and the installer ships without a custom icon.

### Asset gap

`nasr.db` and `basemap/` are not in source control (too large, rebuilt from FAA / Natural Earth sources). `cpack` aborts with `Installer asset missing: ...` until they have been generated. See [Data Preparation](#data-preparation).

### GPU Backend

NASRBrowse defaults to Vulkan on all platforms (via MoltenVK on macOS). Use `--gpu metal` or `--gpu direct3d12` to override. The shader format is selected at runtime based on the active backend.

### Shader Compiler Toolchain

Shaders are written in HLSL and cross-compiled during the build. This requires:

- **Vulkan SDK** - provides `dxc` (HLSL to SPIR-V) and `spirv-cross` (SPIR-V to MSL). Download from [LunarG](https://vulkan.lunarg.com/sdk/home) and set the `VULKAN_SDK` environment variable.
- **xxd** - embeds shader bytecode as C headers. Available via `vim` or `xxd` packages on most systems.
- **Xcode** (macOS only) - provides `metal` and `metallib` for compiling Metal shaders. Requires full Xcode from the App Store, not just Command Line Tools.

## Building

```bash
# Release build (optimized)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Debug build (with AddressSanitizer)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j
```

Shaders are cross-compiled automatically during the build:
- macOS: HLSL → SPIR-V + HLSL → SPIR-V → MSL → .metallib (both formats)
- Linux: HLSL → SPIR-V
- Windows: HLSL → SPIR-V + HLSL → DXIL (both formats)

## Data Preparation

NASRBrowse requires two data sources prepared offline:

### 1. NASR Database

Set up the Python environment, download FAA data, and build the database:

```bash
cd tools && python3 -m venv env && env/bin/pip install -r requirements.txt && cd ..

# Download all FAA data (prints build command when done)
tools/env/bin/python3 tools/download_nasr_data.py nasr_data
```

The download script fetches data from the FAA NASR subscription page, the Digital Obstacle File page, and ADIZ boundaries from the FAA ArcGIS service. Use `--preview` for the next cycle's data instead of the current one.

`build_nasr_db.py` reads the downloaded ZIP files (no manual extraction needed) and produces a SQLite database. All spatial tables have R-tree indexes for efficient bounding-box queries. Column names match FAA NASR naming conventions.

**Airports & Runways**
- `APT_BASE` — 19,606 airports with coordinates, elevation, ownership, facility use
- `APT_RWY` / `APT_RWY_END` / `RWY_SEG` — 23,431 runways with endpoint coordinates and rendering segments
- `APT_ATT` — airport attendance schedules
- `APT_RMK` — airport remarks
- `CLS_ARSP` — airport airspace classification flags (B/C/D/E)

**Navigation**
- `NAV_BASE` / `NAV_RMK` / `NAV_CKPT` — 1,649 navaids (VOR, NDB, DME) with remarks and checkpoints
- `FIX_BASE` / `FIX_NAV` / `FIX_CHRT` — 69,983 fixes with navaid relationships and chart references
- `AWY_BASE` / `AWY_SEG` / `AWY_SEG_ALT` — 17,616 airway segments with resolved coordinates and altitude restrictions
- `WP_LOOKUP` — 91,249 waypoint name→coordinate entries used to resolve route points

**Procedures & Routes**
- `DP_BASE` / `DP_APT` / `DP_RTE` — departure procedures (SIDs)
- `STAR_BASE` / `STAR_APT` / `STAR_RTE` — standard terminal arrivals
- `PFR_BASE` / `PFR_SEG` — preferred flight routes
- `CDR` — coded departure routes
- `HPF_BASE` / `HPF_SPD_ALT` / `HPF_CHRT` / `HPF_RMK` — holding patterns
- `MTR_BASE` / `MTR_SEG` — 5,366 military training route segments

**Airspace**
- `CLS_ARSP_BASE` / `CLS_ARSP_SHP` / `CLS_ARSP_SEG` — 5,608 class airspace polygons (B/C/D/E) with pre-computed rendering segments
- `SUA_BASE` / `SUA_SHP` / `SUA_SEG` — 1,234 special use airspace polygons (MOA/RA/WA/AA/PA/NSA) with rendering segments
- `SUA_CIRCLE` / `SUA_FREQ` / `SUA_SCHEDULE` / `SUA_SERVICE` — SUA circular boundaries, controlling frequencies, activation schedules, and controlling agencies
- `ARTCC_BASE` / `ARTCC_SHP` / `ARTCC_SEG` — ARTCC boundary polygons with rendering segments
- `ADIZ_BASE` / `ADIZ_SHP` / `ADIZ_SEG` — 19 Air Defense Identification Zone boundaries with rendering segments
- `MAA_BASE` / `MAA_SHP` / `MAA_RMK` — 174 miscellaneous activity areas
- `PJA_BASE` — parachute jump areas

**Communications & ATC**
- `ATC_BASE` / `ATC_ATIS` / `ATC_RMK` / `ATC_SVC` — ATC facilities and services
- `FRQ` — frequencies
- `COM` — communication outlet locations (RCO/RCAG)
- `FSS_BASE` / `FSS_RMK` — flight service stations
- `ILS_BASE` / `ILS_GS` / `ILS_DME` / `ILS_MKR` / `ILS_RMK` — instrument landing systems

**Weather & Obstacles**
- `AWOS` — automated weather stations
- `WXL_BASE` / `WXL_SVC` — weather reporting locations and services
- `OBS_BASE` — ~628,000 obstacles (towers, poles, buildings) with AGL/AMSL heights

### 2. Basemap Tiles

NASRBrowse requires a basemap tile directory in standard XYZ layout (`{z}/{x}/{y}.png`). The recommended basemap is rendered from Natural Earth public domain data.

#### Natural Earth basemap (recommended)

A minimal worldwide basemap with coastlines, borders, roads, railroads, rivers, lakes, and labels.

```bash
# Download Natural Earth GeoPackage (~100 MB zip)
wget -P mapdata https://naciscdn.org/naturalearth/packages/natural_earth_vector.gpkg.zip

# Render basemap tiles
tools/env/bin/python3 tools/render_basemap.py natural_earth_vector.gpkg.zip basemap/
```

On first run, the script reprojects the source data to EPSG:3857 and saves a `*_3857.gpkg` file alongside the zip. Subsequent runs reuse the preprocessed file automatically.

#### FAA VFR raster charts (alternative)

For a basemap derived from FAA aeronav charts, generate XYZ tile pyramids using [aeronav2tiles](https://github.com/ryandrake08/aeronav) or a similar tool and point nasrbrowse at the output directory.

## Controls

| Input | Action |
|-------|--------|
| Click + drag | Pan |
| Scroll wheel | Zoom in/out |
| W/A/S/D | Pan (keyboard) |
| R/F | Zoom in/out (keyboard) |

### Command Line Options

| Option | Description |
|--------|-------------|
| `-h`, `--help` | Show usage and exit |
| `-v` | Show warnings |
| `-vv` | Show info (initialization, GPU backend, present mode) |
| `-vvv` | Show debug (resource lifecycle, buffer uploads, shader creation) |
| `-g vulkan`, `--gpu vulkan` | Force Vulkan backend (default on all platforms) |
| `-g metal`, `--gpu metal` | Force Metal backend (macOS only) |
| `-g direct3d12`, `--gpu direct3d12` | Force Direct3D 12 backend (Windows only) |
| `-b <path>`, `--basemap <path>` | XYZ tile directory for the basemap layer |
| `-d <path>`, `--database <path>` | NASR SQLite database |
| `-c <path>`, `--conf <path>` | Chart style INI config |

Any of `-b`, `-d`, `-c` that are omitted are resolved first from next to the
executable (installer layout), then from the current working directory. The
basemap layer is skipped if no basemap is found; the database is required.

Layer visibility (basemap, airports, runways, navaids, fixes, airways, MTRs,
airspace, SUA, ADIZ, ARTCC, obstacles) is controlled via checkbox panel in the
top-right corner.

## Project Structure

```
src/                    Application sources
  main.cpp              Entry point, SDL init, main loop
  layer_map.cpp         Map layer (tiles + features + grid)
  tile_renderer.cpp     XYZ tile loading with LRU GPU cache
  feature_renderer.cpp  Vector feature rendering (airports, airways, airspace, obstacles)
  nasr_database.cpp     SQLite query interface with R-tree spatial queries
  map_view.hpp          Web Mercator viewport, pan/zoom state
  render_context.cpp    Render state (projection matrix, sampler)
  ui_overlay.cpp        ImGui UI (FPS display + layer visibility checkboxes)
lib/imgui/              ImGui RAII wrapper library
lib/sdl/                SDL3 GPU API wrapper library
lib/sqlite/             SQLite RAII wrapper library
shaders/                HLSL shaders (cross-compiled to Metal/SPIR-V/DXIL)
thirdparty/             Vendored dependencies (see "Third-Party Components")
tools/
  download_nasr_data.py   FAA data downloader (NASR, DOF, ADIZ)
  build_nasr_db.py        Database builder (reads downloaded data files)
  build-mingw-deps.sh     Cross-compile Windows dependencies
  test_nasr_queries.py    Database query correctness and performance tests
```

## Testing

```bash
# Run database query tests (requires a built nasr.db)
tools/env/bin/python3 tools/test_nasr_queries.py nasr.db
```

## Third-Party Components

Vendored under `thirdparty/`:

| Component | Version | License | Source |
|---|---|---|---|
| Dear ImGui | tracked | MIT | https://github.com/ocornut/imgui |
| GLM | 1.0.1 | MIT (or Happy Bunny) | https://github.com/g-truc/glm |
| mapbox/earcut.hpp | tracked | ISC | https://github.com/mapbox/earcut.hpp |
| Noto Sans (Regular) | 2022 | SIL Open Font License 1.1 | https://github.com/notofonts/latin-greek-cyrillic |

License texts live next to each component:
`thirdparty/imgui/LICENSE.txt`,
`thirdparty/glm-1.0.1/copying.txt`,
`thirdparty/mapbox/LICENSE`,
`thirdparty/fonts/OFL.txt`.

External runtime dependencies (not vendored): SDL3, SDL3_image, SDL3_ttf,
SQLite3.
