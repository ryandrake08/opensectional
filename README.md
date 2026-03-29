# NASRBrowse

A desktop application for visualizing FAA NASR (National Airspace System Resource) data on an interactive map. Displays airports, navaids, fixes, airways, and airspace boundaries as vector overlays on georeferenced FAA raster chart tiles.

## Quick Start

```bash
# 1. Install dependencies (macOS with MacPorts)
sudo port install cmake SDL3 SDL3_image SDL3_ttf sqlite3

# 2. Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 3. Set up Python venv for data build tools
python3 -m venv env
env/bin/pip install -r requirements.txt

# 4. Download FAA data (prints build command when done)
env/bin/python3 tools/download_nasr_data.py nasr_data

# 5. Build the NASR database (use the command printed by the download script)

# 5. Run (requires pre-built XYZ tile directory)
./build/nasrbrowse <tile_path> nasr.db
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

Set up the Python environment, download FAA data, and build the database:

```bash
python3 -m venv env
env/bin/pip install -r requirements.txt

# Download all FAA data (prints build command when done)
env/bin/python3 tools/download_nasr_data.py nasr_data
```

The download script fetches data from the FAA NASR subscription page, the Digital Obstacle File page, and ADIZ boundaries from the FAA ArcGIS service. Use `--preview` for the next cycle's data instead of the current one.

`build_nasr_db.py` reads the downloaded ZIP files (no manual extraction needed) and produces a SQLite database containing:

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
| OBS_BASE | ~628,000 obstacles (towers, poles, buildings) with AGL/AMSL heights |

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

Layer visibility (basemap, airports, runways, navaids, fixes, airways,
airspace, SUA, obstacles) is controlled via checkbox panel in the top-right
corner.

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
imgui/                  ImGui RAII wrapper library
sdl/                    SDL3 GPU API wrapper library
shaders/                HLSL shaders (cross-compiled to Metal/SPIR-V)
thirdparty/             Vendored dependencies (GLM, Dear ImGui)
tools/
  build_nasr_db.py      NASR database builder (reads FAA ZIP files)
  test_nasr_queries.py  Database query correctness and performance tests
```

## Testing

```bash
# Run database query tests (requires a built nasr.db)
env/bin/python3 tools/test_nasr_queries.py nasr.db
```
