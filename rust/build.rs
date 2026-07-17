// Compiles the cxx bridge (src/ffi.rs) plus the two C++ translation units it
// needs: the real, already-verified OrderBookV2 (cpp/src/order_book_v2.cpp)
// and the thin FFI adapter that translates to/from it
// (cpp/src/order_book_v2_ffi.cpp — see ADR-3).
//
// This is a standalone build, independent of cpp/CMakeLists.txt: `cargo
// build` from rust/ does not require CMake at all. CMake still builds and
// tests OrderBookV2 on its own (cpp/tests/, cpp/bench/) — this file just
// also compiles the same .cpp source a second time, as part of the Rust
// crate, which is the normal cxx pattern (no shared build artifacts between
// the two build systems).

fn main() {
    cxx_build::bridge("src/ffi.rs")
        .file("../cpp/src/order_book_v2.cpp")
        .file("../cpp/src/order_book_v2_ffi.cpp")
        .include("../cpp/include")
        .flag_if_supported("-std=c++17")
        .compile("matching-engine-sidecar-cxxbridge");

    println!("cargo:rerun-if-changed=src/ffi.rs");
    println!("cargo:rerun-if-changed=../cpp/src/order_book_v2.cpp");
    println!("cargo:rerun-if-changed=../cpp/src/order_book_v2_ffi.cpp");
    println!("cargo:rerun-if-changed=../cpp/include/order_book_v2.h");
    println!("cargo:rerun-if-changed=../cpp/include/order_book_v2_ffi.h");
}
