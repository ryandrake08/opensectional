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
    add_test(NAME ${name}
             COMMAND test_${name}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endfunction()

# Pure-header tests (no extra sources needed)
add_osect_test(lru_set)
add_osect_test(altitude_filter)

# Tests that compile one .cpp alongside the test
add_osect_test(geo_math     ${CMAKE_SOURCE_DIR}/src/geo_math.cpp)
add_osect_test(map_view     ${CMAKE_SOURCE_DIR}/src/map_view.cpp)
add_osect_test(ini_config   ${CMAKE_SOURCE_DIR}/src/ini_config.cpp)
add_osect_test(data_source  ${CMAKE_SOURCE_DIR}/src/data_source.cpp)

# http_client confirms that transport failures throw
add_osect_test(http_client  ${CMAKE_SOURCE_DIR}/src/http_client.cpp)
target_link_libraries(test_http_client PRIVATE ${CURL_STATIC_LIBRARIES})
target_link_directories(test_http_client PRIVATE ${CURL_STATIC_LIBRARY_DIRS})
target_link_options(test_http_client PRIVATE ${CURL_LINK_OPTIONS})
target_include_directories(test_http_client PRIVATE ${CURL_STATIC_INCLUDE_DIRS})
target_compile_options(test_http_client PRIVATE ${CURL_STATIC_CFLAGS_OTHER})

# flight_route pulls in the DB layer and needs the sqlite wrapper
add_osect_test(flight_route
    ${CMAKE_SOURCE_DIR}/src/flight_route.cpp
    ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp
    ${CMAKE_SOURCE_DIR}/src/data_source.cpp
    ${CMAKE_SOURCE_DIR}/src/route_planner.cpp
    ${CMAKE_SOURCE_DIR}/src/geo_math.cpp)
target_link_libraries(test_flight_route PRIVATE sqlite_wrapper)

add_osect_test(route_planner
    ${CMAKE_SOURCE_DIR}/src/route_planner.cpp
    ${CMAKE_SOURCE_DIR}/src/route_plan_config.cpp
    ${CMAKE_SOURCE_DIR}/src/ini_config.cpp
    ${CMAKE_SOURCE_DIR}/src/nasr_database.cpp
    ${CMAKE_SOURCE_DIR}/src/data_source.cpp
    ${CMAKE_SOURCE_DIR}/src/geo_math.cpp
    ${CMAKE_SOURCE_DIR}/src/flight_route.cpp)
target_link_libraries(test_route_planner PRIVATE sqlite_wrapper)
