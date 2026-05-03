# Building OpenSectional on Windows (native, MSYS2 + MinGW-w64)

This document covers building OpenSectional **directly on a Windows machine**.
The primary supported workflow remains MinGW-w64 cross-compilation from Linux
(see the "Windows (cross-compiled from Linux)" section of the main `README.md`),
which is what produces the official Windows installer. The native path
documented here exists for contributors who only have access to Windows.

We use the **MSYS2** distribution of MinGW-w64. MSYS2 provides a Unix-like
shell and a `pacman` package manager that already ships every dependency
OpenSectional needs as a prebuilt MinGW package. The compiler (`g++` from
MinGW-w64) and ABI are the same as the cross-compile toolchain, so the build
behaves identically to the Linux-hosted cross-compile.

We do **not** support building with Microsoft Visual C++ (MSVC). The codebase
uses GCC-style attributes and warning flags that would need porting work.

## 1. Install MSYS2

Download the installer from <https://www.msys2.org/> and run it. Accept the
defaults (installs to `C:\msys64`).

After install, open the **MSYS2 MINGW64** shortcut from the Start menu —
**not** the plain "MSYS2 MSYS" or "UCRT64" shortcuts. The MINGW64 environment
is the one that targets native 64-bit Windows binaries with the GCC toolchain
this build expects.

> If you accidentally use the wrong shell, CMake will produce surprising
> errors (wrong compiler picked, wrong libraries found). Look at your prompt:
> the MINGW64 shell shows a magenta `MINGW64` tag; the MSYS shell shows a
> purple `MSYS` tag.

Update the package database the first time you start a MINGW64 shell:

```bash
pacman -Syu
# Close the shell when prompted, then reopen it and run again:
pacman -Syu
```

## 2. Install build dependencies

From the MINGW64 shell:

```bash
pacman -S --needed \
    git \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-pkgconf \
    mingw-w64-x86_64-sdl3 \
    mingw-w64-x86_64-sdl3-image \
    mingw-w64-x86_64-sdl3-ttf \
    mingw-w64-x86_64-sqlite3 \
    mingw-w64-x86_64-curl \
    mingw-w64-x86_64-glslang \
    mingw-w64-x86_64-imagemagick \
    vim
```

(`vim` is intentionally unprefixed — it's only packaged in the MSYS repo, not
the MINGW64 repo. We only need it for `xxd`, which is a build-time tool that
translates bytes to a C header, so the host-environment binary is fine.)

What each package provides:

| Package | Purpose |
|---|---|
| `gcc` | C/C++ compiler targeting native Windows |
| `cmake`, `ninja` | Build system |
| `pkgconf` | `pkg-config` implementation, used to locate SQLite3 |
| `sdl3`, `sdl3-image`, `sdl3-ttf` | Windowing, image loading, text rendering |
| `sqlite3` | NASR database queries |
| `glslang` | Provides `glslangValidator` for HLSL → SPIR-V shader compilation |
| `imagemagick` | Provides `magick`, used to generate the Windows app icon (`osect.ico`) |
| `vim` | Provides `xxd`, used to embed shaders and the bundled font as C headers |

The optional D3D12 backend additionally requires `dxc`, which MSYS2 does not
package. To enable it, install the [Vulkan SDK for
Windows](https://vulkan.lunarg.com/sdk/home) and make `dxc.exe` reachable on
`PATH` before configuring (e.g. by sourcing the SDK's setup script). Without
`dxc` the configure step silently disables the D3D12 backend; the resulting
`osect.exe` still runs via Vulkan.

## 3. Get the source

```bash
git clone https://github.com/ryandrake08/osect.git
cd osect
```

`thirdparty/` is fully vendored, so no submodule init is required.

## 4. Configure and build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`-G Ninja` is recommended on MSYS2; the default generator on Windows is
"MSYS Makefiles", which works but is slower.

The resulting binary is `build/osect.exe`.

## 5. Run

```bash
./build/osect.exe --help
```

You must run `osect.exe` either from inside the MINGW64 shell (which already
has `C:\msys64\mingw64\bin` on `PATH`) or after copying the SDL3 / SQLite3
DLLs from `C:\msys64\mingw64\bin` next to the executable. This is normal
MSYS2 behavior — pacman-installed libraries are dynamic, and Windows resolves
DLLs from `PATH` and the executable's directory.

If you want a portable redistributable instead, use the cross-compile path
documented in `README.md`, which produces a self-contained statically-linked
executable.

> **Don't run `cpack` from a native MSYS2 build.** The installer scaffolding
> in `CMakeLists.txt` only bundles the MinGW C++ runtime DLLs; it assumes
> SDL3/SDL3_image/SDL3_ttf/SQLite3 are statically linked into `osect.exe`,
> which is only true for the cross-compile path. A native-build NSIS
> installer would be missing a dozen `.dll`s and fail to launch on any
> machine without MSYS2 installed. Build installers from Linux via
> `tools/build-mingw-deps.sh` + `cpack -G NSIS` (see `README.md`).

## 6. Run the test suite

```bash
ctest --test-dir build --output-on-failure
```

Tests that need `osect.db` (e.g. `flight_route`, `route_planner`) skip
cleanly if the database hasn't been built. Pure tests (`geo_math`,
`map_view`, `ini_config`, `lru_set`, `altitude_filter`) always run.

## Building the NASR database (optional)

Only needed if you want to actually use the application, not just compile it.
The data preparation tooling is Python-based.

The simplest path on Windows is to install Python from the Microsoft Store or
<https://www.python.org/downloads/windows/> and run the venv setup from a
Windows shell (`cmd.exe` or PowerShell):

```cmd
cd tools
python -m venv env
env\Scripts\pip install -r requirements.txt
env\Scripts\python download_all.py ..\nasr_data
```

Follow the build command printed by `download_all.py` to produce `osect.db`.

(MSYS2 also ships `mingw-w64-x86_64-python`, but several of the geospatial
dependencies in `requirements.txt` are easier to install via Windows-native
Python wheels than under MSYS2. Use whichever you prefer.)

## Headless Windows server hosts

Everything above assumes you're sitting at a desktop Windows machine and can
double-click the MSYS2 installer + Start menu shortcut. On a headless Windows
Server (no console access, no RDP) the same build works, but the install and
shell-invocation steps need adjusting.

### Connect to the host

Modern Windows Server ships **OpenSSH Server** as an optional component. From
an admin PowerShell on the host (one-time setup, e.g. via the cloud
provider's serial console or WinRM):

```powershell
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Start-Service sshd
Set-Service -Name sshd -StartupType Automatic
# Default shell over SSH: switch from cmd to PowerShell for nicer scripting
New-ItemProperty -Path "HKLM:\SOFTWARE\OpenSSH" -Name DefaultShell `
    -Value "C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -PropertyType String -Force
```

Then `ssh user@host` from your workstation. WinRM / PowerShell Remoting works
equally well if that's what your environment uses.

### Install MSYS2 without the GUI installer

The MSYS2 installer (`.exe`) requires a GUI. For headless hosts use the
**self-extracting base archive** instead. From a PowerShell session on the
host:

```powershell
# Download the latest base archive (check https://www.msys2.org/ for the current filename)
$url = "https://github.com/msys2/msys2-installer/releases/download/2025-02-21/msys2-base-x86_64-20250221.sfx.exe"
Invoke-WebRequest -Uri $url -OutFile $env:TEMP\msys2-base.exe

# Extract into C:\ — produces C:\msys64\
& $env:TEMP\msys2-base.exe -y -oC:\

# First-run initialization (sets up /etc, runs post-install hooks)
& C:\msys64\usr\bin\bash.exe -lc ' '
```

Alternatives if your environment already has a package manager set up:

```powershell
winget install --id MSYS2.MSYS2 --silent --accept-package-agreements --accept-source-agreements
# or
choco install msys2 -y --params "/NoUpdate /InstallDir:C:\msys64"
```

### Run MINGW64 commands non-interactively

There is no Start menu shortcut to click. Invoke the MINGW64 shell directly,
either by setting `MSYSTEM=MINGW64` and running `bash`, or by using the
`msys2_shell.cmd` wrapper:

```powershell
# Recommended: set MSYSTEM and run bash with -lc (login shell, run command, exit)
$env:MSYSTEM = "MINGW64"
$env:CHERE_INVOKING = "1"   # keep current directory instead of jumping to $HOME
& C:\msys64\usr\bin\bash.exe -lc "pacman -Syu --noconfirm"
```

Or wrap it in a small PowerShell helper:

```powershell
function mingw64 {
    $env:MSYSTEM = "MINGW64"
    $env:CHERE_INVOKING = "1"
    & C:\msys64\usr\bin\bash.exe -lc "$args"
}

mingw64 "pacman -S --needed --noconfirm git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-sdl3 mingw-w64-x86_64-sdl3-image mingw-w64-x86_64-sdl3-ttf mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-glslang mingw-w64-x86_64-imagemagick vim"
mingw64 "cd /c/src/osect && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release"
mingw64 "cd /c/src/osect && cmake --build build -j"
```

> **`pacman -Syu` two-step.** The first time you update MSYS2, the core
> packages refuse to coexist with running shells and you must run `pacman
> -Syu` twice — the first invocation updates `pacman` itself and exits, then
> a second invocation updates everything else. In an interactive shell MSYS2
> handles this by telling you to close the window. Headless, just run it
> twice in sequence:
>
> ```powershell
> mingw64 "pacman -Syu --noconfirm"
> mingw64 "pacman -Syu --noconfirm"
> ```

Pass `--noconfirm` to every `pacman` invocation in headless contexts so it
never blocks waiting for `[Y/n]` input.

### Build proceeds identically

Once the shell wrapper above works, the dependency-install / `cmake` /
`ctest` commands from sections 2, 4, and 6 above are unchanged — just prefix
each one with `mingw64 "..."`.

## Troubleshooting

**`Could NOT find SDL3` (or SDL3_image, SDL3_ttf)**
You're not in the MINGW64 shell, or you forgot to install the
`mingw-w64-x86_64-sdl3*` packages. Verify with `which cmake` — it should
print `/mingw64/bin/cmake`.

**`Could NOT find PkgConfig` or `sqlite3 not found via pkg-config`**
Install `mingw-w64-x86_64-pkgconf` and `mingw-w64-x86_64-sqlite3`.

**`xxd not found`**
Install `vim` (note: no `mingw-w64-x86_64-` prefix — vim is only in the MSYS
repo, but its bundled `xxd` is fine for our build-time use).

**`glslangValidator not found`**
Install `mingw-w64-x86_64-glslang`.

**`osect.exe` exits immediately with a missing-DLL dialog**
You're running it from outside the MINGW64 shell without the SDL3/SQLite3
DLLs alongside the executable. Either run from MINGW64, add
`C:\msys64\mingw64\bin` to your Windows `PATH`, or copy the DLLs next to
`osect.exe`.

**Configure step says "D3D12 backend: disabled"**
Expected unless you have `dxc.exe` on `PATH` (Vulkan SDK install). Vulkan is
the recommended Windows backend anyway.
