cmake_minimum_required(VERSION 3.16)
project(ll_cmaf_ingest CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Main ingest engine executable
# ---------------------------------------------------------------------------
add_executable(ll_cmaf_ingest
    src/app/ingest_main.cpp
)
target_include_directories(ll_cmaf_ingest PRIVATE src)
target_link_libraries(ll_cmaf_ingest PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Unit tests (custom lightweight runner, see tests/test_main.cpp)
# ---------------------------------------------------------------------------
add_executable(ll_cmaf_tests
    tests/test_main.cpp
    tests/test_hevc_sps_parser.cpp
    tests/test_mp4_boxes.cpp
    tests/test_jitter_buffer.cpp
    tests/test_hevc_depacketizer.cpp
)
target_include_directories(ll_cmaf_tests PRIVATE src)
target_link_libraries(ll_cmaf_tests PRIVATE Threads::Threads)

enable_testing()
add_test(NAME unit_tests COMMAND ll_cmaf_tests)
