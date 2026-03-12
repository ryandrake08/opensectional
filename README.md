# NASRBrowse

A desktop application for visualizing FAA NASR (National Airspace System Resource) data on an interactive map. Displays airports, navaids, fixes, airways, and airspace boundaries as vector overlays on georeferenced FAA raster chart tiles.

## Quick Start

```bash
# 1. Install dependencies (macOS with MacPorts)
sudo port install cmake SDL3 SDL3_image SDL3_ttf sqlite3

# 2. Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 3. Build the NASR database from FAA ZIP files
python3 tools/build_nasr_db.py 19_Feb_2026_CSV.zip class_airspace_shape_files.zip aixm5.0.zip nasr.db

# 4. Run (requires pre-built XYZ tile directory)
./build/nasrbrowse <tile_path> nasr.db
```

## Dependencies

| Library | Purpose | Integration |
|---------|---------|-------------|
| SDL3 | Window, input, GPU rendering | System package |
| SDL3_image | Tile image loading (PNG) | System package |
| SDL3_ttf | Text rendering | System package |
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
- macOS: HLSL to Metal (via `metal` and `metallib` tools)
- Linux: HLSL to SPIR-V (via `glslangValidator` or similar)

## Data Preparation

NASRBrowse requires two data sources prepared offline:

### 1. NASR Database

Download the following ZIP files from the [FAA NASR subscription](https://www.faa.gov/air_traffic/flight_info/aeronav/aero_data/NASR_Subscription/):

- **28-Day Subscription (CSV)** - e.g., `19_Feb_2026_CSV.zip`
- **Class Airspace Shapefiles** - `class_airspace_shape_files.zip`
- **AIXM 5.0 Special Use Airspace** - `aixm5.0.zip`

Build the database:

```bash
python3 tools/build_nasr_db.py <csv.zip> <shapefile.zip> <aixm.zip> <output.db>
```

This reads directly from the downloaded ZIP files (no manual extraction needed) and produces a ~54 MB SQLite database containing:

| Table | Contents |
|-------|----------|
| APT_BASE | 19,606 airports with coordinates |
| NAV_BASE | 1,649 navaids (VOR, NDB, DME, etc.) |
| FIX_BASE | 69,983 fixes/intersections |
| AWY_SEG | 17,616 airway segments with resolved coordinates |
| MAA_BASE/SHP | 174 MOA/SUA records with polygon shapes |
| APT_RWY/RWY_END | 23,431 runways with endpoint coordinates |
| CLS_ARSP_BASE/SHP | 5,608 class airspace polygons (B/C/D/E) |
| SUA_BASE/SHP | 1,234 special use airspace polygons (MOA/RA/WA/AA/PA/NSA) |

All spatial tables have R-tree indexes for efficient bounding-box queries.

### 2. Raster Chart Tiles

The basemap requires pre-built XYZ tile pyramids from FAA aeronav charts. These are generated separately using [aeronav2tiles](https://github.com/ryandrake08/aeronav) or a similar tool. The tile directory structure is:

```
tiles/
  {z}/
    {x}/
      {y}.png
```

## Controls

| Input | Action |
|-------|--------|
| Click + drag | Pan |
| Scroll wheel | Zoom in/out |
| W/A/S/D | Pan (keyboard) |
| R/F | Zoom in/out (keyboard) |
| T | Toggle tile basemap |

## Project Structure

```
src/                    Application sources
  main.cpp              Entry point, SDL init, main loop
  layer_map.cpp         Map layer (tiles + features + grid)
  tile_renderer.cpp     XYZ tile loading with LRU GPU cache
  feature_renderer.cpp  Vector feature rendering (airports, airways, airspace)
  nasr_database.cpp     SQLite query interface with R-tree spatial queries
  map_view.cpp          Web Mercator viewport, pan/zoom state
  render_context.cpp    Render state (projection matrix, sampler)
sdl/                    SDL3 GPU API wrapper library
shaders/                HLSL shaders (cross-compiled to Metal/SPIR-V)
thirdparty/             Vendored dependencies (GLM)
tools/
  build_nasr_db.py      NASR database builder (reads FAA ZIP files)
  test_nasr_queries.py  Database query correctness and performance tests
```

## Testing

```bash
# Run database query tests (requires a built nasr.db)
python3 tools/test_nasr_queries.py nasr.db
```
