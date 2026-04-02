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
    src/omap/da_ost_omap.cpp
    src/tiered_omap.cpp
    src/network/tcp_channel.cpp
    src/network/network_storage.cpp
    src/network/storage_server.cpp
    src/bench_setup.cpp
    src/tee/packed_directory.cpp
    src/tee/enclave_oram.cpp
    src/tee/tee_avl_omap.cpp
    src/tee/tee_omap.cpp
    src/tee/tee_server.cpp
)

target_include_directories(tiered_omap PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(tiered_omap PRIVATE -Wall -Wextra -Wpedantic)

# ─── OpenSSL (AES-128 encryption) ────────────────────────────────────────────

find_package(OpenSSL REQUIRED)
target_link_libraries(tiered_omap PUBLIC OpenSSL::Crypto)

# ─── Google Test (optional) ───────────────────────────────────────────────────

find_package(GTest QUIET)

if(GTest_FOUND)
    enable_testing()

    add_executable(test_path_oram    tests/test_path_oram.cpp)
    add_executable(test_da_oram      tests/test_da_oram.cpp)
    add_executable(test_avl_omap     tests/test_avl_omap.cpp)
    add_executable(test_bplus_omap   tests/test_bplus_omap.cpp)
    add_executable(test_da_ost_omap  tests/test_da_ost_omap.cpp)
    add_executable(test_da_ost_maint tests/test_da_ost_maint.cpp)
    add_executable(test_tiered_omap  tests/test_tiered_omap.cpp)

    add_executable(test_network      tests/test_network.cpp)
    add_executable(test_tee_omap     tests/test_tee_omap.cpp)

    foreach(tgt test_path_oram test_da_oram test_avl_omap test_bplus_omap test_da_ost_omap test_da_ost_maint test_tiered_omap test_network test_tee_omap)
        target_link_libraries(${tgt} PRIVATE tiered_omap GTest::gtest_main)
        add_test(NAME ${tgt} COMMAND ${tgt})
    endforeach()
else()
    message(STATUS "GTest not found — skipping test targets")
endif()

# ─── Benchmark ───────────────────────────────────────────────────────────────

add_executable(bench_tiered_omap benchmark/bench_tiered_omap.cpp)
target_link_libraries(bench_tiered_omap PRIVATE tiered_omap)

add_executable(bench_dynamic benchmark/bench_dynamic.cpp)
target_link_libraries(bench_dynamic PRIVATE tiered_omap)

add_executable(bench_latency benchmark/bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE tiered_omap)

add_executable(bench_comptime benchmark/bench_comptime.cpp)
target_link_libraries(bench_comptime PRIVATE tiered_omap)

add_executable(bench_rounds benchmark/bench_rounds.cpp)
target_link_libraries(bench_rounds PRIVATE tiered_omap)

add_executable(bench_large benchmark/bench_large.cpp)
target_link_libraries(bench_large PRIVATE tiered_omap)

add_executable(bench_paper benchmark/bench_paper.cpp)
target_link_libraries(bench_paper PRIVATE tiered_omap)

add_executable(bench_tee benchmark/bench_tee.cpp)
target_link_libraries(bench_tee PRIVATE tiered_omap)

add_executable(bench_tee_e2e benchmark/bench_tee_e2e.cpp)
target_link_libraries(bench_tee_e2e PRIVATE tiered_omap)

# ─── ORAM Server ─────────────────────────────────────────────────────────────

add_executable(oram_server tools/oram_server.cpp)
target_link_libraries(oram_server PRIVATE tiered_omap)

add_executable(tee_server tools/tee_server_main.cpp)
target_link_libraries(tee_server PRIVATE tiered_omap)
