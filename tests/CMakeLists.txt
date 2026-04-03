cmake_minimum_required(VERSION 3.24)

project(ShipNetSimTests VERSION ${ShipNetSim_VERSION} LANGUAGES CXX)

# Find Qt6 Test component
find_package(QT NAMES Qt6 REQUIRED COMPONENTS Test Core Concurrent Xml Sql Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test Core Concurrent Xml Sql Network)

# Enable Qt's automoc
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Common test setup function
function(add_shipnetsim_test test_name test_sources)
    add_executable(${test_name} ${test_sources})

    target_link_libraries(${test_name} PRIVATE
        ShipNetSimCore
        GeographicLib::GeographicLib
        GDAL::GDAL
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Concurrent
        Qt${QT_VERSION_MAJOR}::Network
        Qt${QT_VERSION_MAJOR}::Xml
        Qt${QT_VERSION_MAJOR}::Sql
    )

    # Conditionally include Container library only if BUILD_SERVER is ON
    if(BUILD_SERVER)
        target_link_libraries(${test_name} PRIVATE Container::Container)
        target_include_directories(${test_name} PRIVATE ${CONTAINER_INCLUDE_DIRS})
        target_compile_definitions(${test_name} PRIVATE BUILD_SERVER_ENABLED)
    endif()

    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/ShipNetSimCore
        ${CMAKE_BINARY_DIR}/include
    )

    # Add test to CTest
    add_test(NAME ${test_name} COMMAND ${test_name})

    # Set test timeout (5 minutes)
    # set_tests_properties(${test_name} PROPERTIES TIMEOUT 300)
endfunction()

# Individual test executables
add_shipnetsim_test(GPointTest test_gpoint.cpp)
add_shipnetsim_test(GLineTest test_gline.cpp)
add_shipnetsim_test(GSegmentTest test_gsegment.cpp)
add_shipnetsim_test(PolygonTest test_polygon.cpp)
add_shipnetsim_test(QuadtreeTest test_quadtree.cpp)
add_shipnetsim_test(HierarchicalVisibilityGraphTest test_hierarchicalvisibilitygraph.cpp)
add_shipnetsim_test(ShortestPathSyntheticTest test_shortestpath_synthetic.cpp)
add_shipnetsim_test(ShortestPathRealWorldTest test_shortestpath_realworld.cpp)
add_shipnetsim_test(BenchmarkPaper benchmark_paper.cpp)
add_shipnetsim_test(BenchmarkFlatVsHier benchmark_flat_vs_hier.cpp)
add_shipnetsim_test(BenchmarkNoCache benchmark_nocache.cpp)
add_shipnetsim_test(BenchmarkNoCacheAllRoutes benchmark_nocache_allroutes.cpp)

# # Real-world tests need longer timeout (HVG build can take several minutes
# # for ne_10m_ocean.shp with 446K vertices). Set both CTest timeout and
# # Qt Test function watchdog (QTEST_FUNCTION_TIMEOUT).
# set_tests_properties(ShortestPathRealWorldTest PROPERTIES
#     TIMEOUT 1200
#     ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=1200000"
# )

# Copy test data files to build directory
file(COPY ${CMAKE_SOURCE_DIR}/src/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
