#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Open-addressing flat hash map (linear probing, backward-shift deletion,
// no tombstones), built specifically to replace
// std::unordered_map<OrderId, IndexEntry> on OrderBookV2's hot path.
//
// Two real, measured design mistakes were made and fixed while building
// this — see devlog day 10 for the full story, both are worth reading
// before touching this file again:
//
// 1. First version hashed keys through a splitmix64 finalizer "for
//    safety against adversarial patterns." Measured result: throughput
//    dropped from std::unordered_map's ~4.8-5.1M ops/sec to ~3.1M
//    ops/sec. Real exchange-assigned OrderIds are sequential/
//    near-sequential; scrambling them destroys the cache locality that
//    sequential keys give a flat array for free (consecutive inserts
//    would otherwise land in consecutive, prefetcher-friendly slots).
//    Fixed by hashing with the identity function — deliberately not
//    "textbook safe" against adversarial key clustering, but this class
//    exists for exactly one caller with a known key distribution, and the
//    measured, direct benefit here is real. If this is ever reused
//    somewhere with a different key distribution, reconsider hash_key().
// 2. First version used tombstones for deletion, with a periodic
//    same-size "compact" rehash once tombstones passed a threshold.
//    Measured result: occasional 40-60ms stalls in an otherwise
//    ~100-250ns-per-op benchmark — a full-table rehash pass is a real,
//    visible tail-latency spike, and critically, std::unordered_map has
//    no equivalent cost at all (chained buckets erase in true O(1), no
//    sweep ever needed). Fixed by switching to backward-shift deletion
//    (the standard technique for tombstone-free linear probing): erasing
//    a slot immediately pulls back any later entry in the same probe
//    cluster that can legally move without breaking its own
//    findability, so no entry is ever unreachable and no bulk compaction
//    pass is ever needed.
//
// Not a general-purpose drop-in for std::unordered_map — no custom
// allocators, no heterogeneous lookup, no iterator invalidation
// guarantees across insert/erase. Built to do exactly what OrderBookV2
// needs: insert-or-assign (operator[]), find, erase-by-key, iterate-all
// (the rare rebase path in ensure_index_for_price), size. Verified
// against the same differential-fuzz harness (v1 vs v2) used for
// everything else in this repo, plus the TSan/ASan threaded stress test
// and the real 400K-event NASDAQ replay — see cpp/fuzz/ and devlog day 10.
template <typename K, typename V>
class FlatHashMap {
public:
    struct Entry {
        K first;
        V second;
    };

    explicit FlatHashMap(size_t initial_capacity = 16) {
        size_t cap = 16;
        while (cap < initial_capacity) cap <<= 1;
        slots_.resize(cap);
        mask_ = cap - 1;
    }

    // Pre-size for at least `n` live entries at the target load factor.
    // Same call shape as std::unordered_map::reserve, same call site in
    // OrderBookV2's constructor.
    void reserve(size_t n) {
        size_t cap = slots_.size();
        while (static_cast<double>(n) > static_cast<double>(cap) * kMaxLoad) cap <<= 1;
        if (cap > slots_.size()) rehash(cap);
    }

    // Insert-or-get, mirrors std::unordered_map::operator[].
    V& operator[](const K& key) {
        maybe_grow();
        size_t idx = probe_for_insert(key);
        Slot& s = slots_[idx];
        if (s.state == State::Occupied) return s.entry.second;
        ++live_;
        s.state = State::Occupied;
        s.entry.first = key;
        s.entry.second = V{};
        return s.entry.second;
    }

    // Returns a pointer to the live entry, or end() (== nullptr) if absent
    // — deliberately pointer-shaped so `it == index_.end()` / `it->second`
    // at call sites keep working exactly as they did against
    // std::unordered_map's iterator, with zero call-site changes needed
    // beyond erase (see below).
    Entry* find(const K& key) {
        size_t idx = probe_existing(key);
        return (idx == kNotFound) ? nullptr : &slots_[idx].entry;
    }
    static Entry* end() { return nullptr; }

    // Erase by key (not by iterator/pointer — OrderBookV2's one erase(it)
    // call site was changed to erase(id) instead, since the key is always
    // already in scope there; see order_book_v2.cpp). Backward-shift
    // deletion: no tombstones, no bulk compaction ever needed.
    bool erase(const K& key) {
        size_t i = probe_existing(key);
        if (i == kNotFound) return false;
        slots_[i].state = State::Empty;
        --live_;

        // Standard open-addressing backward-shift deletion (see e.g. the
        // "Open addressing" article's Deletion section for the reference
        // form of this exact loop): for each occupied slot j following the
        // gap at i, let k be that entry's home slot. The entry can move
        // back into the gap unless k lies cyclically in (i, j] — i.e.,
        // unless a probe search starting at k would already pass through
        // i on its way to j today, in which case moving it would make
        // that same search stop at i (now empty) too early and lose the
        // entry. Move when NOT in that range; otherwise leave it and keep
        // scanning — a later entry in the same cluster may still qualify.
        size_t j = i;
        while (true) {
            j = (j + 1) & mask_;
            if (slots_[j].state != State::Occupied) break;  // gap reached, done
            const size_t k = hash_key(slots_[j].entry.first) & mask_;
            bool k_in_range_i_j;
            if (i <= j) {
                k_in_range_i_j = (i < k) && (k <= j);
            } else {
                k_in_range_i_j = (i < k) || (k <= j);
            }
            if (!k_in_range_i_j) {
                slots_[i].entry = std::move(slots_[j].entry);
                slots_[i].state = State::Occupied;
                slots_[j].state = State::Empty;
                i = j;
            }
        }
        return true;
    }

    size_t size() const { return live_; }

    // Visits every live entry, letting the callback mutate `.second` in
    // place — used by ensure_index_for_price's rebase path (shifting every
    // open order's cached level_idx). Deliberately not a range-for-style
    // begin()/end() iterator pair: this class already uses a member
    // end() returning Entry* as the find() sentinel (so cancel()/reduce()
    // call sites read identically to the old std::unordered_map code,
    // `it == index_.end()`), and C++'s range-for rule prefers member
    // lookup for *both* begin and end the moment either exists as a
    // member — mixing that with a real iterator's end() would silently
    // resolve to the wrong overload. A named for_each_mut() sidesteps the
    // ambiguity entirely instead of fighting the language over it.
    template <typename Fn>
    void for_each_mut(Fn&& fn) {
        for (auto& s : slots_) {
            if (s.state == State::Occupied) fn(s.entry);
        }
    }

private:
    enum class State : uint8_t { Empty, Occupied };
    struct Slot {
        State state = State::Empty;
        Entry entry{};
    };

    static constexpr size_t kNotFound = static_cast<size_t>(-1);
    static constexpr double kMaxLoad = 0.70;

    std::vector<Slot> slots_;
    size_t mask_ = 0;
    size_t live_ = 0;

    // Identity hash — see the file header comment for why this beat a
    // scrambling finalizer, measured, on this class's one real workload
    // (sequential/near-sequential real exchange OrderIds).
    static size_t hash_key(const K& key) { return static_cast<size_t>(key); }

    size_t probe_for_insert(const K& key) {
        size_t idx = hash_key(key) & mask_;
        while (true) {
            Slot& s = slots_[idx];
            if (s.state == State::Empty) return idx;
            if (s.entry.first == key) return idx;
            idx = (idx + 1) & mask_;
        }
    }

    size_t probe_existing(const K& key) const {
        size_t idx = hash_key(key) & mask_;
        while (true) {
            const Slot& s = slots_[idx];
            if (s.state == State::Empty) return kNotFound;
            if (s.entry.first == key) return idx;
            idx = (idx + 1) & mask_;
        }
    }

    // No tombstones in this design, so growth is driven purely by live_
    // count against the max load factor — nothing else to account for.
    void maybe_grow() {
        if (static_cast<double>(live_ + 1) > static_cast<double>(slots_.size()) * kMaxLoad) {
            rehash(slots_.size() * 2);
        }
    }

    void rehash(size_t new_cap) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(new_cap, Slot{});
        mask_ = new_cap - 1;
        for (auto& s : old) {
            if (s.state == State::Occupied) {
                size_t idx = hash_key(s.entry.first) & mask_;
                while (slots_[idx].state == State::Occupied) idx = (idx + 1) & mask_;
                slots_[idx].state = State::Occupied;
                slots_[idx].entry = std::move(s.entry);
            }
        }
    }
};
