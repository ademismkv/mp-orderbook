cmake_minimum_required(VERSION 3.16)
project(matching_engine CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# Cross-TU inlining + native codegen. Neither changes any observable
# behavior (verified: 16/16 v2 unit tests, 10/10 v1 unit tests, 8/8
# differential fuzz seeds byte-identical vs. a non-LTO/non-native build
# (both the standard fixed-band workload and the rebase-forcing price-walk
# workload, including IOC/FOK/PostOnly), threaded pipeline still matches
# the sequential reference under plain and TSan builds — see devlog).
# Measured ~5% throughput gain on a 2M-op single-thread run;
# -march=native trades portability of the built binary for this, which is
# fine for a benchmark/demo target built and run on the same machine.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  add_compile_options(-march=native)
  add_link_options(-march=native)
endif()

add_library(order_book src/order_book.cpp)
target_include_directories(order_book PUBLIC include)

add_library(order_book_v2 src/order_book_v2.cpp)
target_include_directories(order_book_v2 PUBLIC include)

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE order_book)

add_executable(test_order_book_v2 tests/test_order_book_v2.cpp)
target_link_libraries(test_order_book_v2 PRIVATE order_book_v2)

# Turns "the arena design means no malloc on the hot path" from a claim
# into something actually checked: overrides global operator new/delete
# for this one executable and counts calls across 200,000 steady-state
# add/cancel/reduce/cross ops. Found a real gap doing this — see
# tests/test_zero_alloc.cpp and order_book_v2.cpp's free_list_.reserve().
add_executable(test_zero_alloc tests/test_zero_alloc.cpp)
target_link_libraries(test_zero_alloc PRIVATE order_book_v2)

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

# Feed Handler: real Nasdaq TotalView-ITCH 5.0 message parsing. Separate
# library from order_book — this stage doesn't know about OrderBookV2 yet
# (see ROADMAP.md V3 for the Sequencer/Risk stages that will sit between
# the two).
#
# itch_simd_neon.cpp uses <arm_neon.h> directly (no portable dispatch
# written — see devlog day 16: this repo's real hardware is ARM64, and
# writing an AVX2 path too would mean shipping code never actually
# compiled or measured, the thing this repo has avoided everywhere else).
# Only built/tested on ARM so a clone on x86 still builds clean instead of
# failing on a missing header.
set(ITCH_FEED_SOURCES feed/itch_parser.cpp feed/sequencer.cpp feed/risk.cpp)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  list(APPEND ITCH_FEED_SOURCES feed/itch_simd_neon.cpp)
  set(ITCH_HAVE_NEON ON)
else()
  message(STATUS "itch_simd_neon.cpp skipped: CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR} is not ARM64 — see devlog day 16")
endif()

add_library(itch_feed ${ITCH_FEED_SOURCES})
target_include_directories(itch_feed PUBLIC feed)
# sequencer.cpp is the Feed -> Sequencer -> Order Books wiring (ROADMAP.md
# V3) — it's the first thing in cpp/feed/ that actually depends on
# OrderBookV2, so itch_feed needs the engine's headers/library too.
target_link_libraries(itch_feed PUBLIC order_book_v2)

add_executable(test_itch_parser feed/tests/test_itch_parser.cpp)
target_link_libraries(test_itch_parser PRIVATE itch_feed)

# MoldUDP64 session/gap-recovery layer is header-only (moldudp64.h,
# moldudp64_session.h) — this test links itch_feed only to reuse
# BinaryFileReader for pulling real message bytes as test payloads.
add_executable(test_moldudp64 feed/tests/test_moldudp64.cpp)
target_link_libraries(test_moldudp64 PRIVATE itch_feed)

if(ITCH_HAVE_NEON)
  add_executable(test_itch_simd_neon feed/tests/test_itch_simd_neon.cpp)
  target_link_libraries(test_itch_simd_neon PRIVATE itch_feed)
endif()

# Feed -> Sequencer -> Order Books, the wiring ROADMAP.md's V3 called for.
add_executable(test_sequencer_wiring feed/tests/test_sequencer_wiring.cpp)
target_link_libraries(test_sequencer_wiring PRIVATE itch_feed)

find_package(Threads REQUIRED)
# Real UDP multicast + unicast retransmission over loopback — genuine
# socket code, not simulated. See devlog for why loopback stands in for a
# live exchange feed here, and for the ENODEV/IP_MULTICAST_IF fix this
# sandbox needed (no default route for the kernel to resolve INADDR_ANY
# against — real deployments bind to the actual NIC instead).
add_executable(test_udp_multicast_e2e feed/tests/test_udp_multicast_e2e.cpp)
target_link_libraries(test_udp_multicast_e2e PRIVATE itch_feed Threads::Threads)

# Real-socket exercise of gap detection triggered purely by a heartbeat
# during an idle period — the scenario test_udp_multicast_e2e.cpp doesn't
# cover (there, the gap is discovered by the next real message arriving
# early). See ADR-5 and devlog day 21.
add_executable(test_udp_multicast_heartbeat feed/tests/test_udp_multicast_heartbeat.cpp)
target_link_libraries(test_udp_multicast_heartbeat PRIVATE itch_feed Threads::Threads)

# Market Data Feed — ROADMAP.md's last pipeline stage. Runs the full real
# file through Sequencer -> Risk -> Order Books -> Execution Reports (same
# as test_sequencer_wiring), capturing every public market data event, then
# actually publishes it over real UDP multicast wrapped in real MoldUDP64
# framing (reusing the day 14/15 transport), with a second thread
# subscribing independently and reconstructing a shadow book from the wire
# feed alone. See devlog day 19.
add_executable(test_market_data_feed feed/tests/test_market_data_feed.cpp)
target_link_libraries(test_market_data_feed PRIVATE itch_feed Threads::Threads)

# Real snapshot recovery: a subscriber that joins the feed mid-stream (never
# sees the first half of real event history, not even via retransmission)
# bootstraps from a real TCP snapshot exchange (snapshot.h, tcp_socket.h)
# instead, then finishes via the live second half of the same real UDP
# multicast feed. See ADR-5 and devlog day 22 for why this, unlike MoldUDP64
# "session establishment," is a real gap this repo had.
add_executable(test_snapshot_recovery feed/tests/test_snapshot_recovery.cpp)
target_link_libraries(test_snapshot_recovery PRIVATE itch_feed Threads::Threads)

# ADR-1's "symbol -> hash -> shard -> single writer" threading model,
# proven against the real ITCH pipeline (Sequencer -> Risk -> Order Books),
# not just the synthetic bench_threaded_scaling.cpp workload. Runs the real
# file twice — sequential reference, then 4-way symbol-sharded concurrent —
# and requires them to agree exactly. See devlog day 22c.
add_executable(test_concurrent_sharded_pipeline feed/tests/test_concurrent_sharded_pipeline.cpp)
target_link_libraries(test_concurrent_sharded_pipeline PRIVATE itch_feed Threads::Threads)

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
add_test(NAME zero_alloc COMMAND test_zero_alloc)
add_test(NAME threaded COMMAND test_threaded)
add_test(NAME ffi_adapter_standalone COMMAND test_order_book_v2_ffi_standalone)
# Runs from the build directory by default via CTest's WORKING_DIRECTORY,
# so point it at the real data file relative to the repo root.
add_test(NAME itch_parser_real_data COMMAND test_itch_parser "${CMAKE_SOURCE_DIR}/../data/itch50_sample_20191230.bin")
add_test(NAME moldudp64_session COMMAND test_moldudp64 "${CMAKE_SOURCE_DIR}/../data/itch50_sample_20191230.bin")
add_test(NAME udp_multicast_e2e COMMAND test_udp_multicast_e2e "${CMAKE_SOURCE_DIR}/../data/itch50_sample_20191230.bin")
add_test(NAME udp_multicast_heartbeat COMMAND test_udp_multicast_heartbeat "${CMAKE_SOURCE_DIR}/../data/itch50_sample_20191230.bin")
if(ITCH_HAVE_NEON)
  add_test(NAME itch_simd_neon COMMAND test_itch_simd_neon "${CMAKE_SOURCE_DIR}/../data/itch50_sample_20191230.bin")
endif()
add_test(NAME sequencer_wiring COMMAND test_sequencer_wiring "${CMAKE_SOURCE_DIR}/../data/itch50_sample_20191230.bin")
add_test(NAME market_data_feed COMMAND test_market_data_feed "${CMAKE_SOURCE_DIR}/../data/itch50_sample_20191230.bin")
add_test(NAME snapshot_recovery COMMAND test_snapshot_recovery "${CMAKE_SOURCE_DIR}/../data/itch50_sample_20191230.bin")
add_test(NAME concurrent_sharded_pipeline COMMAND test_concurrent_sharded_pipeline "${CMAKE_SOURCE_DIR}/../data/itch50_sample_20191230.bin")
