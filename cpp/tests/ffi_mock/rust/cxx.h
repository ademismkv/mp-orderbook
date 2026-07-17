#pragma once
// Hand-written stand-in for cxx_build's generated rust/cxx.h — just enough of
// rust::Vec<T>'s interface for order_book_v2_ffi.cpp to compile and run
// against, so the real adapter can be verified WITHOUT cargo/rustc. Not a
// claim that this matches cxx's real ABI/ownership semantics exactly (it
// doesn't need to for this purpose — see cpp/tests/test_order_book_v2_ffi_standalone.cpp).
#include <vector>
#include <cstddef>

namespace rust {
template <typename T>
class Vec {
public:
    void push_back(const T& v) { data_.push_back(v); }
    size_t size() const { return data_.size(); }
    const T& operator[](size_t i) const { return data_[i]; }
    T& operator[](size_t i) { return data_[i]; }
    auto begin() { return data_.begin(); }
    auto end() { return data_.end(); }
    auto begin() const { return data_.begin(); }
    auto end() const { return data_.end(); }
private:
    std::vector<T> data_;
};
} // namespace rust
