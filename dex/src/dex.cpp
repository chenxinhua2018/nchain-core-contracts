#include <dex.hpp>
#include <utils.hpp>

using namespace eosio;
using namespace std;


constexpr name BANK = "eosio.token"_n;

namespace order_type {
   static const string_view LIMIT_PRICE = "limit_price";
   static const string_view MARKET_PRICE = "market_price";
   static const set<string_view> MODES = {
      LIMIT_PRICE, MARKET_PRICE
   };
   inline bool is_valid(const string_view &mode) {
      return MODES.count(mode);
   }
}

namespace order_side {
   static const string_view BUY = "buy";
   static const string_view SELL = "sell";
   static const set<string_view> MODES = {
      BUY, SELL
   };
   inline bool is_valid(const string_view &mode) {
      return MODES.count(mode);
   }
}

namespace open_mode {
   static const string_view PUBLIC = "public";
   static const string_view PRIVATE = "private";
   static const set<string_view> MODES = {
      PUBLIC, PRIVATE
   };
   inline bool is_valid(const string_view &mode) {
      return MODES.count(mode);
   }
}

uint64_t parse_price(string_view str) {
   safe<int64_t> ret;
   to_int(str, ret);
   // TODO: check range of price
   return ret.value;
}

uint64_t parse_ratio(string_view str) {
   safe<int64_t> ret;
   to_int(str, ret);
   // TODO: check range of ratio
   return ret.value;
}

string parse_order_type(string_view str) {
    check(order_type::is_valid(str), "invalid order_type=" + string{str});
    return string{str};
}

string parse_order_side(string_view str) {
    check(order_side::is_valid(str), "invalid order_side=" + string{str});
    return string{str};
}

string parse_open_mode(string_view str) {
    check(open_mode::is_valid(str), "invalid open_mode=" + string{str});
    return string{str};
}

void dex::ontransfer(name from, name to, asset quantity, string memo) {
    constexpr string_view DEPOSIT_TO = "deposit to:";
    constexpr string_view EXCHANGE   = "exchange:";

    if (from == get_self())
        return; // transfer out from this contract
    check(to == get_self(), "Must transfer to this contract");
    check(quantity.amount >= 0, "quantity must be positive");
    auto bank = get_first_receiver();
    // TODO: check bank
    check(bank == BANK, "the bank must be " + BANK.to_string());

    vector<string_view> params = split(memo, ":");
    if (params.size() == 7 && params[0] == "order") {
      // order:<type>:<side>:<coin_quant>:<asset_quant>:<price>:<ex_id>
        order_t order;
        order.order_type = parse_order_type(params[1]);
        order.order_side = parse_order_side(params[2]);
        order.coin_quant = asset_from_string(params[3]);
        order.asset_quant = asset_from_string(params[4]);
        order.price = parse_price(params[5]);
        order.deal_coin_amount = 0;
        order.deal_asset_amount = 0;

        // TODO: check coin_pair exist

        // check amount
        if (order.order_type == order_type::MARKET_PRICE && order.order_side == order_side::BUY) {
            check(order.coin_quant.amount == quantity.amount, "coin amount must be equal to the transfer quantity");
            // TODO: check coin amount range
            check(order.asset_quant.amount == 0, "asset amount must be 0 for market buy order");
        } else {
            check(order.coin_quant.amount == 0, "coin amount must be 0");
            check(order.asset_quant.amount == quantity.amount, "asset amount must be equal to the transfer quantity");
            // TODO: check asset amount range
        }

        // TODO: need to add the total order coin/asset amount?

        order_table order_tbl(get_self(), get_self().value);
        order.order_id = order_tbl.available_primary_key();
        order.owner = from;

        check(order_tbl.find(order.order_id) == order_tbl.end(), "The order is exist. order_id=" + std::to_string(order.order_id));

        order_tbl.emplace( from, [&]( auto& o ) {
            o = order;
        });
    }
    if (params.size() == 8 && params[0] == "ex") {
        // ex:<ex_id>:<owner>:<payee>:<open_mode>:<maker_ratio>:<taker_ratio>:<url>:<memo>
        exchange_t exchange;
        exchange.ex_id       = name(params[1]);
        exchange.owner       = name(params[2]);
        exchange.payee       = name(params[3]);
        exchange.open_mode   = parse_open_mode(params[4]);
        exchange.maker_ratio = parse_ratio(params[5]);
        exchange.taker_ratio = parse_ratio(params[6]);
        exchange.url         = string{params[7]};
        exchange.memo        = string{params[8]};

        exchange_table ex_tbl(get_self(), get_self().value);
        const auto &it = ex_tbl.find(exchange.primary_key());
        check(it == ex_tbl.end(), "The exchange is exist. ex_id=" + exchange.ex_id.to_string());
        // TODO: process reg exchange fee
        ex_tbl.emplace( from, [&]( auto& ex ) {
            ex = exchange;
        });
    }
}

string get_taker_side(const dex::order_t &buy_order, const dex::order_t &sell_order) {
    string taker_side;
    if (buy_order.order_type != sell_order.order_type) {
        if (buy_order.order_type == order_type::MARKET_PRICE) {
            taker_side = order_side::BUY;
        } else {
            // assert(sell_order.order_type == order_type::MARKET_PRICE);
            taker_side = order_side::SELL;
        }
    } else { // buy_order.order_type == sell_order.order_type
        taker_side = (buy_order.order_id < sell_order.order_id) ? order_side::SELL : order_side::BUY;
    }
    return taker_side;
}

// TODO: ...
static const uint64_t DEX_FRICTION_FEE_RATIO = 10; // 0.001%

uint64_t calc_friction_fee(uint64_t amount) {

    uint128_t fee = divide_decimal(amount, DEX_FRICTION_FEE_RATIO, RATIO_PRECISION);

    check(fee <= std::numeric_limits<uint64_t>::max(), "overflow exception for calc friction fee");
    check(fee <= amount, "invalid friction fee ratio");
    return (uint64_t)fee;
}

uint64_t calc_match_fee(const dex::order_t &order, const dex::exchange_t &exchange, const string &taker_side, uint64_t amount) {

    uint64_t ratio = 0;
    // TODO: order custom exchange params
    if (order.order_side == taker_side) {
        ratio = exchange.taker_ratio;
    } else {
        ratio = exchange.maker_ratio;
    }

    uint128_t fee = divide_decimal(amount, ratio, RATIO_PRECISION);

    check(fee <= std::numeric_limits<uint64_t>::max(), "overflow exception for calc match fee");
    check(fee <= amount, "invalid match fee ratio=" + std::to_string(ratio));
    return (uint64_t)fee;
}

uint64_t sub_fee(uint64_t &amount, uint64_t fee, const string &msg) {
    check(amount > fee, "the fee exeed the amount of " + msg);
    return amount - fee;
}

void transfer_out(const name &contract, const name &bank, const name &to, const asset &quantity,
                  const string &memo) {

    action(permission_level{contract, "active"_n}, bank, "transfer"_n,
           std::make_tuple(contract, to, quantity, memo))
        .send();
}

void dex::settle(const uint64_t &buy_id, const uint64_t &sell_id, const uint64_t &price,
                 const asset &coin_quant, const asset &asset_quant, const string &memo) {
    // require(matcher)

        // CCacheWrapper &cw = *context.pCw; CValidationState &state = *context.pState;
        // FeatureForkVersionEnum version = GetFeatureForkVersion(context.height);

    //1.1 get and check buy_order and sell_order

    order_table order_tbl(get_self(), get_self().value);
    auto buy_order = order_tbl.get(buy_id);
    auto sell_order = order_tbl.get(sell_id);

    // // 1.2 get exchange of order

    exchange_table ex_tbl(get_self(), get_self().value);
    auto buy_exchange = ex_tbl.get(buy_order.order_id);
    auto sell_exchange = ex_tbl.get(sell_order.order_id);

    // spSellOpAccount = tx.GetAccount(context, sellOperatorDetail.fee_receiver_regid, "sell_operator");
    // if (!spSellOpAccount) return false;

    // 1.3 get order exchange params
    // buy_orderOperatorParams = GetOrderOperatorParams(buy_order, buyOperatorDetail);
    // sell_orderOperatorParams = GetOrderOperatorParams(sell_order, sellOperatorDetail);

    // 1.5 get taker side
    auto taker_side = get_taker_side(buy_order, sell_order);

    // spBpAccount = tx.GetAccount(context, context.bp_regid, "current_bp");
    // if (!spBpAccount) return false;

    // 2. check coin pair type match
    check(buy_order.coin_quant.symbol == coin_quant.symbol &&
              buy_order.asset_quant.symbol == asset_quant.symbol,
          "buy order coin pair mismatch");
    check(sell_order.coin_quant.symbol == coin_quant.symbol &&
              sell_order.asset_quant.symbol == asset_quant.symbol,
          "sell order coin pair mismatch");

    // 3. check price match
    if (buy_order.order_type == order_type::LIMIT_PRICE && sell_order.order_type == order_type::LIMIT_PRICE) {
        check(price <= buy_order.price, "the deal price must <= buy_order.price");
        check(price >= sell_order.price, "the deal price must >= sell_order.price");
    } else if (buy_order.order_type == order_type::LIMIT_PRICE && sell_order.order_type == order_type::MARKET_PRICE) {
        check(price == buy_order.price, "the deal price must == buy_order.price when sell_order is MARKET_PRICE");
    } else if (buy_order.order_type == order_type::MARKET_PRICE && sell_order.order_type == order_type::LIMIT_PRICE) {
        check(price == sell_order.price, "the deal price must == sell_order.price when buy_order is MARKET_PRICE");
    } else {
        check(buy_order.order_type == order_type::MARKET_PRICE && sell_order.order_type == order_type::MARKET_PRICE, "order type mismatch");
    }

    // 4. check cross exchange trading with public mode
    // if (!CheckOrderOpenMode()) return false;

    // 5. check coin amount
    if (buy_order.order_type == order_type::MARKET_PRICE) {

    }

    // 6. check the dust amount for mariet price buy order
    // uint64_t calcCoinAmount = CDEXOrderBaseTx::CalcCoinAmount(asset_quant.amount, dealItem.dealPrice);
    // int64_t dealAmountDiff = calcCoinAmount - coin_quant.amount;
    // bool isCoinAmountMatch = false;
    // if (buy_order.order_type == order_type::MARKET_PRICE) {
    //     isCoinAmountMatch = (std::abs(dealAmountDiff) <= std::max<int64_t>(1, (1 * dealItem.dealPrice / PRICE_BOOST)));
    // } else {
    //     isCoinAmountMatch = (dealAmountDiff == 0);
    // }
    // if (!isCoinAmountMatch)
    //     return state.DoS(100, ERRORMSG("%s, the deal_coin_quant.amount unmatch! deal_info={%s}, calcCoinAmount=%llu",
    //         DEAL_ITEM_TITLE, dealItem.ToString(), calcCoinAmount),
    //         REJECT_INVALID, "deal-coin-amount-unmatch");

    buy_order.deal_coin_amount += coin_quant.amount;
    buy_order.deal_asset_amount += asset_quant.amount;
    sell_order.deal_coin_amount += coin_quant.amount;
    sell_order.deal_asset_amount += asset_quant.amount;

    // 7. check the order amount limits
    //  and get residual amount
    // uint64_t buy_residual_amount  = 0;
    // uint64_t sell_residual_amount = 0;

    // 7.1 check the buy_order amount limit
    if (buy_order.order_type == order_type::MARKET_PRICE) {
        check(buy_order.coin_quant.amount >= buy_order.deal_coin_amount, "the deal coin_quant.amount exceed residual coin amount of buy_order");
        // buy_residual_amount = buy_order.coin_quant.amount - buy_order.deal_coin_amount;
    } else {
        check(buy_order.asset_quant.amount >= buy_order.deal_asset_amount, "the deal asset_quant.amount exceed residual asset amount of buy_order");
        // buy_residual_amount = limitAssetAmount - buy_order.deal_asset_amount;
    }

    // 7.2 check the buy_order amount limit
    {
        check(sell_order.asset_quant.amount >= sell_order.deal_asset_amount, "the deal asset_quant.amount exceed residual asset amount of sell_order");
        // sell_residual_amount = limitAssetAmount - sell_order.deal_asset_amount;
    }

    // the seller receive coins
    uint64_t seller_recv_coins = coin_quant.amount;
    // the buyer receive assets
    uint64_t buyer_recv_assets = asset_quant.amount;

    // 8. calc the friction fees
    uint64_t coin_friction_fee = calc_friction_fee(coin_quant.amount);
    seller_recv_coins = sub_fee(seller_recv_coins, coin_friction_fee, "seller_recv_coins");
    uint64_t asset_friction_fee = calc_friction_fee(asset_quant.amount);
    buyer_recv_assets = sub_fee(buyer_recv_assets, asset_friction_fee, "buyer_recv_assets");

    // 9. calc deal fees for exchange
    // 9.1. calc deal asset fee payed by buyer for exchange
    uint64_t asset_match_fee = calc_match_fee(buy_order, buy_exchange, taker_side, buyer_recv_assets);
    buyer_recv_assets = sub_fee(buyer_recv_assets, asset_match_fee, "buyer_recv_assets");

    // 9.2. calc deal coin fee payed by seller for exhange
    uint64_t coin_match_fee = calc_match_fee(sell_order, sell_exchange, taker_side, seller_recv_coins);
    seller_recv_coins = sub_fee(seller_recv_coins, asset_match_fee, "seller_recv_coins");


    // TODO: send fee to fee_receiver of this contract
        // 11.1 coin friction fee: seller -> risk reserve
        // if (!ProcessCoinFrictionFee(*spsell_orderAccount, sell_order.coin_quant.symbol, coin_friction_fee)) return false;
        // // 11.2 asset friction fee: buyer -> current BP
        // if (!spbuy_orderAccount->OperateBalance(buy_order.asset_quant.symbol, BalanceOpType::SUB_FREE, asset_friction_fee,
        //                                         ReceiptType::FRICTION_FEE, receipts, spBpAccount.get())) {
        //     return state.DoS(100, ERRORMSG("transfer friction to current bp account failed"),
        //                     UPDATE_ACCOUNT_FAIL, "transfer-friction-fee-failed");
        // }

    // 11. pay the coin and asset match fee to exchange fee_receiver
    // 11.1. pay the coin_match_fee to sell_exchange.payee
    transfer_out(get_self(), BANK, sell_exchange.payee, asset(coin_match_fee, coin_quant.symbol), "coin_match_fee");
    // 11.2. pay the asset_match_fee to buy_exchange.payee
    transfer_out(get_self(), BANK, buy_exchange.payee, asset(asset_match_fee, asset_quant.symbol), "asset_match_fee");

    // 12. transfer the coins and assets to seller and buyer
    // 12.1. transfer the coins to seller
    transfer_out(get_self(), BANK, sell_order.owner, asset(seller_recv_coins, coin_quant.symbol), "seller_recv_coins");
    // 12.2. transfer the assets to buyer
    transfer_out(get_self(), BANK, buy_order.owner, asset(buyer_recv_assets, asset_quant.symbol), "buyer_recv_assets");


    // 13. check order fullfiled to del or update
    // 13.1 check buy order fullfiled to del or update
    bool buy_order_fulfilled = false;
    if (buy_order.order_type == order_type::LIMIT_PRICE) {
        buy_order_fulfilled = (buy_order.asset_quant.amount == buy_order.deal_asset_amount);
        if (buy_order_fulfilled) {
            if (buy_order.coin_quant.amount > buy_order.deal_coin_amount) {
                uint64_t refund_coins = buy_order.coin_quant.amount - buy_order.deal_coin_amount;
                transfer_out(get_self(), BANK, buy_order.owner, asset(refund_coins, coin_quant.symbol), "refund_coins");
            }
        }
    } else { // buy_order.order_type == order_type::MARKET_PRICE
        buy_order_fulfilled = buy_order.coin_quant.amount == buy_order.deal_coin_amount;
    }
    if (buy_order_fulfilled) {
        // erase order
        // TODO: ...
    } else {
        // udpate order
        order_tbl.modify(buy_order, same_payer, [&]( auto& a ) {
            a = buy_order;
        });
    }

    // 13.2 check sell order fullfiled to del or update
    if (sell_order.asset_quant.amount == sell_order.deal_asset_amount) {
        // erase order
        // TODO: ...
    } else {
        // udpate order
        order_tbl.modify(sell_order, same_payer, [&]( auto& a ) {
            a = sell_order;
        });
    }
}