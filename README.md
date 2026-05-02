# OpenSectional

A desktop application for visualizing FAA NASR (National Airspace System Resource) data on an interactive map. Displays airports, navaids, fixes, airways, airspace boundaries, TFRs, military training routes, obstacles, weather stations, and communication outlets as vector overlays on a raster basemap. Features include rotated airway/MTR labels, composite airspace labels with altitude bounds, overlap-eliminated text placement, interactive flight-route planning with drag-to-edit waypoints, and A\* route pathfinding driven by a `?` sigil in the route text. Geographic features use spherical geometry (great-circle arcs, geodesic circles).

## Quick Start

```bash
# 1. Install system dependencies (see "Build from source" below for the
#    Ubuntu / brew / MSYS2 package lists).

# 2. Build:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 3. Set up Python venv for data build tools
cd tools && python3 -m venv env && env/bin/pip install -r requirements.txt && cd ..

# 4. Download FAA data (prints build command when done)
tools/env/bin/python3 tools/download_all.py nasr_data

# 5. Build the NASR database (use the command printed by the download script)

# 6. Download the Natural Earth basemap source (~100 MB zip; one time)
mkdir -p mapdata
curl -L -o mapdata/natural_earth_vector.gpkg.zip \
    https://naciscdn.org/naturalearth/packages/natural_earth_vector.gpkg.zip

# 7. Render the basemap tile pyramid into basemap/
tools/env/bin/python3 tools/render_basemap.py mapdata/natural_earth_vector.gpkg.zip basemap/

# 8. Run. With no options, osect looks for osect.db and basemap/ next
#    to the executable (installer layout) or in the current working
#    directory (dev). Configuration is optional — sensible chart-style
#    and routing defaults are baked into the binary; see "Configuration"
#    below for the override file format.
./build/osect

# Override any asset path explicitly:
./build/osect -d osect.db -b basemap -c osect.ini

# Verbosity: -v (warnings), -vv (info), -vvv (debug)
./build/osect -vv

# Full usage:
./build/osect --help
```

## Build from source

Two paths exist:

- **Contributor build (default).** Dependencies come from your system package manager. Fast configure, fast build, dynamic linkage. This is the path described below.
- **Release/installer build.** Dependencies are pinned via in-tree submodules and built statically into a self-contained binary. Triggered by the per-platform scripts under `tools/build-*-package.sh`. See [Cutting a release](#cutting-a-release).

`git clone` the repo without `--recurse-submodules` — the submodules under `thirdparty/SDL`, `thirdparty/SDL_image`, `thirdparty/SDL_ttf`, `thirdparty/zlib`, and `thirdparty/curl` are only needed for release builds and stay un-initialized otherwise.

### Library dependencies

| Library | Source for contributor build | Purpose |
|---|---|---|
| SDL3 | system (libsdl3-dev / sdl3 / mingw-w64-x86_64-SDL3) | Window, input, GPU rendering |
| SDL3_image | system | Tile image loading (PNG) |
| SDL3_ttf | system | Text rendering (freetype + harfbuzz) |
| libcurl | system | Ephemeral-data HTTP client |
| zlib | system | gzip/deflate (libcurl dependency) |
| SQLite3 | system | NASR database queries |
| Dear ImGui | in-repo | UI widgets |
| GLM | in-repo | Matrix/vector math |
| pugixml | in-repo | XML parser (XNOTAM) |
| mapbox/earcut | in-repo | Polygon triangulation |
| doctest | in-repo | Unit-test harness |
| Noto Sans (Regular) | in-repo | Embedded UI font |

Plus `xxd` (vim) for embedding shaders/font as C headers, and the shader cross-compilation toolchain — `glslangValidator` (or `dxc`) for HLSL → SPIR-V, and `spirv-cross` on macOS for SPIR-V → MSL. Each per-platform package list below includes these. The [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) bundles all of them and is sufficient on its own, but is **not required** — the build searches `$VULKAN_SDK/bin` first when set, then falls through to `PATH`, so distro / Homebrew / MacPorts packages work without any Vulkan SDK install. The SDK is only strictly needed on Windows when you want the experimental D3D12 backend, which requires `dxc` (the Microsoft compiler that produces DXIL bytecode). SDL3 must be 3.2 or newer.

### macOS (MacPorts)

```bash
sudo port install cmake pkgconfig SDL3 SDL3_image SDL3_ttf sqlite3 curl glslang spirv-cross
```

### macOS (Homebrew)

```bash
brew install cmake pkgconf sdl3 sdl3_image sdl3_ttf sqlite curl shaderc spirv-cross
```

Apple's Command Line Tools provide `xxd`, `git`, and the C/C++ toolchain. Full Xcode is optional (it provides the Metal compiler for `.metallib` precompilation; without it, osect falls back to compiling MSL at runtime — same correctness, slightly slower first frame).

### Linux (Debian/Ubuntu)

```bash
sudo apt install build-essential cmake pkgconf xxd \
    libsdl3-dev libsdl3-image-dev libsdl3-ttf-dev \
    libcurl4-openssl-dev libsqlite3-dev \
    glslang-tools
```

`libsdl3-dev` pulls in all the X11/Wayland/audio dev headers SDL3 needs (libx11-dev, libwayland-dev, libxkbcommon-dev, libasound2-dev, libpulse-dev, libpipewire-0.3-dev, libdecor-0-dev, …) as transitive dependencies; `libcurl4-openssl-dev` similarly pulls in libssl-dev and zlib1g-dev. `glslang-tools` ships `glslangValidator`, the HLSL → SPIR-V compiler. `spirv-cross` is not needed on Linux — the Linux build doesn't produce MSL or DXIL.

### Linux (Alpine)

```bash
sudo apk add build-base cmake pkgconf vim \
    sdl3-dev sdl3_image-dev sdl3_ttf-dev \
    curl-dev sqlite-dev \
    glslang spirv-cross-dev
```

### FreeBSD

```bash
pkg install cmake pkgconf vim sdl3 sdl3-image sdl3-ttf curl sqlite3 glslang spirv-cross
```

### Windows (MSYS2 / MinGW-w64)

```bash
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
          mingw-w64-x86_64-pkgconf \
          mingw-w64-x86_64-SDL3 mingw-w64-x86_64-SDL3_image mingw-w64-x86_64-SDL3_ttf \
          mingw-w64-x86_64-curl mingw-w64-x86_64-zlib mingw-w64-x86_64-sqlite3 \
          mingw-w64-x86_64-vulkan-headers mingw-w64-x86_64-vulkan-loader \
          mingw-w64-x86_64-shaderc mingw-w64-x86_64-spirv-cross
```

MSVC is not supported. See [BUILD-WINDOWS.md](BUILD-WINDOWS.md) for a step-by-step walkthrough.

### Build commands

```bash
# Release build (optimized)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Debug build (with AddressSanitizer on Linux/macOS)
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j

# Tests
ctest --test-dir build --output-on-failure
```

## Cutting a release

The macOS DMG and Windows NSIS installers ship a self-contained binary with all C/C++ dependencies (SDL3, SDL3_image, SDL3_ttf, libcurl, zlib, SQLite3) built from pinned submodules and linked statically. TLS comes from the OS-native backend on each platform (SecureTransport on macOS, Schannel on Windows).

Both package scripts initialize the dependency submodules, download the SQLite amalgamation (sha256-verified), configure CMake with `-DOSECT_VENDOR_DEPS=ON`, build, run `cpack`, and then **restore `thirdparty/` to its pre-build state by default** — submodules deinitialized, tarball-extracted directories and `.cache/` removed. Pass `--no-clean` to keep the build state in place when iterating on the installer (faster re-runs since submodules don't need to re-init).

### macOS DMG

```bash
./tools/build-macos-package.sh              # build then clean
./tools/build-macos-package.sh --no-clean   # build, leave thirdparty/ initialized
```

Additionally downloads a precompiled universal MoltenVK dylib (sha256-verified) and ships it in `Contents/Frameworks/`, so the .app runs without requiring the user to install Vulkan SDK or Homebrew. Builds a universal (arm64+x86_64) binary via `cmake/macos-toolchain.cmake` (`CMAKE_OSX_ARCHITECTURES=arm64;x86_64`, `CMAKE_OSX_DEPLOYMENT_TARGET=11.0`). Output: `build-macos-package/OpenSectional-X.Y.Z-Darwin.dmg`.

The DMG is **not signed by a Developer ID and not notarized.** First-launch instructions and the optional Developer ID / notarization workflow are documented in [Installer signing](#installer-signing) below.

### Windows NSIS (MinGW-w64 cross-compile)

```bash
sudo apt install g++-mingw-w64-x86-64 nsis    # Debian/Ubuntu host
./tools/build-mingw-package.sh                # build then clean
./tools/build-mingw-package.sh --no-clean     # build, leave thirdparty/ initialized
```

The resulting `osect.exe` is self-contained: the only DLLs shipped alongside it are the MinGW C++ runtime (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`). Output: `build-mingw-package/OpenSectional-X.Y.Z-win64.exe`.

To additionally include the experimental D3D12 backend, install the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) on the build host so `dxc` is available; otherwise the configure step skips DXIL automatically and the binary builds Vulkan-only.

### Installer signing — macOS

The macOS DMG is ad-hoc signed (`codesign --sign -`) at install time so it launches on Apple Silicon, but **not signed by a Developer ID and not notarized.** On a user's machine the first launch will be blocked by Gatekeeper. To open it the first time:

1. Drag `OpenSectional.app` from the DMG to `/Applications`.
2. Double-click and dismiss the warning ("Apple could not verify…").
3. Open **System Settings → Privacy & Security**, scroll to the bottom, and click **Open Anyway** next to the OpenSectional entry. Confirm at the next prompt.

Alternative for terminal users: strip the quarantine attribute after dragging out of the DMG:

```bash
xattr -dr com.apple.quarantine /Applications/OpenSectional.app
```

To distribute without the Gatekeeper warning you need a paid Apple Developer Program membership for a Developer ID certificate and notarization:

```bash
codesign --deep --force --options runtime --sign "Developer ID Application: Your Name (TEAMID)" \
    build-macos-package/_CPack_Packages/Darwin/DragNDrop/OpenSectional-0.1.0-Darwin/ALL_IN_ONE/OpenSectional.app
xcrun notarytool submit build-macos-package/OpenSectional-0.1.0-Darwin.dmg \
    --apple-id <your-apple-id> --team-id TEAMID --password <app-specific-password> --wait
xcrun stapler staple build-macos-package/OpenSectional-0.1.0-Darwin.dmg
```

### Installer signing — Windows

The MinGW-cross-built `osect.exe` and the NSIS installer (`OpenSectional-X.Y.Z-win64.exe`) ship **unsigned**. There is no Windows analogue to macOS's free `codesign --sign -` ad-hoc signing — Authenticode requires a CA-issued certificate by design, so the unsigned binaries are simply unsigned. They still run; the user just sees a SmartScreen warning on first launch. To open the installer the first time:

1. Click **More info** in the "Windows protected your PC" dialog.
2. Click **Run anyway**.

The accept persists per-machine, so subsequent launches open without the prompt.

To distribute without the SmartScreen warning you need a code-signing certificate from a CA (Sectigo, DigiCert, GlobalSign — typically $200–$300/year). Note the trade-off: a standard **OV** certificate identifies the publisher but doesn't immediately remove the SmartScreen warning — it has to "build reputation" through download volume, which can take weeks or months for a low-volume project. An **EV** certificate (hardware token, higher cost) bypasses the reputation gate immediately. Sign on the Linux build host with `osslsigncode` against both the inner executable and the NSIS installer, including an RFC 3161 timestamp so the signature stays valid past the cert's expiration:

```bash
sudo apt install osslsigncode

osslsigncode sign \
    -pkcs12 path/to/cert.pfx -pass 'redacted' \
    -t http://timestamp.sectigo.com \
    -in  build-mingw-package/osect.exe \
    -out build-mingw-package/osect-signed.exe

osslsigncode sign \
    -pkcs12 path/to/cert.pfx -pass 'redacted' \
    -t http://timestamp.sectigo.com \
    -in  build-mingw-package/OpenSectional-0.1.0-win64.exe \
    -out build-mingw-package/OpenSectional-0.1.0-win64-signed.exe
```

For a clean signed installer, sign `osect.exe` *before* `cpack` runs (so the NSIS installer wraps an already-signed binary), then sign the produced installer afterward. The package script doesn't currently automate this; invoke `osslsigncode` manually around `tools/build-mingw-package.sh` with `--no-clean`, or extend the script when you have a cert in hand.

### Installer assets and packaging notes

The package scripts run `cpack` against pre-generated runtime assets that aren't checked in:

- `osect.db` — built from FAA NASR data (see [Data Preparation](#data-preparation))
- `basemap/` — rendered from Natural Earth (see [Data Preparation](#data-preparation))

`cpack` aborts with `Installer asset missing: ...` until those exist. The macOS bundle additionally needs `osect.png` for icon generation (via `sips` + `iconutil`); the Windows installer uses the same PNG via ImageMagick `magick`. If the icon-generation tool is missing the installer still builds, just without a custom icon.

### GPU Backend

OpenSectional defaults to Vulkan on all platforms (via MoltenVK on macOS). Use `--gpu metal` to override on macOS. The shader format is selected at runtime based on the active backend. On macOS, MoltenVK is bundled into `OpenSectional.app/Contents/Frameworks/`, so the .app runs out of the box without requiring the user to install Vulkan SDK or Homebrew.

A D3D12 backend is available on Windows but is **experimental** — Vulkan has shown better performance in testing and is the recommended Windows backend. The D3D12 path is built whenever `dxc` is available at configure time; pass `-DOSECT_ENABLE_D3D12=OFF` (or omit `dxc` from the toolchain) to skip it. Builds without DXIL reject `--gpu direct3d12` at startup with a descriptive error.

### Shader Compiler Toolchain

Shaders are written in HLSL and cross-compiled during the build. The build searches `$VULKAN_SDK/bin` (when set) before falling through to `PATH`, so distro / Homebrew / MacPorts packages work without any Vulkan SDK install. The pipeline:

- **HLSL → SPIR-V**: `glslangValidator` (preferred) or `dxc`. The build picks whichever it finds; output is functionally equivalent.
- **HLSL → DXIL**: `dxc`. Optional — only used when building with the experimental D3D12 backend (Windows targets, controlled by `-DOSECT_ENABLE_D3D12=ON`, default ON). DXIL is Microsoft-defined and has no alternative producer. The configure step auto-disables D3D12 if `dxc` is not found; the resulting Windows binary still runs via Vulkan. The [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) ships `dxc` on all platforms.
- **SPIR-V → MSL**: `spirv-cross`. Required only on macOS for the Metal backend.
- **xxd**: embeds shader bytecode as C headers. Ships with `vim` on most systems.
- **Xcode** (macOS only): full Xcode provides `metal` and `metallib` for precompiling shaders to `.metallib` bytecode (fastest startup). If only Command Line Tools (`xcode-select --install`) is installed, the build automatically falls back to embedding MSL source for the Metal driver to compile at first use — runtime behavior is identical, just a small per-shader compile on first frame. (Apple no longer ships a standalone Metal compiler download.)

## Shader pipeline

Shaders are cross-compiled automatically during the build:
- macOS: HLSL → SPIR-V (always) + HLSL → SPIR-V → MSL source (always) + MSL → .metallib (when full Xcode is available)
- Linux: HLSL → SPIR-V
- Windows: HLSL → SPIR-V (always) + HLSL → DXIL (when `dxc` is available and `OSECT_ENABLE_D3D12=ON`, which is the default)

## Data Preparation

OpenSectional draws on the following upstream data sources. The
"static" sources (everything in this section) are baked into
`osect.db` by the Python ingesters on a cycle cadence; the user
re-runs `download_all.py` + `build_all.py` to refresh. Ephemeral
sources (currently TFRs, with NOTAMs / weather to follow) are
fetched and parsed in-app at runtime — see
[Network and offline mode](#network-and-offline-mode).

| Data | Source | Website | Cadence | Ingester |
|---|---|---|---|---|
| NASR CSV subscription | FAA | https://www.faa.gov/air_traffic/flight_info/aeronav/aero_data/NASR_Subscription/ | 28-day cycle | build_nasr.py |
| Class airspace shapefiles | FAA | https://www.faa.gov/air_traffic/flight_info/aeronav/aero_data/NASR_Subscription/ | 28-day cycle | build_shp.py |
| Special use airspace (AIXM 5.0) | FAA | https://www.faa.gov/air_traffic/flight_info/aeronav/aero_data/NASR_Subscription/ | 28-day cycle | build_aixm.py |
| Digital Obstacle File | FAA | https://www.faa.gov/air_traffic/flight_info/aeronav/digital_products/dof/ | 56-day cycle | build_dof.py |
| ADIZ boundaries | FAA | https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/ArcGIS/rest/services/Airspace/FeatureServer | Irregular | build_adiz.py |
| Temporary flight restrictions | FAA | https://tfr.faa.gov/ | Continuous (as NOTAMs are issued) | (in-app, see [Network and offline mode](#network-and-offline-mode)) |
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

`build_all.py` orchestrates the per-source ingesters (`build_nasr.py` for the NASR CSV subscription, `build_shp.py` for class airspace, `build_aixm.py` for SUA, `build_dof.py` for obstacles, `build_adiz.py` for the ADIZ GeoJSON, and `build_search.py` for the FTS5 index). Each ingester reads its ZIP or directory directly (no manual extraction) and can be re-run on its own when that source updates. All spatial tables have R-tree indexes for bounding-box queries. Column names match FAA NASR naming conventions.

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

OpenSectional requires a basemap tile directory in standard XYZ layout (`{z}/{x}/{y}.png`). The recommended basemap is rendered from Natural Earth public domain data.

#### Natural Earth basemap (recommended)

A minimal worldwide basemap with coastlines, borders, roads, railroads, rivers, lakes, and labels.

```bash
# Download Natural Earth GeoPackage (~100 MB zip)
mkdir -p mapdata
curl -L -o mapdata/natural_earth_vector.gpkg.zip \
    https://naciscdn.org/naturalearth/packages/natural_earth_vector.gpkg.zip

# Render basemap tiles
tools/env/bin/python3 tools/render_basemap.py mapdata/natural_earth_vector.gpkg.zip basemap/
```

On first run, the script reprojects the source data to EPSG:3857 and saves a `*_3857.gpkg` file alongside the zip. Subsequent runs reuse the preprocessed file automatically.

#### FAA VFR raster charts (alternative)

For a basemap derived from FAA aeronav charts, generate XYZ tile pyramids using [aeronav2tiles](https://github.com/ryandrake08/aeronav) or a similar tool and point osect at the output directory.

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

After a route is parsed or drag-edited, OpenSectional rewrites it into its most compact airway-aware form:

- **Airway compaction.** A run of three or more consecutive waypoints that are sequential fixes on a common airway is collapsed into that airway's shorthand. `SLI DODGR DARTS BERRI KIMMO` becomes `SLI V459 KIMMO`.
- **Colinear coercion.** A user-typed direct leg A→B is rewritten to airway shorthand when both endpoints share an airway *and* every intermediate fix of that airway lies within 0.5 NM of the direct great-circle path. The check is iterative from the near edge, so effectively-straight airways spanning hundreds of miles still coerce even when the midpoint shows a larger global cross-track. If any intermediate fails the tolerance, the leg is left alone — the user's typed direct route is always preserved.
- **Discontinuous airways.** When an airway has a published gap (for example `V23` between `FRAME` and `EBTUW`) and a traversal crosses it, the route splits into two airway segments joined by an explicit bridge waypoint. `KSMF V23 KBFL` becomes `KSMF CAPTO V23 EBTUW FRAME V23 EHF KBFL`.

### A\* route pathfinding

Insert a `?` between two waypoints (or between a waypoint and an airway token) and OpenSectional's A\* planner expands it into a sigil-free route before parsing. Examples:

| Input | Result |
|---|---|
| `KSMF ? KBFL` | Plans intermediate fixes/navaids/airports between the two airports. |
| `KSMF ? LIN ? KBFL` | Plans `KSMF → LIN`, then `LIN → KBFL`. |
| `KSMF ? LIN KBFL` | Plans `KSMF → LIN`; `LIN → KBFL` stays direct. |
| `KSMF ? V23 ? KBFL` | Picks V23 entry/exit by *project-and-walk* (project the airport onto V23, pick the adjacent fix nearer the other endpoint), then plans the off-airway segments. |
| `KSMF V23 ? KBFL` | Haversine-nearest entry (existing behavior), project-and-walk exit, plan the exit→KBFL leg. |

The "Use airways" checkbox in the Route panel turns on the airway-class preference (Victor PREFER by default, etc.) and forces airway-routable navaids and WP/RP/CN/MR fixes to INCLUDE for that submission. The "Max leg (nm)" input next to it caps any single A\* hop at the chosen distance. While planning runs on a background thread the input is disabled and an animated indicator is shown. Cross-country plans (e.g. `KSFO ? KJFK`) take a couple of seconds; short hops are imperceptible.

Routing preferences are configured in the `[route_plan]` section of an `osect.ini` override file (see [Configuration](#configuration)). Each waypoint subtype (airport, balloonport, seaplane base, gliderport, heliport, ultralight, VOR, VORTAC, VOR/DME, DME, NDB, NDB/DME, VFR fix) and each airway class (Victor, Jet, RNAV, color, other) takes one of `PREFER` / `INCLUDE` / `AVOID` / `REJECT` (cost multipliers 0.8 / 1.0 / 1.25 / 1000). A separate `route_airway_gap` key controls how A\* prices crossings of published airway discontinuities — `PREFER` makes following a named airway through its gaps cost-attractive; `INCLUDE` is neutral; `AVOID`/`REJECT` push the planner toward switching airways.

### Configuration

OpenSectional ships with sensible chart-style and routing defaults baked into the binary — no configuration file is required. To override defaults, drop an `osect.ini` in any of the following locations (each layers on top of the previous, last wins):

1. Next to the executable (installer layout, contributor builds running from the build dir).
2. The platform-specific user config dir:
   - macOS: `~/Library/Application Support/org.existens.opensectional/osect.ini`
   - Windows: `%APPDATA%\osect\osect.ini`
   - Linux: `${XDG_CONFIG_HOME:-~/.config}/osect/osect.ini`
3. An explicit `-c <path>` / `--conf <path>` on the command line.

The repo's [`osect.ini`](osect.ini) at the source root is a worked example of every key — the values it sets exactly reproduce the in-code defaults, so it's safe to copy and edit as a starting point.

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
| `-c <path>`, `--conf <path>` | Override INI layered last over the default cascade (see [Configuration](#configuration)). Errors if the path does not exist. |
| `--offline` | Skip every network fetch on startup and during refresh; render whatever's in the on-disk ephemeral cache |

When `-b` / `-d` are omitted they're resolved first from next to the
executable (installer layout), then from the current working directory. The
basemap layer is skipped if no basemap is found; the database is required.
`-c` is fully optional — see [Configuration](#configuration) for the
override-file lookup order.

Layer visibility (basemap, airports, runways, navaids, fixes, airways, MTRs,
airspace, SUA, ADIZ, ARTCC, PJA, MAA, TFR, obstacles, AWOS, RCO) is controlled
via checkbox panel in the top-right corner.

## Network and offline mode

Static data ships in `osect.db` and never causes runtime network
traffic. Ephemeral data — TFRs, NOTAMs, weather, etc. — is fetched
in-app on a per-source schedule once the corresponding source lands.
At present no source is wired up, so a default-launched binary makes
zero outbound requests; this section is here to document what to
expect as those sources land.

**Endpoints contacted:**

| Source | Endpoint | Cadence |
|--------|----------|---------|
| TFRs | `https://tfr.faa.gov/tfrapi/getTfrList` + `https://tfr.faa.gov/download/detail_*.xml` | 15 min auto-refresh, manual via the data-status panel |

**Cache directory.** Cached responses live under a per-platform
directory:

- macOS: `~/Library/Caches/org.existens.opensectional/ephemeral/`
- Linux/BSD: `${XDG_CACHE_HOME:-$HOME/.cache}/osect/ephemeral/`
- Windows: `%LOCALAPPDATA%\osect\ephemeral\`

One file per source, with a versioned binary header. Safe to delete
manually — sources fall back to "no prior data" and re-fetch on next
launch.

**`--offline` flag.** Suppresses every outbound HTTP request for the
process lifetime. Sources catch the resulting "offline mode" exception
and fall back to whatever is in the cache; sources whose cache is
missing display empty layers. Useful for working on a plane, behind a
captive portal, or against a stale-but-frozen view of the world.

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
cmake/                    CMake helpers: macOS / MinGW toolchain files, FindZLIB shim
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
  build_search.py         FTS5 search index builder (run last)
  render_basemap.py       Natural Earth basemap tile renderer
  build-macos-package.sh  Vendored universal-binary build → DMG installer (cleans thirdparty/ on success unless --no-clean)
  build-mingw-package.sh  Vendored MinGW-w64 cross build → NSIS installer  (cleans thirdparty/ on success unless --no-clean)
  test_nasr_queries.py    Database query correctness and performance tests
```

## Testing

```bash
# Run database query tests (requires a built osect.db)
tools/env/bin/python3 tools/test_nasr_queries.py osect.db
```

## Third-Party Components

Contributor builds link the SDL trio, libcurl, zlib, and SQLite3 from the host's package manager. Release builds (DMG / NSIS) link them statically from pinned in-tree submodules. Either way the runtime contract — version floor, feature set, license obligations — matches the table below. Smaller header-only / single-source components are always in-repo and list their license texts alongside the source.

| Component | Version pin (release build) | License | Source |
|---|---|---|---|
| SDL3 | 3.4.4 (submodule) | zlib | https://github.com/libsdl-org/SDL |
| SDL3_image | 3.4.2 (submodule) | zlib | https://github.com/libsdl-org/SDL_image |
| SDL3_ttf | 3.2.2 (submodule) | zlib | https://github.com/libsdl-org/SDL_ttf |
| zlib | 1.3.1 (submodule) | zlib | https://github.com/madler/zlib |
| libcurl | 8.13.0 (submodule) | curl (MIT-style) | https://github.com/curl/curl |
| SQLite3 | 3.49.1 (tarball, sha256-pinned) | Public domain | https://www.sqlite.org |
| MoltenVK | 1.3.0 (binary tarball, macOS only, sha256-pinned) | Apache-2.0 | https://github.com/KhronosGroup/MoltenVK |
| Dear ImGui | tracked | MIT | https://github.com/ocornut/imgui |
| GLM | 1.0.1 | MIT (or Happy Bunny) | https://github.com/g-truc/glm |
| pugixml | 1.14 | MIT | https://github.com/zeux/pugixml |
| mapbox/earcut.hpp | tracked | ISC | https://github.com/mapbox/earcut.hpp |
| doctest | tracked | MIT | https://github.com/doctest/doctest |
| Noto Sans (Regular) | 2022 | SIL Open Font License 1.1 | https://github.com/notofonts/latin-greek-cyrillic |

License texts for in-repo components:
`thirdparty/imgui/LICENSE.txt`,
`thirdparty/glm-1.0.1/copying.txt`,
`thirdparty/pugixml-1.14/LICENSE.md`,
`thirdparty/mapbox/LICENSE`,
`thirdparty/doctest/LICENSE.txt`,
`thirdparty/fonts/OFL.txt`.

The Apache-2.0 LICENSE text from MoltenVK's release tarball ships in the installed macOS bundle as `Contents/Resources/LICENSE-MoltenVK.txt`.
