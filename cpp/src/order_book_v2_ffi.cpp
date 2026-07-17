#include "order_book_v2_ffi.h"

namespace {

Side to_side(FfiSide s) {
    return s == FfiSide::Buy ? Side::Buy : Side::Sell;
}

Type to_type(FfiOrderType t) {
    switch (t) {
        case FfiOrderType::Market: return Type::Market;
        case FfiOrderType::Cancel: return Type::Cancel;
        case FfiOrderType::Limit:
        default:                   return Type::Limit;
    }
}

} // namespace

OrderBookV2Ffi::OrderBookV2Ffi(uint32_t arena_capacity, int64_t initial_window)
    : inner_(arena_capacity, initial_window) {}

rust::Vec<FfiTrade> OrderBookV2Ffi::add(FfiOrderRequest req) {
    OrderRequest native{
        req.id,
        to_side(req.side),
        to_type(req.kind),
        req.price,
        req.qty,
    };

    std::vector<Trade> trades = inner_.add(native);

    rust::Vec<FfiTrade> out;
    for (const auto& t : trades) {
        out.push_back(FfiTrade{ t.maker_id, t.taker_id, t.price, t.qty });
    }
    return out;
}

bool OrderBookV2Ffi::cancel(uint64_t id) {
    return inner_.cancel(id);
}

bool OrderBookV2Ffi::reduce(uint64_t id, uint64_t delta) {
    return inner_.reduce(id, delta);
}

int64_t OrderBookV2Ffi::best_bid() const { return inner_.best_bid(); }
int64_t OrderBookV2Ffi::best_ask() const { return inner_.best_ask(); }
bool    OrderBookV2Ffi::has_bid() const  { return inner_.has_bid(); }
bool    OrderBookV2Ffi::has_ask() const  { return inner_.has_ask(); }
size_t  OrderBookV2Ffi::depth() const    { return inner_.depth(); }

std::unique_ptr<OrderBookV2Ffi> make_order_book(uint32_t arena_capacity, int64_t initial_window) {
    return std::make_unique<OrderBookV2Ffi>(arena_capacity, initial_window);
}
