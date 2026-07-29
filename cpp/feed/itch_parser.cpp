#include "itch_messages.h"

namespace itch {

size_t expected_length(char type) {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;   // Operational Halt — lowercase, see itch_messages.h
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        case 'O': return 48;
        default: return 0;
    }
}

std::optional<Message> parse_message(const uint8_t* data, size_t len) {
    if (len == 0) return std::nullopt;
    const char type = static_cast<char>(data[0]);
    const size_t need = expected_length(type);
    if (need == 0 || len < need) return std::nullopt;

    const CommonHeader h = read_common_header(data);

    switch (type) {
        case 'S': {
            SystemEvent m{h, static_cast<char>(data[11])};
            return m;
        }
        case 'R': {
            StockDirectory m{};
            m.h = h;
            m.stock = read_alpha<8>(data + 11);
            m.market_category = static_cast<char>(data[19]);
            m.financial_status_indicator = static_cast<char>(data[20]);
            m.round_lot_size = be32(data + 21);
            m.round_lots_only = static_cast<char>(data[25]);
            m.issue_classification = static_cast<char>(data[26]);
            m.issue_subtype = read_alpha<2>(data + 27);
            m.authenticity = static_cast<char>(data[29]);
            m.short_sale_threshold_indicator = static_cast<char>(data[30]);
            m.ipo_flag = static_cast<char>(data[31]);
            m.luld_reference_price_tier = static_cast<char>(data[32]);
            m.etp_flag = static_cast<char>(data[33]);
            m.etp_leverage_factor = be32(data + 34);
            m.inverse_indicator = static_cast<char>(data[38]);
            return m;
        }
        case 'H': {
            StockTradingAction m{};
            m.h = h;
            m.stock = read_alpha<8>(data + 11);
            m.trading_state = static_cast<char>(data[19]);
            m.reserved = static_cast<char>(data[20]);
            m.reason = read_alpha<4>(data + 21);
            return m;
        }
        case 'Y': {
            RegSHORestriction m{};
            m.h = h;
            m.stock = read_alpha<8>(data + 11);
            m.reg_sho_action = static_cast<char>(data[19]);
            return m;
        }
        case 'L': {
            MarketParticipantPosition m{};
            m.h = h;
            m.mpid = read_alpha<4>(data + 11);
            m.stock = read_alpha<8>(data + 15);
            m.primary_market_maker = static_cast<char>(data[23]);
            m.market_maker_mode = static_cast<char>(data[24]);
            m.market_participant_state = static_cast<char>(data[25]);
            return m;
        }
        case 'V': {
            MWCBDeclineLevel m{};
            m.h = h;
            m.level1 = be64(data + 11);
            m.level2 = be64(data + 19);
            m.level3 = be64(data + 27);
            return m;
        }
        case 'W': {
            MWCBStatus m{h, static_cast<char>(data[11])};
            return m;
        }
        case 'K': {
            IPOQuotingPeriodUpdate m{};
            m.h = h;
            m.stock = read_alpha<8>(data + 11);
            m.ipo_quotation_release_time = be32(data + 19);
            m.ipo_quotation_release_qualifier = static_cast<char>(data[23]);
            m.ipo_price = be32(data + 24);
            return m;
        }
        case 'J': {
            LULDAuctionCollar m{};
            m.h = h;
            m.stock = read_alpha<8>(data + 11);
            m.auction_collar_reference_price = be32(data + 19);
            m.upper_auction_collar_price = be32(data + 23);
            m.lower_auction_collar_price = be32(data + 27);
            m.auction_collar_extension = be32(data + 31);
            return m;
        }
        case 'h': {
            OperationalHalt m{};
            m.h = h;
            m.stock = read_alpha<8>(data + 11);
            m.market_code = static_cast<char>(data[19]);
            m.operational_halt_action = static_cast<char>(data[20]);
            return m;
        }
        case 'A': {
            AddOrderNoMPID m{};
            m.h = h;
            m.order_ref_number = be64(data + 11);
            m.buy_sell_indicator = static_cast<char>(data[19]);
            m.shares = be32(data + 20);
            m.stock = read_alpha<8>(data + 24);
            m.price = be32(data + 32);
            return m;
        }
        case 'F': {
            AddOrderMPID m{};
            m.h = h;
            m.order_ref_number = be64(data + 11);
            m.buy_sell_indicator = static_cast<char>(data[19]);
            m.shares = be32(data + 20);
            m.stock = read_alpha<8>(data + 24);
            m.price = be32(data + 32);
            m.attribution = read_alpha<4>(data + 36);
            return m;
        }
        case 'E': {
            OrderExecuted m{};
            m.h = h;
            m.order_ref_number = be64(data + 11);
            m.executed_shares = be32(data + 19);
            m.match_number = be64(data + 23);
            return m;
        }
        case 'C': {
            OrderExecutedWithPrice m{};
            m.h = h;
            m.order_ref_number = be64(data + 11);
            m.executed_shares = be32(data + 19);
            m.match_number = be64(data + 23);
            m.printable = static_cast<char>(data[31]);
            m.execution_price = be32(data + 32);
            return m;
        }
        case 'X': {
            OrderCancel m{};
            m.h = h;
            m.order_ref_number = be64(data + 11);
            m.canceled_shares = be32(data + 19);
            return m;
        }
        case 'D': {
            OrderDelete m{};
            m.h = h;
            m.order_ref_number = be64(data + 11);
            return m;
        }
        case 'U': {
            OrderReplace m{};
            m.h = h;
            m.original_order_ref_number = be64(data + 11);
            m.new_order_ref_number = be64(data + 19);
            m.shares = be32(data + 27);
            m.price = be32(data + 31);
            return m;
        }
        case 'P': {
            TradeNonCross m{};
            m.h = h;
            m.order_ref_number = be64(data + 11);
            m.buy_sell_indicator = static_cast<char>(data[19]);
            m.shares = be32(data + 20);
            m.stock = read_alpha<8>(data + 24);
            m.price = be32(data + 32);
            m.match_number = be64(data + 36);
            return m;
        }
        case 'Q': {
            CrossTrade m{};
            m.h = h;
            m.shares = be64(data + 11);   // 8 bytes — see itch_messages.h
            m.stock = read_alpha<8>(data + 19);
            m.cross_price = be32(data + 27);
            m.match_number = be64(data + 31);
            m.cross_type = static_cast<char>(data[39]);
            return m;
        }
        case 'B': {
            BrokenTrade m{h, be64(data + 11)};
            return m;
        }
        case 'I': {
            NOII m{};
            m.h = h;
            m.paired_shares = be64(data + 11);
            m.imbalance_shares = be64(data + 19);
            m.imbalance_direction = static_cast<char>(data[27]);
            m.stock = read_alpha<8>(data + 28);
            m.far_price = be32(data + 36);
            m.near_price = be32(data + 40);
            m.current_reference_price = be32(data + 44);
            m.cross_type = static_cast<char>(data[48]);
            m.price_variation_indicator = static_cast<char>(data[49]);
            return m;
        }
        case 'N': {
            RPII m{};
            m.h = h;
            m.stock = read_alpha<8>(data + 11);
            m.interest_flag = static_cast<char>(data[19]);
            return m;
        }
        case 'O': {
            DirectListingCapitalRaise m{};
            m.h = h;
            m.stock = read_alpha<8>(data + 11);
            m.open_eligibility_status = static_cast<char>(data[19]);
            m.minimum_allowable_price = be32(data + 20);
            m.maximum_allowable_price = be32(data + 24);
            m.near_execution_price = be32(data + 28);
            m.near_execution_time = be64(data + 32);
            m.lower_price_range_collar = be32(data + 40);
            m.upper_price_range_collar = be32(data + 44);
            return m;
        }
        default:
            return std::nullopt;   // unreachable given the expected_length() guard above
    }
}

} // namespace itch
