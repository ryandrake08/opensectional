# Unit tests using doctest. Each test binary is small — list its source
# dependencies explicitly rather than linking the whole app.
#
# add_nasrbrowse_test(<name> [extra_sources...])
# Creates an executable test_<name> from test_<name>.cpp plus any extra
# sources (e.g. the module-under-test's .cpp), and registers a ctest entry
# named <name> run with the repo root as its working directory (so relative
# paths like "nasr.db" resolve).
function(add_nasrbrowse_test name)
    add_executable(test_${name} test_${name}.cpp ${ARGN})
    target_compile_features(test_${name} PRIVATE cxx_std_17)
    target_include_directories(test_${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/thirdparty
    )
    target_compile_options(test_${name} PRIVATE -Wall -Wextra
        $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
    )
    target_link_options(test_${name} PRIVATE
        $<$<CONFIG:Debug>:${DEBUG_COMPILE_OPTIONS}>
        $<$<CONFIG:RelWithDebInfo>:${RELWITHDEBINFO_COMPILE_OPTIONS}>
    )
    add_test(NAME ${name}
             COMMAND test_${name}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

# Pure-header tests (no extra sources needed)
add_nasrbrowse_test(lru_set)
add_nasrbrowse_test(altitude_filter)

# Tests that compile one .cpp alongside the test
add_nasrbrowse_test(geo_math    ${CMAKE_SOURCE_DIR}/src/geo_math.cpp)
add_nasrbrowse_test(map_view    ${CMAKE_SOURCE_DIR}/src/map_view.cpp)
add_nasrbrowse_test(ini_config  ${CMAKE_SOURCE_DIR}/src/ini_config.cpp)

# flight_route pulls in the DB layer and needs the sqlite wrapper
add_nasrbrowse_test(flight_route
    ${CMAKE_SOURCE_DIR}/src/flight_route.cpp
    ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp)
target_link_libraries(test_flight_route PRIVATE sqlite_wrapper)
