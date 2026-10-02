# Building OpenSectional on Windows (native, MSYS2 + MinGW-w64)

This covers building **directly on a Windows machine**, for contributors who
only have Windows. The official Windows installer is cross-compiled from Linux
or macOS with the `mingw-package` preset (see "Cutting a release" in
`README.md`).

The native build uses **MSYS2**'s MinGW-w64 toolchain and packages. The
compiler and ABI match the cross-compile; the difference is that this build
links MSYS2's DLLs dynamically, while the release build links pinned sources
statically. MSVC is not supported: the codebase uses GCC-style attributes and
warning flags.

## 1. Install MSYS2

Run the installer from <https://www.msys2.org/> (defaults to `C:\msys64`), then
open the **MSYS2 MINGW64** shell from the Start menu — not "MSYS2 MSYS" or
"UCRT64". The prompt shows a magenta `MINGW64` tag; the wrong shell picks the
wrong compiler and libraries and CMake fails in confusing ways.

Update twice the first time (the first run updates `pacman` itself and asks
you to close the shell):

```bash
pacman -Syu
pacman -Syu
```

## 2. Install dependencies

Install the packages listed under "Windows (MSYS2 / MinGW-w64)" in
`README.md`, from the MINGW64 shell. `vim` is unprefixed because it only
exists in the MSYS repo; it's there for `xxd`, a build-time tool, so the MSYS
binary is fine.

The experimental D3D12 backend needs `dxc`, which MSYS2 doesn't package; the
configure step downloads a pinned prebuilt one from Microsoft's releases, so
it needs network access the first time. Pass `-DOSECT_ENABLE_D3D12=OFF` to
skip D3D12; `osect.exe` then runs on Vulkan, the recommended Windows backend.

## 3. Build, test, run

```bash
git clone https://github.com/ryandrake08/osect.git
cd osect
cmake --preset release -G Ninja
cmake --build --preset release -j
ctest --preset release
./build/osect.exe --help
```

`-G Ninja` is recommended; the default "MSYS Makefiles" generator works but is
slower. `debug` and `relwithdebinfo` presets work the same way (see "Build
commands" in `README.md`). Tests that need `osect.db` are only built once the
database exists.

Run `osect.exe` from the MINGW64 shell, which has `C:\msys64\mingw64\bin` on
`PATH`. Elsewhere, add that directory to `PATH` or copy the SDL3 / SQLite3 /
curl DLLs next to the executable — Windows resolves DLLs from `PATH` and the
executable's directory.

This build doesn't produce an installer: packaging requires
`OSECT_VENDOR_DEPS=ON`, because the NSIS installer bundles only the MinGW C++
runtime and assumes everything else is linked into `osect.exe`.

## 4. Data (optional)

To actually use the app, build `osect.db`, `basemap/`, and `terrain/` as
described under "Data Preparation" in `README.md`. On Windows, use
Windows-native Python (python.org or the Microsoft Store) from `cmd.exe` or
PowerShell rather than MSYS2's Python: several geospatial dependencies in
the `pyproject.toml` `tools` dependency group install more easily as Windows wheels. The venv's
interpreter is `tools\env\Scripts\python`.

## Headless Windows hosts

The same build works over SSH on a Windows host with no desktop session:

- **Remote access:** enable the OpenSSH Server optional feature
  (`Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0`, then start
  and enable the `sshd` service).
- **Install MSYS2 without the GUI installer:** use the self-extracting
  `msys2-base-x86_64-*.sfx.exe` from the
  [MSYS2 releases](https://github.com/msys2/msys2-installer/releases)
  (`msys2-base-x86_64-<date>.sfx.exe -y -oC:\`), or
  `winget install --id MSYS2.MSYS2`.
- **Run MINGW64 commands non-interactively:** set `MSYSTEM=MINGW64` and
  `CHERE_INVOKING=1` (stay in the current directory), then run each command
  through `C:\msys64\usr\bin\bash.exe -lc "<command>"`. Pass `--noconfirm` to
  every `pacman` call, and run the first `pacman -Syu --noconfirm` twice.

Steps 2 and 3 are otherwise unchanged.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Could NOT find SDL3` (or SDL3_image, SDL3_ttf) | Not in the MINGW64 shell, or the `mingw-w64-x86_64-sdl3*` packages are missing. `which cmake` should print `/mingw64/bin/cmake`. |
| `Could NOT find PkgConfig` / `sqlite3 not found via pkg-config` | Install `mingw-w64-x86_64-pkgconf` and `mingw-w64-x86_64-sqlite3`. |
| `xxd not found` | Install `vim` (MSYS repo, no prefix). |
| `glslangValidator not found` | Install `mingw-w64-x86_64-glslang`. |
| `osect.exe` exits with a missing-DLL dialog | Run it from the MINGW64 shell, add `C:\msys64\mingw64\bin` to `PATH`, or copy the DLLs next to it. |
| Configure fails downloading `dxc` | No network access: reconnect, or configure with `-DOSECT_ENABLE_D3D12=OFF`. |
