# NASRBrowse

A desktop application for visualizing FAA NASR (National Airspace System Resource) data on an interactive map. Displays airports, navaids, fixes, airways, airspace boundaries, TFRs, military training routes, obstacles, weather stations, and communication outlets as vector overlays on a raster basemap. Features include rotated airway/MTR labels, composite airspace labels with altitude bounds, overlap-eliminated text placement, interactive flight-route planning with drag-to-edit waypoints, and A\* route pathfinding driven by a `?` sigil in the route text. Geographic features use spherical geometry (great-circle arcs, geodesic circles).

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
tools/env/bin/python3 tools/download_all.py nasr_data

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

NASRBrowse draws on the following upstream data sources.

| Data | Source | Website | Cadence | Ingester |
|---|---|---|---|---|
| NASR CSV subscription | FAA | https://www.faa.gov/air_traffic/flight_info/aeronav/aero_data/NASR_Subscription/ | 28-day cycle | build_nasr.py |
| Class airspace shapefiles | FAA | https://www.faa.gov/air_traffic/flight_info/aeronav/aero_data/NASR_Subscription/ | 28-day cycle | build_shp.py |
| Special use airspace (AIXM 5.0) | FAA | https://www.faa.gov/air_traffic/flight_info/aeronav/aero_data/NASR_Subscription/ | 28-day cycle | build_aixm.py |
| Digital Obstacle File | FAA | https://www.faa.gov/air_traffic/flight_info/aeronav/digital_products/dof/ | 56-day cycle | build_dof.py |
| ADIZ boundaries | FAA | https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/ArcGIS/rest/services/Airspace/FeatureServer | Irregular | build_adiz.py |
| Temporary flight restrictions | FAA | https://tfr.faa.gov/ | Continuous (as NOTAMs are issued) | build_tfr.py |
| Natural Earth basemap | Natural Earth | https://www.naturalearthdata.com/ | Irregular | render_basemap.py |

Two offline artifacts are produced from these sources: the aviation database and the
basemap tile directory.

### 1. NASR Database

Set up the Python environment, download FAA data, and build the database:

```bash
cd tools && python3 -m venv env && env/bin/pip install -r requirements.txt && cd ..

# Download all FAA data (prints build command when done)
tools/env/bin/python3 tools/download_all.py nasr_data
```

The download script fetches data from the FAA NASR subscription page, the Digital Obstacle File page, and ADIZ boundaries from the FAA ArcGIS service. Use `--preview` for the next cycle's data instead of the current one.

`build_all.py` orchestrates the per-source ingesters (`build_nasr.py` for the NASR CSV subscription, `build_shp.py` for class airspace, `build_aixm.py` for SUA, `build_dof.py` for obstacles, `build_adiz.py` for the ADIZ GeoJSON, `build_tfr.py` for TFR XNOTAMs, and `build_search.py` for the FTS5 index). Each ingester reads its ZIP or directory directly (no manual extraction) and can be re-run on its own when that source updates. All spatial tables have R-tree indexes for bounding-box queries. Column names match FAA NASR naming conventions.

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
- `CLS_ARSP_BASE` / `CLS_ARSP_SHP` / `CLS_ARSP_SEG` — 5,608 class airspace polygons (B/C/D/E) with pre-computed rendering segments. Altitudes parsed from shapefile DESC/VAL/UOM/CODE fields into numeric UPPER_FT/UPPER_REF/LOWER_FT/LOWER_REF columns
- `SUA_BASE` / `SUA_SHP` / `SUA_SEG` — 1,234 special use airspace polygons (MOA/RA/WA/AA/PA/NSA) with rendering segments
- `SUA_CIRCLE` / `SUA_FREQ` / `SUA_SCHEDULE` / `SUA_SERVICE` — SUA circular boundaries, controlling frequencies, activation schedules, and controlling agencies
- `ARTCC_BASE` / `ARTCC_SHP` / `ARTCC_SEG` — ARTCC boundary polygons with rendering segments
- `ADIZ_BASE` / `ADIZ_SHP` / `ADIZ_SEG` — 19 Air Defense Identification Zone boundaries with rendering segments
- `MAA_BASE` / `MAA_SHP` / `MAA_RMK` — 174 miscellaneous activity areas with parsed numeric altitudes (MAX_ALT_FT/MAX_ALT_REF/MIN_ALT_FT/MIN_ALT_REF)
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
| Click + drag (empty space) | Pan |
| Scroll wheel | Zoom in/out |
| W/A/S/D | Pan (keyboard) |
| R/F | Zoom in/out (keyboard) |
| Click feature | Show info popup (or selector if multiple features overlap) |
| Click route line | Select the active route (highlights it white with waypoint halos) |
| Drag route leg | Insert a new waypoint on that leg at the release point |
| Drag route waypoint | Replace that waypoint with whatever's under the cursor on release |
| Drag route waypoint → adjacent waypoint | Delete the dragged waypoint |

Type a route string in the "Route" panel at the top-center, e.g. `O61 LIN V459 LOPES KTSP`, and press Enter or click **Set**. Waypoints may be airports, navaids, fixes, or raw lat/lon (`DDMMSSXDDDMMSSY`, e.g. `383412N1210305W`). Three-token runs `ENTRY AIRWAY EXIT` expand airway shorthand into individual fixes (auto-correcting ENTRY/EXIT to the closest airway fix if needed).

After a route is parsed or drag-edited, NASRBrowse rewrites it into its most compact airway-aware form:

- **Airway compaction.** A run of three or more consecutive waypoints that are sequential fixes on a common airway is collapsed into that airway's shorthand. `SLI DODGR DARTS BERRI KIMMO` becomes `SLI V459 KIMMO`.
- **Colinear coercion.** A user-typed direct leg A→B is rewritten to airway shorthand when both endpoints share an airway *and* every intermediate fix of that airway lies within 0.5 NM of the direct great-circle path. The check is iterative from the near edge, so effectively-straight airways spanning hundreds of miles still coerce even when the midpoint shows a larger global cross-track. If any intermediate fails the tolerance, the leg is left alone — the user's typed direct route is always preserved.
- **Discontinuous airways.** When an airway has a published gap (for example `V23` between `FRAME` and `EBTUW`) and a traversal crosses it, the route splits into two airway segments joined by an explicit bridge waypoint. `KSMF V23 KBFL` becomes `KSMF CAPTO V23 EBTUW FRAME V23 EHF KBFL`.

### A\* route pathfinding

Insert a `?` between two waypoints (or between a waypoint and an airway token) and NASRBrowse's A\* planner expands it into a sigil-free route before parsing. Examples:

| Input | Result |
|---|---|
| `KSMF ? KBFL` | Plans intermediate fixes/navaids/airports between the two airports. |
| `KSMF ? LIN ? KBFL` | Plans `KSMF → LIN`, then `LIN → KBFL`. |
| `KSMF ? LIN KBFL` | Plans `KSMF → LIN`; `LIN → KBFL` stays direct. |
| `KSMF ? V23 ? KBFL` | Picks V23 entry/exit by *project-and-walk* (project the airport onto V23, pick the adjacent fix nearer the other endpoint), then plans the off-airway segments. |
| `KSMF V23 ? KBFL` | Haversine-nearest entry (existing behavior), project-and-walk exit, plan the exit→KBFL leg. |

The "Use airways" checkbox in the Route panel turns on the airway-class preference (Victor PREFER by default, etc.) and forces airway-routable navaids and WP/RP/CN/MR fixes to INCLUDE for that submission. The "Max leg (nm)" input next to it caps any single A\* hop at the chosen distance. While planning runs on a background thread the input is disabled and an animated indicator is shown. Cross-country plans (e.g. `KSFO ? KJFK`) take a couple of seconds; short hops are imperceptible.

Routing preferences are configured in the `[route_plan]` section of `nasrbrowse.ini`. Each waypoint subtype (airport, balloonport, seaplane base, gliderport, heliport, ultralight, VOR, VORTAC, VOR/DME, DME, NDB, NDB/DME, VFR fix) and each airway class (Victor, Jet, RNAV, color, other) takes one of `PREFER` / `INCLUDE` / `AVOID` / `REJECT` (cost multipliers 0.8 / 1.0 / 1.25 / 1000). A separate `route_airway_gap` key controls how A\* prices crossings of published airway discontinuities — `PREFER` makes following a named airway through its gaps cost-attractive; `INCLUDE` is neutral; `AVOID`/`REJECT` push the planner toward switching airways.

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
airspace, SUA, ADIZ, ARTCC, PJA, MAA, TFR, obstacles, AWOS, RCO) is controlled
via checkbox panel in the top-right corner.

## Project Structure

```
src/                      Application sources
  main.cpp                Entry point, SDL init, main loop
  map_widget.cpp          Map container: pipelines, input, grid, render orchestration
  map_view.cpp            Web Mercator viewport, pan/zoom, coordinate conversions
  geo_math.cpp            Spherical geometry (geodesic circles, great-circle interpolation)
  tile_renderer.cpp       XYZ tile loading with LRU GPU cache
  feature_renderer.cpp    Feature layer: query scheduling, SDF line packing, GPU upload
  feature_builder.cpp     Background worker: builds polyline geometry from DB results
  feature_type.cpp        Per-feature-type build/pick/selection logic (polymorphic)
  flight_route.cpp        Route data model, shorthand parser, airway expansion, leg computation
  route_planner.cpp       In-memory A* route planner (catalog, airway adjacency, project-and-walk, sigil expansion)
  route_plan_config.cpp   Loads [route_plan] preferences (per-subtype/airway costs) into route_planner::options
  route_submitter.cpp     Background-thread wrapper around route_planner::expand_sigils
  line_renderer.cpp       SDF polyline rendering (lines, dashes, borders, circles)
  label_renderer.cpp      Text label placement, overlap elimination, and rendering (supports rotated and composite labels)
  nasr_database.cpp       SQLite query interface with R-tree spatial queries
  chart_style.cpp         INI-based zoom-dependent feature styling
  tile_loader.cpp         Background tile I/O
  ui_overlay.cpp          ImGui UI (FPS, layer checkboxes, search, altitude filter, route input/info, planner knobs)
  ui_sectioned_list.cpp   Grouped selectable list widget (pick popup, search results)
  render_context.cpp      Render state (projection matrix, sampler)
  ini_config.cpp          INI file parser
lib/imgui/                ImGui RAII wrapper library
lib/sdl/                  SDL3 GPU API wrapper library
lib/sqlite/               SQLite RAII wrapper library
shaders/                  HLSL shaders (cross-compiled to Metal/SPIR-V/DXIL)
thirdparty/               Vendored dependencies (see "Third-Party Components")
tools/
  download_all.py         FAA data downloader (supports --only for per-source fetches)
  build_all.py            Orchestrator; runs every per-source ingester
  build_common.py         Shared ingestion helpers (ring/antimeridian/altitude)
  build_nasr.py           NASR CSV subscription ingester (APT/NAV/FIX/AWY/...)
  build_shp.py            Class airspace shapefile ingester
  build_aixm.py           AIXM 5.0 SUA ingester
  build_dof.py            Digital Obstacle File ingester
  build_adiz.py           ADIZ GeoJSON ingester
  build_tfr.py            TFR XNOTAM directory ingester
  build_search.py         FTS5 search index builder (run last)
  render_basemap.py       Natural Earth basemap tile renderer
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
