#pragma once

#include <string>

namespace sdl
{
    // Directory containing the running executable. On macOS this is the
    // bundle's Contents/Resources/ when run from an .app. Empty string if
    // SDL cannot determine it.
    std::string base_path();

    // Look up an asset (file or directory) that ships with the application.
    // First checks base_path()/name, then the current working directory.
    // Returns the full path if found, empty string otherwise.
    std::string resolve_bundled_asset(const char* name);
}
