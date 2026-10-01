# Unit tests using doctest. Each test binary is small — list its source
# dependencies explicitly rather than linking the whole app.
#
# add_osect_test(<name> [extra_sources...])
# Creates an executable test_<name> from test_<name>.cpp plus any extra
# sources (e.g. the module-under-test's .cpp), and registers a ctest entry
# named <name> run with the repo root as its working directory (so relative
# paths like "osect.db" resolve).
function(add_osect_test name)
    add_executable(test_${name} test_${name}.cpp ${ARGN})
    target_compile_features(test_${name} PRIVATE cxx_std_17)
    target_include_directories(test_${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/thirdparty
    )
    target_compile_options(test_${name} PRIVATE -Wall -Wextra -Werror
        $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
    )
    target_link_options(test_${name} PRIVATE
        $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
        $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
    )
    # Several src/ modules start std::threads; FreeBSD's libc (unlike glibc)
    # needs the threads library linked explicitly for pthread_create.
    target_link_libraries(test_${name} PRIVATE Threads::Threads)
    add_test(NAME ${name}
             COMMAND test_${name}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

# Pure-header tests (no extra sources needed)
add_osect_test(altitude_filter)
add_osect_test(lru_map)
add_osect_test(tile_cache)
add_osect_test(tile_key)
add_osect_test(tile_quad ${CMAKE_SOURCE_DIR}/src/tile_quad.cpp ${CMAKE_SOURCE_DIR}/src/map_view.cpp)
target_link_libraries(test_tile_quad PRIVATE sdl_wrapper)
# tile_loader logs failed loads through the sdl wrapper.
add_osect_test(tile_loader)
target_link_libraries(test_tile_loader PRIVATE sdl_wrapper)
add_osect_test(texture_format)
target_link_libraries(test_texture_format PRIVATE sdl_wrapper)
add_osect_test(elevation_tile ${CMAKE_SOURCE_DIR}/src/elevation_tile.cpp)
target_link_libraries(test_elevation_tile PRIVATE sdl_wrapper)
add_osect_test(elevation_tile_cache
    ${CMAKE_SOURCE_DIR}/src/elevation_tile_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/elevation_tile.cpp)
target_link_libraries(test_elevation_tile_cache PRIVATE sdl_wrapper)
add_osect_test(elevation_address)
add_osect_test(terrain_profile)
add_osect_test(elevation_source
    ${CMAKE_SOURCE_DIR}/src/elevation_source.cpp
    ${CMAKE_SOURCE_DIR}/src/elevation_tile.cpp
    ${CMAKE_SOURCE_DIR}/src/elevation_tile_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/data_source.cpp)
target_link_libraries(test_elevation_source PRIVATE sdl_wrapper)

# chart_type rules — pure functions of input fields, no DB needed.
add_osect_test(chart_membership ${CMAKE_SOURCE_DIR}/src/chart_type.cpp)

# Tests that compile one .cpp alongside the test
add_osect_test(geo_math        ${CMAKE_SOURCE_DIR}/src/geo_math.cpp)
add_osect_test(map_view        ${CMAKE_SOURCE_DIR}/src/map_view.cpp)
add_osect_test(ini_config      ${CMAKE_SOURCE_DIR}/src/ini_config.cpp)
add_osect_test(chart_style
    ${CMAKE_SOURCE_DIR}/src/chart_style.cpp
    ${CMAKE_SOURCE_DIR}/src/ini_config.cpp)
add_osect_test(terrain_style
    ${CMAKE_SOURCE_DIR}/src/terrain_style.cpp
    ${CMAKE_SOURCE_DIR}/src/ini_config.cpp)
add_osect_test(data_source     ${CMAKE_SOURCE_DIR}/src/data_source.cpp)
# ephemeral_database links the sqlite wrapper and sdl_wrapper (for log_warn).
add_osect_test(ephemeral_database ${CMAKE_SOURCE_DIR}/src/ephemeral_database.cpp)
target_link_libraries(test_ephemeral_database PRIVATE sqlite_wrapper sdl_wrapper)
osect_apply_app_definitions(test_ephemeral_database)

# user_database — same wiring as ephemeral_database.
add_osect_test(user_database ${CMAKE_SOURCE_DIR}/src/user_database.cpp)
target_link_libraries(test_user_database PRIVATE sqlite_wrapper sdl_wrapper)
osect_apply_app_definitions(test_user_database)

# xnotam_parser links the vendored pugixml lib; the test reads
# fixture XMLs from nasr_data/tfr_data/ at the repo root via the
# ctest WORKING_DIRECTORY.
add_osect_test(xnotam_parser   ${CMAKE_SOURCE_DIR}/src/xnotam_parser.cpp)
target_link_libraries(test_xnotam_parser PRIVATE pugixml sdl_wrapper)

# http_client confirms that transport failures throw
add_osect_test(http_client  ${CMAKE_SOURCE_DIR}/src/http_client.cpp)
target_link_libraries(test_http_client PRIVATE CURL::libcurl)
osect_apply_app_definitions(test_http_client)

# flight_route and route_planner query the generated NASR database. They are
# integration tests, enabled only when data preparation has produced osect.db.
if(EXISTS "${CMAKE_SOURCE_DIR}/osect.db")
    add_osect_test(terrain_profile_sampling
        ${CMAKE_SOURCE_DIR}/src/terrain_profile.cpp
        ${CMAKE_SOURCE_DIR}/src/terrain_corridor.cpp
        ${CMAKE_SOURCE_DIR}/src/geo_math.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_source.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile_cache.cpp
        ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp
        ${CMAKE_SOURCE_DIR}/src/data_source.cpp)
    target_link_libraries(test_terrain_profile_sampling PRIVATE sqlite_wrapper sdl_wrapper)
    osect_apply_app_definitions(test_terrain_profile_sampling)

    add_osect_test(terrain_corridor
        ${CMAKE_SOURCE_DIR}/src/terrain_corridor.cpp
        ${CMAKE_SOURCE_DIR}/src/geo_math.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_source.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile_cache.cpp
        ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp
        ${CMAKE_SOURCE_DIR}/src/data_source.cpp)
    target_link_libraries(test_terrain_corridor PRIVATE sqlite_wrapper sdl_wrapper)
    osect_apply_app_definitions(test_terrain_corridor)

    add_osect_test(terrain_profile_worker
        ${CMAKE_SOURCE_DIR}/src/terrain_profile_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/terrain_profile.cpp
        ${CMAKE_SOURCE_DIR}/src/terrain_corridor.cpp
        ${CMAKE_SOURCE_DIR}/src/geo_math.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_source.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile_cache.cpp
        ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp
        ${CMAKE_SOURCE_DIR}/src/data_source.cpp)
    target_link_libraries(test_terrain_profile_worker PRIVATE sqlite_wrapper sdl_wrapper)
    osect_apply_app_definitions(test_terrain_profile_worker)

    # route_planner reads user.db, so user_database.cpp comes along too.
    add_osect_test(flight_route
        ${CMAKE_SOURCE_DIR}/src/flight_route.cpp
        ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp
        ${CMAKE_SOURCE_DIR}/src/data_source.cpp
        ${CMAKE_SOURCE_DIR}/src/route_planner.cpp
        ${CMAKE_SOURCE_DIR}/src/obstacle_index.cpp
        ${CMAKE_SOURCE_DIR}/src/terrain_corridor.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_source.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile_cache.cpp
        ${CMAKE_SOURCE_DIR}/src/user_database.cpp
        ${CMAKE_SOURCE_DIR}/src/geo_math.cpp)
    target_link_libraries(test_flight_route PRIVATE sqlite_wrapper sdl_wrapper)
    osect_apply_app_definitions(test_flight_route)

    add_osect_test(route_planner
        ${CMAKE_SOURCE_DIR}/src/route_planner.cpp
        ${CMAKE_SOURCE_DIR}/src/obstacle_index.cpp
        ${CMAKE_SOURCE_DIR}/src/terrain_corridor.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_source.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile_cache.cpp
        ${CMAKE_SOURCE_DIR}/src/route_plan_config.cpp
        ${CMAKE_SOURCE_DIR}/src/ini_config.cpp
        ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp
        ${CMAKE_SOURCE_DIR}/src/data_source.cpp
        ${CMAKE_SOURCE_DIR}/src/geo_math.cpp
        ${CMAKE_SOURCE_DIR}/src/flight_route.cpp
        ${CMAKE_SOURCE_DIR}/src/user_database.cpp)
    target_link_libraries(test_route_planner PRIVATE sqlite_wrapper sdl_wrapper)
    osect_apply_app_definitions(test_route_planner)

    add_osect_test(obstacle_index
        ${CMAKE_SOURCE_DIR}/src/obstacle_index.cpp
        ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp
        ${CMAKE_SOURCE_DIR}/src/data_source.cpp)
    target_link_libraries(test_obstacle_index PRIVATE sqlite_wrapper sdl_wrapper)
    osect_apply_app_definitions(test_obstacle_index)

    add_osect_test(route_planner_terrain
        ${CMAKE_SOURCE_DIR}/src/route_planner.cpp
        ${CMAKE_SOURCE_DIR}/src/obstacle_index.cpp
        ${CMAKE_SOURCE_DIR}/src/terrain_corridor.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_source.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile_cache.cpp
        ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp
        ${CMAKE_SOURCE_DIR}/src/data_source.cpp
        ${CMAKE_SOURCE_DIR}/src/geo_math.cpp
        ${CMAKE_SOURCE_DIR}/src/flight_route.cpp
        ${CMAKE_SOURCE_DIR}/src/user_database.cpp)
    target_link_libraries(test_route_planner_terrain PRIVATE sqlite_wrapper sdl_wrapper)
    osect_apply_app_definitions(test_route_planner_terrain)

    add_osect_test(route_submitter
        ${CMAKE_SOURCE_DIR}/src/route_submitter.cpp
        ${CMAKE_SOURCE_DIR}/src/route_planner.cpp
        ${CMAKE_SOURCE_DIR}/src/obstacle_index.cpp
        ${CMAKE_SOURCE_DIR}/src/terrain_corridor.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_source.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile.cpp
        ${CMAKE_SOURCE_DIR}/src/elevation_tile_cache.cpp
        ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp
        ${CMAKE_SOURCE_DIR}/src/data_source.cpp
        ${CMAKE_SOURCE_DIR}/src/geo_math.cpp
        ${CMAKE_SOURCE_DIR}/src/flight_route.cpp
        ${CMAKE_SOURCE_DIR}/src/user_database.cpp)
    target_link_libraries(test_route_submitter PRIVATE sqlite_wrapper sdl_wrapper)
    osect_apply_app_definitions(test_route_submitter)
else()
    message(STATUS "Skipping NASR integration tests: osect.db is missing")
endif()

# app_options — command-line parsing + asset/driver resolution.
# build_ini pulls in ini_config; the resolve_* helpers and log_info
# need the sdl wrapper.
add_osect_test(app_options
    ${CMAKE_SOURCE_DIR}/src/app_options.cpp
    ${CMAKE_SOURCE_DIR}/src/ini_config.cpp)
target_link_libraries(test_app_options PRIVATE sdl_wrapper)
osect_apply_app_definitions(test_app_options)
