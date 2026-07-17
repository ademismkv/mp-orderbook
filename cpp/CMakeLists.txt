cmake_minimum_required(VERSION 3.16)
project(matching_engine CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

add_library(order_book src/order_book.cpp)
target_include_directories(order_book PUBLIC include)

add_library(order_book_v2 src/order_book_v2.cpp)
target_include_directories(order_book_v2 PUBLIC include)

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE order_book)

add_executable(test_order_book_v2 tests/test_order_book_v2.cpp)
target_link_libraries(test_order_book_v2 PRIVATE order_book_v2)

# v1 and v2 both define Side/Type/etc. at global scope, so bench/fuzz
# binaries link against exactly one of the two libraries each — never both
# in the same translation unit. See fuzz/workload.h for why differential
# fuzzing runs them as separate processes instead.
add_executable(bench_v1 bench/bench_v1.cpp)
target_include_directories(bench_v1 PRIVATE bench)
target_link_libraries(bench_v1 PRIVATE order_book)

add_executable(bench_v2 bench/bench_v2.cpp)
target_include_directories(bench_v2 PRIVATE bench)
target_link_libraries(bench_v2 PRIVATE order_book_v2)

add_executable(fuzz_v1 fuzz/fuzz_v1.cpp)
target_include_directories(fuzz_v1 PRIVATE fuzz)
target_link_libraries(fuzz_v1 PRIVATE order_book)

add_executable(fuzz_v2 fuzz/fuzz_v2.cpp)
target_include_directories(fuzz_v2 PRIVATE fuzz)
target_link_libraries(fuzz_v2 PRIVATE order_book_v2)

find_package(Threads REQUIRED)

# ADR-1: producer -> SPSC ring -> matching thread -> SPSC ring -> consumer,
# diffed against the sequential reference. Run the tsan/asan variants via
# the direct g++ commands in README (CMake doesn't wire sanitizer builds by
# default here to keep the normal Release build simple).
add_executable(test_threaded tests/test_threaded.cpp)
target_include_directories(test_threaded PRIVATE fuzz)
target_link_libraries(test_threaded PRIVATE order_book_v2 Threads::Threads)

add_executable(bench_threaded_scaling bench/bench_threaded_scaling.cpp)
target_link_libraries(bench_threaded_scaling PRIVATE order_book_v2 Threads::Threads)

# Replays a real LOBSTER-format NASDAQ message file through OrderBookV2.
# Sample data lives in data/ — see README "Real data replay" section.
add_executable(replay_lobster tools/replay_lobster.cpp)
target_link_libraries(replay_lobster PRIVATE order_book_v2)

# Same replay, per-batch chrono timing — generates the real numbers embedded
# in dashboard/index.html. See README "Live NASDAQ replay dashboard".
add_executable(replay_lobster_batched tools/replay_lobster_batched.cpp)
target_link_libraries(replay_lobster_batched PRIVATE order_book_v2)

# ADR-3: compiles the real FFI adapter (order_book_v2_ffi.cpp) against
# hand-written mocks of the headers cxx_build would otherwise generate, so
# the adapter's translation logic is verified without needing cargo/rustc.
# See README "Rust sidecar" and ADR.md's ADR-3 status.
add_executable(test_order_book_v2_ffi_standalone tests/test_order_book_v2_ffi_standalone.cpp src/order_book_v2_ffi.cpp)
target_include_directories(test_order_book_v2_ffi_standalone PRIVATE tests/ffi_mock)
target_link_libraries(test_order_book_v2_ffi_standalone PRIVATE order_book_v2)

enable_testing()
add_test(NAME basic COMMAND test_order_book)
add_test(NAME basic_v2 COMMAND test_order_book_v2)
add_test(NAME threaded COMMAND test_threaded)
add_test(NAME ffi_adapter_standalone COMMAND test_order_book_v2_ffi_standalone)
