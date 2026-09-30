# OpenSectional

A desktop application for visualizing FAA NASR (National Airspace System Resource) data on an interactive map. Displays airports, navaids, fixes, airways, airspace boundaries, TFRs, military training routes, obstacles, weather stations, and communication outlets as vector overlays on a raster basemap. Features include rotated airway/MTR labels, composite airspace labels with altitude bounds, overlap-eliminated text placement, interactive flight-route planning with drag-to-edit waypoints, user-placed persistent waypoints, and A\* route pathfinding driven by a `?` sigil in the route text. Geographic features use spherical geometry (great-circle arcs, geodesic circles).

## Quick Start

```bash
# 1. Install system dependencies (see "Build from source" below for the
#    Ubuntu / brew / MSYS2 package lists).

# 2. Build (CMake 3.21+):
cmake --preset release
cmake --build --preset release -j

# 3. Set up Python venv for data build tools
cd tools && python3 -m venv env && env/bin/pip install -r requirements.txt && cd ..

# 4. Download FAA data (prints build command when done)
tools/env/bin/python3 tools/download_faa.py nasr_data

# 5. Build the NASR database (use the command printed by the download script)

# 6. Download the Natural Earth basemap source (~426 MB zip; one time)
tools/env/bin/python3 tools/download_basemap.py mapdata

# 7. Render the basemap tile pyramid into basemap/
tools/env/bin/python3 tools/build_basemap.py mapdata/natural_earth_vector.gpkg.zip basemap/

# 8. Build the bundled GMTED2010 z0-z6 global terrain set into terrain/
tools/env/bin/python3 tools/download_terrain.py --dataset gmted2010-30 terrain_source/gmted2010-30
tools/env/bin/python3 tools/build_terrain.py --dataset gmted2010-30 --zoom 0-6 \
    terrain_source/gmted2010-30 terrain

# 9. Run. With no options, osect looks for osect.db and basemap/ next
#    to the executable (installer layout) or in the current working
#    directory (dev). Configuration is optional — sensible chart-style
#    and routing defaults are baked into the binary; see "Configuration"
#    below for the override file format.
./build/osect

# Override any asset path explicitly:
./build/osect -d osect.db -b basemap -t terrain -c osect.ini

# Verbosity: -v (warnings), -vv (info), -vvv (debug)
./build/osect -vv

# Full usage:
./build/osect --help
```

## Build from source

Two paths exist:

- **Contributor build (default).** Dependencies come from your system package manager. Fast configure, fast build, dynamic linkage. This is the path described below.
- **Release/installer build.** CMake fetches pinned dependency sources into the build directory and links them statically into a self-contained binary. See [Cutting a release](#cutting-a-release).

### Library dependencies

| Library | Source for contributor build | Purpose |
|---|---|---|
| SDL3 | system (libsdl3-dev / sdl3 / mingw-w64-x86_64-SDL3) | Window, input, GPU rendering |
| SDL3_image | system | Tile image loading (PNG) |
| SDL3_ttf | system | Text rendering (freetype + harfbuzz) |
| libcurl | system | Ephemeral-data HTTP client |
| zlib | system | gzip/deflate (libcurl dependency) |
| SQLite3 | system | NASR database queries |
| MoltenVK | system, macOS only, optional (MoltenVK / molten-vk) | Vulkan on macOS, the default GPU backend. Not needed to build; without it, run with `--gpu metal`. See [GPU Backend](#gpu-backend) |
| Dear ImGui | in-repo | UI widgets |
| GLM | in-repo | Matrix/vector math |
| pugixml | in-repo | XML parser (XNOTAM) |
| mapbox/earcut | in-repo | Polygon triangulation |
| doctest | in-repo | Unit-test harness |
| Noto Sans (Regular) | in-repo | Embedded UI font |

Plus `xxd` (vim) for embedding shaders/font as C headers, and the shader cross-compilation toolchain — `glslangValidator` (or `dxc`) for HLSL → SPIR-V, and the `spirv-cross` headers and libraries on macOS for SPIR-V → MSL. Each per-platform package list below includes these. The [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) bundles all of them and is sufficient on its own, but is **not required** — the build searches `$VULKAN_SDK/bin` first when set, then falls through to `PATH`, so distro / Homebrew / MacPorts packages work without any Vulkan SDK install. The experimental D3D12 backend for Windows also needs `dxc` (the Microsoft compiler that produces DXIL bytecode); see [Shader Compiler Toolchain](#shader-compiler-toolchain). SDL3 must be 3.2 or newer.

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
    glslang
```

### FreeBSD

```bash
pkg install cmake pkgconf vim sdl3 sdl3-image sdl3-ttf curl sqlite3 glslang
```

### Windows (MSYS2 / MinGW-w64)

```bash
pacman -S --needed git vim \
          mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-pkgconf \
          mingw-w64-x86_64-sdl3 mingw-w64-x86_64-sdl3-image mingw-w64-x86_64-sdl3-ttf \
          mingw-w64-x86_64-curl mingw-w64-x86_64-sqlite3 \
          mingw-w64-x86_64-glslang
```

MSVC is not supported. See [BUILD-WINDOWS.md](BUILD-WINDOWS.md) for a step-by-step walkthrough.

### Build commands

Builds are driven by the presets in `CMakePresets.json`, which needs CMake 3.21 or newer. Configure, build, and test with the same preset name:

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release
```

| Preset | Build directory | Purpose |
|---|---|---|
| `release` | `build/` | Optimized contributor build |
| `debug` | `build-debug/` | Unoptimized, with AddressSanitizer on Linux/macOS/FreeBSD |
| `relwithdebinfo` | `build-relwithdebinfo/` | Optimized with debug info and frame pointers, for profiling |

`cmake --list-presets` shows every preset available on the current host, including the release presets described in [Cutting a release](#cutting-a-release). Extra `-D` options can be appended to any configure command, e.g. `cmake --preset release -DBUILD_TESTING=OFF`. Put personal presets in `CMakeUserPresets.json`, which is git-ignored.

The contributor presets produce `osect` on macOS, Linux, and FreeBSD, or
`osect.exe` on Windows, in the preset's build directory. It does not configure an installer, copy
`osect.db` or `basemap/`, generate platform installer icons, or bundle runtime
libraries. Run it from the repository root so the default asset lookup can find
`osect.db` and `basemap/`, or pass their paths with `--database` and
`--basemap`. On macOS it is a plain executable rather than an application
bundle, and needs a separately installed MoltenVK plus `SDL_VULKAN_LIBRARY` to
use the Vulkan backend (see [GPU Backend](#gpu-backend)).
The release scripts below produce the self-contained platform packages instead.

### Code quality

With `clang-format` and `clang-tidy` installed, a configured build directory also has lint targets for `src/` and `lib/`:

```bash
cmake --build --preset release --target clang-format        # reformat in place
cmake --build --preset release --target clang-format-check  # fail if anything would change
cmake --build --preset release --target clang-tidy
tools/check-sources.sh [-t]                                 # clangd diagnostics (plus clang-tidy with -t)
```

## Cutting a release

The macOS DMG, Windows NSIS installer, and Linux AppImage ship a self-contained binary with all C/C++ dependencies (SDL3, SDL3_image, SDL3_ttf, libcurl, zlib, SQLite3) built from pinned sources and linked statically. TLS comes from the OS-native backend on macOS (SecureTransport) and Windows (Schannel), and from vendored mbedTLS on Linux.

Each installer is one preset. Configuring it fetches the pinned dependency sources (SHA-256-pinned archives; no git needed) into `<build>/_deps/`, which needs network access the first time; building it compiles everything and runs CPack. Nothing is written outside the build directory.

The `macos-vendored` and `mingw-vendored` presets build the same self-contained binary without configuring an installer, so they don't need the bundled data assets below. `linux-vendored` builds the Linux equivalent: a binary that depends only on glibc (TLS from vendored mbedTLS; the system's CA bundle is located at runtime), runnable on distros whose glibc is at least the build host's.

### Prerequisite: build the bundled data assets

The package scripts handle only the C/C++ build. They do **not** download or
process any dataset. Build all three bundled assets, from the repo root, before
running a package script:

| Asset | Build steps | Detail |
|---|---|---|
| `osect.db` | `download_faa.py nasr_data` → `build_faa.py` (use the command the download prints) | [NASR Database](#1-nasr-database) |
| `basemap/` | `download_basemap.py mapdata` → `build_basemap.py mapdata/natural_earth_vector.gpkg.zip basemap/` | [Basemap Tiles](#2-basemap-tiles) |
| `terrain/` | `download_terrain.py --dataset gmted2010-30 terrain_source/gmted2010-30` → `build_terrain.py --dataset gmted2010-30 --zoom 0-6 terrain_source/gmted2010-30 terrain` | [Bundled Terrain Tiles](#3-bundled-terrain-tiles) |

If any of the three is missing, CMake configuration under `OSECT_ENABLE_PACKAGING=ON`
aborts with `Installer asset missing: ...`. These assets are not in source control
and are rebuilt on the source data's own cadence, independent of the app version.

### macOS DMG

```bash
cmake --preset macos-package
cmake --build --preset macos-package -j
```

Also fetches a precompiled universal MoltenVK dylib and ships it in `Contents/Frameworks/`, so the .app runs without requiring the user to install Vulkan SDK or Homebrew. Builds a universal (arm64+x86_64) binary via `cmake/macos-toolchain.cmake` (`CMAKE_OSX_ARCHITECTURES=arm64;x86_64`, `CMAKE_OSX_DEPLOYMENT_TARGET=11.0`). Build on a machine with full Xcode so the DMG includes precompiled `.metallib` shaders for the Metal backend (see [Shader Compiler Toolchain](#shader-compiler-toolchain)). Output: `build-macos-package/OpenSectional-X.Y.Z-Darwin.dmg`.

The DMG is **not signed by a Developer ID and not notarized.** First-launch instructions and the optional Developer ID / notarization workflow are documented in [Installer signing](#installer-signing) below.

### Windows NSIS (MinGW-w64 cross-compile)

```bash
sudo apt install g++-mingw-w64-x86-64 nsis    # Debian/Ubuntu host
cmake --preset mingw-package
cmake --build --preset mingw-package -j
```

The resulting `osect.exe` is self-contained: the only DLLs shipped alongside it are the MinGW C++ runtime (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`). Output: `build-mingw-package/OpenSectional-X.Y.Z-win64.exe`.

The experimental D3D12 backend is included automatically: on a Linux x86_64 build host the configure step downloads a pinned prebuilt `dxc` to compile its shaders. On a macOS or Linux arm64 host, put `dxc` on `PATH` to include it; otherwise the binary builds Vulkan-only.

### Linux AppImage

The vendored SDL is built with only the subsystems osect uses (video, GPU, and the renderer), compiled against the X11, Wayland, D-Bus, and udev development headers; it loads those libraries from the user's system at runtime, and its configure step stops at the first one missing. On the build host, install those packages plus the project's shader and embedding tools. Ubuntu 22.04+ / Debian 12+:

```bash
sudo apt install build-essential pkg-config xxd glslang-tools \
    libfribidi-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
    libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev \
    libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
    libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev \
    libwayland-dev libdecor-0-dev liburing-dev
```

Fedora:

```bash
sudo dnf install gcc-c++ make pkgconf vim-common glslang \
    fribidi-devel libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel \
    libXi-devel libXScrnSaver-devel libXtst-devel dbus-devel ibus-devel \
    systemd-devel mesa-libGL-devel libxkbcommon-devel mesa-libGLES-devel \
    mesa-libEGL-devel vulkan-devel wayland-devel wayland-protocols-devel \
    libdrm-devel mesa-libgbm-devel libdecor-devel \
    libthai-devel liburing-devel
```

The lists are SDL's [Linux build dependencies](https://wiki.libsdl.org/SDL3/README-linux#build-dependencies) minus those of the subsystems osect's build disables (audio, camera, joystick, haptic, HIDAPI); the same packages cover the `linux-vendored` preset.

Packaging also needs CMake 4.2 or newer for CPack's AppImage generator, newer than Ubuntu 22.04 (3.22) and 24.04 (3.28) ship. Install a current CMake from [Kitware's releases](https://cmake.org/download/) (the `cmake-*-linux-<arch>.tar.gz` archive runs from wherever it's unpacked) or Kitware's APT repository, or with `pipx install cmake`. Then:

```bash
cmake --preset linux-package
cmake --build --preset linux-package -j
```

This produces `build-linux-package/OpenSectional-X.Y.Z-<arch>.AppImage` and `OpenSectional-X.Y.Z-<arch>.tar.xz`, both holding `bin/osect` with its data under `share/osect/`, plus a desktop entry and icon. The configure step downloads pinned `appimagetool`, `patchelf`, and AppImage runtime binaries for the build host's architecture (x86_64 or aarch64); packaging needs no FUSE and no network. `osect` depends only on glibc, so the packages run on distros whose glibc is at least the build host's: build on the oldest distribution you intend to support. Running an AppImage needs FUSE (`fusermount`), which desktop distributions include; the `.tar.xz` is the alternative, run in place as `bin/osect`.

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

For a clean signed installer, sign `osect.exe` *before* CPack runs (so the NSIS installer wraps an already-signed binary), then sign the produced installer afterward. The build doesn't automate this: build only the executable with `cmake --build --preset mingw-package --target osect`, sign `build-mingw-package/osect.exe` in place, run `cpack` from `build-mingw-package/`, then sign the installer.

### Installer assets and packaging notes

The package presets run CPack against three pre-generated runtime assets that aren't checked in — `osect.db`, `basemap/`, and `terrain/`. Build them first (see [Prerequisite: build the bundled data assets](#prerequisite-build-the-bundled-data-assets)); packaging configuration aborts with `Installer asset missing: ...` until they exist.

The macOS bundle additionally needs `osect.png` for icon generation (via `sips` + `iconutil`); the Windows installer uses the same PNG via ImageMagick `magick`. If the icon-generation tool is missing the installer still builds, just without a custom icon.

### GPU Backend

OpenSectional defaults to Vulkan on all platforms (via MoltenVK on macOS). Use `--gpu metal` to override on macOS. The shader format is selected at runtime based on the active backend. The packaged macOS build includes MoltenVK in `OpenSectional.app/Contents/Frameworks/`, so the .app runs out of the box without requiring the user to install Vulkan SDK or Homebrew.

**Vulkan on macOS for development.** The bundled MoltenVK only lands in packaged builds (`OSECT_ENABLE_PACKAGING=ON`); a contributor build (`release`, `debug`, or `relwithdebinfo` preset) carries no MoltenVK of its own and needs one installed elsewhere:

- **MacPorts**: `sudo port install MoltenVK` → `/opt/local/lib/libMoltenVK.dylib`
- **Homebrew**: `brew install molten-vk` → `$(brew --prefix)/lib/libMoltenVK.dylib`

Installing it is not enough — you must also point SDL at it by absolute path:

```sh
export SDL_VULKAN_LIBRARY=/opt/local/lib/libMoltenVK.dylib   # MacPorts; adjust for Homebrew
./build/osect --gpu vulkan
```

`SDL_VULKAN_LIBRARY` (SDL's `SDL_HINT_VULKAN_LIBRARY`) is the full path to the dylib SDL should `dlopen` for its Vulkan backend. It is required because current macOS resolves a bare leaf-name `dlopen` (`libvulkan.1.dylib`, `libMoltenVK.dylib`) only against `/usr/lib` and the dyld shared cache — **not** `/usr/local/lib`, `/opt/local/lib`, `/opt/homebrew/lib`, or `$HOME/lib`. Without the hint, SDL finds no Vulkan library and `--gpu vulkan` aborts at startup with `SDL_HINT_GPU_DRIVER vulkan unsupported!`. Unlike `DYLD_LIBRARY_PATH` the hint is surgical: it affects only SDL's Vulkan load, nothing else in the process.

None of this applies to Metal — `--gpu metal` links against Apple system frameworks with no extra deps and is the path of least resistance for day-to-day macOS dev.

**Validation layers (`--gpu_debug`).** Pointing `SDL_VULKAN_LIBRARY` straight at `libMoltenVK.dylib` loads MoltenVK with no Vulkan loader in the chain, so there are no layers for `--gpu_debug` to enable. For validation, install the [Vulkan SDK](https://vulkan.lunarg.com/sdk/home) (keep its "System Install" component, which copies a loader to `/usr/local/lib`) and point the hint at the *loader* instead:

```sh
export SDL_VULKAN_LIBRARY=/usr/local/lib/libvulkan.1.dylib
./build/osect --gpu vulkan --gpu_debug
```

The loader finds MoltenVK and the Khronos validation layer through the SDK's own ICD / layer manifests. Do **not** also `source setup-env.sh` from the SDK — it exports `DYLD_LIBRARY_PATH=$VULKAN_SDK/lib`, which intercepts every leaf-name `dlopen` in the process. Installing both the Vulkan SDK and a package-manager MoltenVK is redundant (the SDK also ships `glslangValidator` / `spirv-cross`, already covered by Homebrew/MacPorts) — pick one, and use the SDK only if you want the validation layer.

A D3D12 backend is available on Windows but is **experimental** — Vulkan has shown better performance in testing and is the recommended Windows backend. It is built by default for Windows targets whenever `dxc` is available (see [Shader Compiler Toolchain](#shader-compiler-toolchain)); pass `-DOSECT_ENABLE_D3D12=OFF` to skip it. Builds without DXIL reject `--gpu direct3d12` at startup with a descriptive error.

### Shader Compiler Toolchain

Shaders are written in HLSL and cross-compiled during the build. The build searches `$VULKAN_SDK/bin` (when set) before falling through to `PATH`, so distro / Homebrew / MacPorts packages work without any Vulkan SDK install. The pipeline:

- **HLSL → SPIR-V**: `glslangValidator` (preferred) or `dxc`. The build picks whichever it finds; output is functionally equivalent.
- **HLSL → DXIL**: `dxc`. Optional — only used when building with the experimental D3D12 backend (Windows targets, controlled by `-DOSECT_ENABLE_D3D12=ON`, default ON). DXIL is Microsoft-defined and has no alternative producer. For Windows targets, the build downloads a pinned prebuilt `dxc` when the build host is Linux x86_64 or Windows (x64 / arm64); on other hosts (macOS, Linux arm64) it uses a `dxc` found on `PATH` or in `$VULKAN_SDK/bin`, and otherwise builds without D3D12 (the Windows binary still runs via Vulkan). The pin lives in `cmake/dxc.cmake`. `dxc` computes the DXIL validation hash itself, so no separate validator library is needed.
- **SPIR-V → MSL**: the `spirv-cross` headers and libraries, linked into a small build-time tool (`shaders/spirv_to_msl.cpp`) that also remaps resource bindings for SDL GPU. Required only on macOS for the Metal backend. The `spirv-cross` command-line tool isn't used.
- **xxd**: embeds shader bytecode as C headers. Ships with `vim` on most systems.
- **Xcode** (macOS only): the build always embeds MSL source. When full Xcode is installed it also precompiles that MSL with `metal` / `metallib` to `.metallib` bytecode and embeds it, which catches MSL errors at build time and skips the runtime compile. With `--gpu metal` the app uses the `.metallib` when present, and otherwise has the Metal driver compile the embedded MSL at first use — same rendering, just a small per-shader compile on the first frame. The default Vulkan backend is unaffected either way (MoltenVK translates the SPIR-V itself). The build detects `metal` automatically; `-DOSECT_ENABLE_METALLIB=OFF` forces the MSL-only path even with Xcode installed, e.g. to test it. Because the choice follows the build machine, a DMG built with only the Command Line Tools ships MSL source only. (Apple no longer ships a standalone Metal compiler download.)

## Shader pipeline

Shaders are cross-compiled automatically during the build:
- macOS: HLSL → SPIR-V (always) + HLSL → SPIR-V → MSL source (always) + MSL → .metallib (when full Xcode is available)
- Linux: HLSL → SPIR-V
- Windows: HLSL → SPIR-V (always) + HLSL → DXIL (when `OSECT_ENABLE_D3D12=ON`, the default, and `dxc` is available — downloaded automatically on Linux x86_64 and Windows build hosts)

## Data Preparation

OpenSectional draws on the following upstream data sources. The
"static" sources (everything in this section) are baked into
`osect.db` by the Python ingesters on a cycle cadence; the user
re-runs `download_faa.py` + `build_faa.py` to refresh. Ephemeral
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
| Natural Earth basemap | Natural Earth | https://www.naturalearthdata.com/ | Irregular | build_basemap.py |
| GMTED2010 terrain elevation | USGS/NGA | https://www.usgs.gov/coastal-changes-and-impacts/gmted2010 | Static (2010) | build_terrain.py |
| Copernicus DEM (GLO-90 / GLO-30) terrain elevation | ESA / Airbus | https://registry.opendata.aws/copernicus-dem/ | Static (2021 release) | build_terrain.py |
| GSHHG shoreline polygons (water mask for GMTED2010) | Wessel & Smith (SOEST Hawaii) | https://www.soest.hawaii.edu/pwessel/gshhg/ | Static (2.3.7, 2017) | build_terrain.py |

Three offline artifacts are produced from these sources: the aviation database, the
basemap tile directory, and the terrain elevation tile directory.

### 1. NASR Database

Set up the Python environment, download FAA data, and build the database:

```bash
cd tools && python3 -m venv env && env/bin/pip install -r requirements.txt && cd ..

# Download all FAA data (prints build command when done)
tools/env/bin/python3 tools/download_faa.py nasr_data
```

The download script fetches data from the FAA NASR subscription page, the Digital Obstacle File page, and ADIZ boundaries from the FAA ArcGIS service. Use `--preview` for the next cycle's data instead of the current one.

`build_faa.py` orchestrates the per-source ingesters (`build_nasr.py` for the NASR CSV subscription, `build_shp.py` for class airspace, `build_aixm.py` for SUA, `build_dof.py` for obstacles, `build_adiz.py` for the ADIZ GeoJSON, and `build_search.py` for the FTS5 index). Each ingester reads its ZIP or directory directly (no manual extraction) and can be re-run on its own when that source updates. All spatial tables have R-tree indexes for bounding-box queries. Column names match FAA NASR naming conventions.

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
# Download the Natural Earth vector GeoPackage (~426 MB zip) into mapdata/
tools/env/bin/python3 tools/download_basemap.py mapdata

# Render basemap tiles
tools/env/bin/python3 tools/build_basemap.py mapdata/natural_earth_vector.gpkg.zip basemap/
```

On first run, the script reprojects the source data to EPSG:3857 and saves a `*_3857.gpkg` file alongside the zip. Subsequent runs reuse the preprocessed file automatically.

#### FAA VFR raster charts (alternative)

For a basemap derived from FAA aeronav charts, generate XYZ tile pyramids using [aeronav2tiles](https://github.com/ryandrake08/aeronav) or a similar tool and point osect at the output directory.

### 3. Bundled Terrain Tiles

The installers ship a small global terrain elevation set built from
[GMTED2010](https://www.usgs.gov/coastal-changes-and-impacts/gmted2010)
(USGS/NGA, public domain). It covers zoom levels 0–6 worldwide (~85 MB)
and feeds the shaded-relief map layer and route terrain profile.

```bash
# Download the GMTED2010 grid plus the GSHHG shoreline archive (149 MB, once)
tools/env/bin/python3 tools/download_terrain.py --dataset gmted2010-30 terrain_source/gmted2010-30

# Build the z0-z6 Terrarium tile tree, water mask and manifest.json into terrain/
tools/env/bin/python3 tools/build_terrain.py --dataset gmted2010-30 --zoom 0-6 \
    terrain_source/gmted2010-30 terrain
```

The build also writes a `water/` sidecar tree so the terrain layer can
draw oceans and lakes on its own (from the Copernicus Water Body Mask,
or GSHHG shoreline polygons for GMTED2010). It covers the same zoom
range as the height tiles and adds roughly 6–20% to the store;
`--no-water` on either tool skips it.

The `[terrain]` section of the ini controls how the relief is drawn:
shading `mode` (`hillshade` / `hypsometric` / `cruise_relative`),
`opacity`, sun `sun_azimuth` / `sun_altitude`, `exaggeration`, the
hypsometric colour `ramp`, and the `cruise_relative` band edges. The Route
panel's per-tab **Cruise altitude (ft)** field is blank until entered and
drives `cruise_relative` shading for the active tab only; it does not yet
affect route planning. See the commented
block in `osect.ini` for the full list and defaults. The layer panel's
**Terrain shading** radio group switches among the three modes for the current
session without changing the ini file. Cruise-relative shading falls back to
hypsometric terrain while no route is active, without changing the selected
radio mode.

Larger, finer sets are downloaded and built with the same two tools by
passing a different `--dataset` and `--zoom` range into a directory of your
choice, then selected with `-t <path>`. With no `-t`, osect uses the tile tree in
`terrain/` next to the executable or in the working directory.

| `--dataset` | Coverage | Post spacing | Model | Licence | Source download | Max `--zoom` |
|---|---|---|---|---|---|---|
| `gmted2010-30` | Global | ~925 m (30") | DSM | Public domain | 0.25 GB grid + 0.15 GB GSHHG | z8 |
| `gmted2010-15` | Global | ~460 m (15") | DSM | Public domain | 0.9 GB grid + 0.15 GB GSHHG | z9 |
| `gmted2010-75` | Global | ~230 m (7.5") | DSM | Public domain | 3.0 GB grid + 0.15 GB GSHHG | z10 |
| `copernicus-glo90` | Global land | ~90 m (3") | DSM | Attribution required | ~4 MB per 1° land tile | z11 |
| `copernicus-glo30` | Global land | ~30 m (1") | DSM | Attribution required | ~25 MB per 1° land tile | z13 |

The max `--zoom` is where output pixels reach half the native post
spacing; `build_terrain.py` refuses a finer zoom rather than
interpolating a coarser source upward.

`gmted2010*` is one whole-globe file. Copernicus is one Cloud Optimized
GeoTIFF per 1° land tile on AWS Open Data.

```bash
tools/env/bin/python3 tools/download_terrain.py --dataset copernicus-glo90 \
    --bbox=-125,24,-66,50 terrain_source/glo90
tools/env/bin/python3 tools/build_terrain.py --dataset copernicus-glo90 --zoom 0-10 \
    terrain_source/glo90 terrain/glo90
```

**Surface-model caveat.** GMTED2010 and Copernicus are digital *surface*
models: their heights include tree canopy and buildings, so a displayed
elevation can read 15–30 m high over forest. For terrain *clearance*
that errs on the safe side. USGS 3DEP (bare earth) does not have this
property but is not yet a supported `--dataset`.

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
| **WPT** button in the click popup header | Drop a user-defined waypoint at the clicked coordinates |
| Drag a user waypoint | Move it to a new location |

Type a route string in the "Route" panel at the top-center, e.g. `O61 LIN V459 LOPES KTSP`, and press Enter or click **Set**. Waypoints may be airports, navaids, fixes, or raw lat/lon (`DDMMSSXDDDMMSSY`, e.g. `383412N1210305W`). Three-token runs `ENTRY AIRWAY EXIT` expand airway shorthand into individual fixes (auto-correcting ENTRY/EXIT to the closest airway fix if needed). Each route occupies its own Route-panel tab — the **+** tab adds another — and routes are saved automatically and restored the next time OpenSectional launches.

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

### User-defined waypoints

Drop your own waypoints anywhere on the map: click a point and press the **WPT** button in the popup header. The waypoint appears under the "user waypoints" layer; its info popup carries **Rename** and **Delete** buttons, and dragging it relocates it.

User waypoints are first-class route points — type a waypoint's name in the Route panel or the search box and it resolves like any airport, navaid, or fix. A NASR identifier of the same name always wins, so a user waypoint never shadows published data.

Saved routes and user waypoints persist across launches in a `user.db` SQLite file under the platform user-data directory — distinct from the re-fetchable ephemeral cache:

- macOS: `~/Library/Application Support/org.existens.opensectional/user.db`
- Windows: `%APPDATA%\osect\user.db`
- Linux: `${XDG_DATA_HOME:-~/.local/share}/osect/user.db`

Delete it to reset OpenSectional to no saved routes or waypoints.

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
| `--gpu_debug` | Enable GPU debug + validation layers (Vulkan needs the SDK loader — see [GPU Backend](#gpu-backend)) |
| `-b <path>`, `--basemap <path>` | XYZ tile directory for the basemap layer |
| `-t <path>`, `--terrain <path>` | Terrain tile directory containing `manifest.json` |
| `-d <path>`, `--database <path>` | NASR SQLite database |
| `-c <path>`, `--conf <path>` | Override INI layered last over the default cascade (see [Configuration](#configuration)). Errors if the path does not exist. |
| `--offline` | Skip every network fetch on startup and during refresh; render whatever's in the on-disk ephemeral cache |

When `-b` / `-d` are omitted they're resolved first from next to the
executable (installer layout), then from the current working directory. The
basemap layer is skipped if no basemap is found; the database is required.
`-c` is fully optional — see [Configuration](#configuration) for the
override-file lookup order.

Layer visibility (basemap, airports, runways, navaids, fixes, airways, MTRs,
airspace, SUA, ADIZ, ARTCC, PJA, MAA, TFR, user waypoints, obstacles, AWOS,
RCO) is controlled via checkbox panel in the top-right corner.

## Network and offline mode

Static data ships in `osect.db` and never causes runtime network
traffic. Ephemeral data — TFRs, NOTAMs, weather, etc. — is fetched
in-app on a per-source schedule. TFRs are live today: a
default-launched binary fetches them at startup and every 15 minutes
after. NOTAMs, weather, and other sources are planned and will be
documented here as they land. Pass `--offline` to suppress every
outbound request (see below).

**Endpoints contacted:**

| Source | Endpoint | Cadence |
|--------|----------|---------|
| TFRs | `https://tfr.faa.gov/tfrapi/getTfrList` + `https://tfr.faa.gov/download/detail_*.xml` | 15 min auto-refresh, manual via the data-status panel |

**Cache file.** Fetched data is cached in a single `ephemeral.db`
SQLite file at the per-platform cache directory:

- macOS: `~/Library/Caches/org.existens.opensectional/ephemeral.db`
- Linux/BSD: `${XDG_CACHE_HOME:-$HOME/.cache}/osect/ephemeral.db`
- Windows: `%LOCALAPPDATA%\osect\ephemeral.db`

It holds every cached source, schema-versioned per source group. Safe
to delete manually — sources fall back to "no prior data" and re-fetch
on next launch.

**`--offline` flag.** Suppresses every outbound HTTP request for the
process lifetime. Sources catch the resulting "offline mode" exception
and fall back to whatever is in the cache; sources whose cache is
missing display empty layers. Useful for working on a plane, behind a
captive portal, or against a stale-but-frozen view of the world.

## Project Structure

```
src/                      Application sources
  main.cpp                Entry point: parse options, construct and run the app
  program.cpp             Application object: SDL init, main run loop, event dispatch
  app_options.cpp         Command-line parsing and bundled-asset path resolution
  map_widget.cpp          Map container: pipelines, input, grid, render orchestration
  map_view.cpp            Web Mercator viewport, pan/zoom, coordinate conversions
  geo_math.cpp            Geodesic and equirectangular geometry helpers
  tile_renderer.cpp       XYZ tile loading with LRU GPU cache
  tile_loader.cpp         Background tile I/O
  feature_renderer.cpp    Feature layer: query scheduling, SDF line packing, GPU upload
  feature_builder.cpp     Background worker: builds polyline geometry from DB results
  feature_type.cpp        Per-feature-type build/pick/selection logic (polymorphic)
  line_renderer.cpp       SDF polyline rendering (lines, dashes, borders, circles)
  label_renderer.cpp      Text label placement, overlap elimination, and rendering (supports rotated and composite labels)
  chart_style.cpp         INI-based zoom-dependent feature styling
  chart_type.cpp          Chart-type enum and per-feature chart membership
  flight_route.cpp        Route data model, shorthand parser, airway expansion, leg computation
  route_planner.cpp       In-memory A* route planner (catalog, airway adjacency, project-and-walk, sigil expansion)
  route_plan_config.cpp   Loads [route_plan] preferences (per-subtype/airway costs) into route_planner::options
  route_submitter.cpp     Background-thread wrapper around route_planner::expand_sigils
  route_session.cpp       Route panel / map / user.db route correspondence and events
  waypoint_session.cpp    User-waypoint create/rename/delete/drag handling
  nasr_database.cpp       SQLite query interface with R-tree spatial queries
  user_database.cpp       SQLite store for user content (saved routes, user waypoints)
  ephemeral_database.cpp  SQLite store for runtime-fetched (ephemeral) data
  ephemeral_source.cpp    Ephemeral-source enum and refresh SDL event type
  tfr_refresher.cpp       Background TFR fetch into ephemeral_database
  http_client.cpp         Synchronous libcurl HTTP client (ETag, offline support)
  xnotam_parser.cpp       XNOTAM XML parser (TFR detail documents)
  data_source.cpp         Data-source freshness records for the status panel
  ui_overlay.cpp          ImGui UI (FPS, layer checkboxes, search, altitude filter, route panel, planner knobs)
  ui_popup_manager.cpp    Feature-info and pick-selector popups
  ui_sectioned_list.cpp   Grouped selectable list widget (pick popup, search results)
  ini_config.cpp          INI file parser
tests/                    doctest unit-test suites (run via ctest)
lib/imgui/                ImGui RAII wrapper library
lib/sdl/                  SDL3 GPU API wrapper library
lib/sqlite/               SQLite RAII wrapper library
shaders/                  HLSL shaders (cross-compiled to Metal/SPIR-V/DXIL)
cmake/                    CMake helpers: macOS / MinGW toolchain files, FindZLIB shim
thirdparty/               Vendored dependencies (see "Third-Party Components")
tools/
  download_faa.py         FAA data downloader (supports --only for per-source fetches)
  build_faa.py            Orchestrator; runs every per-source ingester
  build_common.py         Shared ingestion helpers (ring/antimeridian/altitude)
  build_nasr.py           NASR CSV subscription ingester (APT/NAV/FIX/AWY/...)
  build_shp.py            Class airspace shapefile ingester
  build_aixm.py           AIXM 5.0 SUA ingester
  build_dof.py            Digital Obstacle File ingester
  build_adiz.py           ADIZ GeoJSON ingester
  build_search.py         FTS5 search index builder (run last)
  download_basemap.py     Natural Earth basemap source downloader
  build_basemap.py        Natural Earth basemap tile renderer
  download_terrain.py     DEM source downloader (--dataset NAME, optional --bbox / --jobs)
  build_terrain.py        DEM source → Terrarium z/x/y.png tree + manifest.json (--dataset NAME)
  terrain_datasets.py     Per-dataset adapter registry (gmted2010-*, copernicus-glo*)
  terrain_common.py       Terrarium RGB codec, skirt geometry, tile PNG I/O
  terrain_datum.py        Vertical datum conversion to EGM2008 via PROJ grids
  terrain_manifest.py     manifest.json builder (the ingester↔client contract)
  tile_math.py            Web Mercator tile-bounds math, zoom-range parsing
  http_retry.py           Backoff wrapper for the terrain HTTP fetches
  build_macos_icon.sh     PNG → .icns app icon (sips + iconutil)
  build_windows_icon.sh   PNG → .ico installer icon (ImageMagick)
  test_nasr_queries.py    Database query correctness and performance tests
```

## Testing

```bash
# C++ unit tests (doctest, registered with ctest)
ctest --preset release

# The flight_route and route_planner integration suites require the generated
# osect.db at the repository root. CMake skips them when it is absent.

# Database query tests (requires a built osect.db)
tools/env/bin/python3 tools/test_nasr_queries.py osect.db
```

## Third-Party Components

Contributor builds link the SDL trio, libcurl, zlib, and SQLite3 from the host's package manager. Release builds (DMG / NSIS) fetch pinned sources (`cmake/vendored_deps.cmake`) and link them statically. Either way the runtime contract — version floor, feature set, license obligations — matches the table below. Smaller header-only / single-source components are always in-repo and list their license texts alongside the source.

| Component | Version pin (release build) | License | Source |
|---|---|---|---|
| SDL3 | 3.4.4 (release archive, sha256-pinned) | zlib | https://github.com/libsdl-org/SDL |
| SDL3_image | 3.4.2 (release archive + vendored libtiff, sha256-pinned) | zlib | https://github.com/libsdl-org/SDL_image |
| SDL3_ttf | 3.2.2 (release archive + vendored freetype, harfbuzz, plutosvg, plutovg, sha256-pinned) | zlib | https://github.com/libsdl-org/SDL_ttf |
| zlib | 1.3.1 (release archive, sha256-pinned) | zlib | https://github.com/madler/zlib |
| libcurl | 8.13.0 (release archive, sha256-pinned) | curl (MIT-style) | https://github.com/curl/curl |
| SQLite3 | 3.49.1 (tarball, sha256-pinned) | Public domain | https://www.sqlite.org |
| mbedTLS | 3.6.7 (release archive, Linux vendored builds only, sha256-pinned) | Apache-2.0 | https://github.com/Mbed-TLS/mbedtls |
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
