cmake_minimum_required(VERSION 3.16)
project(TieredOMap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Library ─────────────────────────────────────────────────────────────────

add_library(tiered_omap STATIC
    src/oram/binary_tree_storage.cpp
    src/oram/path_oram.cpp
    src/oram/da_oram.cpp
    src/omap/avl_omap.cpp
    src/omap/bplus_omap.cpp
    src/tiered_omap.cpp
)

target_include_directories(tiered_omap PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(tiered_omap PRIVATE -Wall -Wextra -Wpedantic)

# ─── Google Test ─────────────────────────────────────────────────────────────

find_package(GTest REQUIRED)

enable_testing()

add_executable(test_path_oram   tests/test_path_oram.cpp)
add_executable(test_da_oram     tests/test_da_oram.cpp)
add_executable(test_avl_omap    tests/test_avl_omap.cpp)
add_executable(test_bplus_omap  tests/test_bplus_omap.cpp)
add_executable(test_tiered_omap tests/test_tiered_omap.cpp)

foreach(tgt test_path_oram test_da_oram test_avl_omap test_bplus_omap test_tiered_omap)
    target_link_libraries(${tgt} PRIVATE tiered_omap GTest::gtest_main)
    add_test(NAME ${tgt} COMMAND ${tgt})
endforeach()

# ─── Benchmark ───────────────────────────────────────────────────────────────

add_executable(bench_tiered_omap benchmark/bench_tiered_omap.cpp)
target_link_libraries(bench_tiered_omap PRIVATE tiered_omap)
