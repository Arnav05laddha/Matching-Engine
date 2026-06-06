cmake_minimum_required(VERSION 3.14)
project(hyper-engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release: maximum optimisation. Debug: assertions active, no optimisation.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /GL /DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG   "/Od /Zi")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g")
endif()

# ─────────────────────────────────────────────
# Main engine executable
# ─────────────────────────────────────────────
add_executable(hyper-engine src/main.cpp src/core/OrderBook.cpp)

# ─────────────────────────────────────────────
# Benchmark binary (standalone, no GTest)
# ─────────────────────────────────────────────
add_executable(bench tests/Benchmark.cpp src/core/OrderBook.cpp)

# ─────────────────────────────────────────────
# Google Test
# ─────────────────────────────────────────────
enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# Shared compile options for all test targets
set(COMMON_TEST_LIBS gtest_main)
set(ORDERBOOK_SRC    src/core/OrderBook.cpp)

# ── test_order_pool: OrderPool allocation/freelist ──────────────────────────
add_executable(test_order_pool tests/test_order_pool.cpp)
target_link_libraries(test_order_pool ${COMMON_TEST_LIBS})

# ── test_order_book: Legacy day-by-day correctness tests ────────────────────
add_executable(test_order_book tests/test_order_book.cpp ${ORDERBOOK_SRC})
target_link_libraries(test_order_book ${COMMON_TEST_LIBS})

# ── test_correctness: Comprehensive correctness (structure, matching, cancel) ─
add_executable(test_correctness tests/test_correctness.cpp ${ORDERBOOK_SRC})
target_link_libraries(test_correctness ${COMMON_TEST_LIBS})

# ── test_spsc_queue: Lock-free queue single & multi-thread tests ─────────────
add_executable(test_spsc_queue tests/test_spsc_queue.cpp)
target_link_libraries(test_spsc_queue ${COMMON_TEST_LIBS})

# ── test_stress: End-to-end, realistic MBO simulation, perf measurement ──────
add_executable(test_stress tests/test_stress.cpp ${ORDERBOOK_SRC})
target_link_libraries(test_stress ${COMMON_TEST_LIBS})

# Windows: link against threading library
if(WIN32)
    target_link_libraries(test_stress     winmm)
    target_link_libraries(test_spsc_queue winmm)
endif()

# ─────────────────────────────────────────────
# Register all GTest targets with CTest
# ─────────────────────────────────────────────
include(GoogleTest)
gtest_discover_tests(test_order_pool)
gtest_discover_tests(test_order_book)
gtest_discover_tests(test_correctness)
gtest_discover_tests(test_spsc_queue)
gtest_discover_tests(test_stress)
