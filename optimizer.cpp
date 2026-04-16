/*
 * IMC Prosperity 4 - C++ Optimizer
 * Parameter grid search for ASH_COATED_OSMIUM and INTARIAN_PEPPER_ROOT
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <iomanip>

struct PriceRow
{
    int day, timestamp;
    std::string product;
    double bid_price_1, bid_volume_1;
    double bid_price_2, bid_volume_2;
    double bid_price_3, bid_volume_3;
    double ask_price_1, ask_volume_1;
    double ask_price_2, ask_volume_2;
    double ask_price_3, ask_volume_3;
    double mid_price;
    bool has_bp2 = false, has_bp3 = false;
    bool has_ap2 = false, has_ap3 = false;
};

struct TradeRow
{
    int timestamp;
    std::string buyer, seller, symbol, currency;
    double price;
    int quantity;
    int day;
    long long compositeKey() const { return (long long)day * 1000000LL + timestamp; }
};

struct OrderDepth
{
    std::map<int, int> buy_orders;
    std::map<int, int> sell_orders;
};

struct Order
{
    std::string symbol;
    int price;
    int quantity;
};

struct State
{
    double ipr_ema = -1.0;
    double ipr_momentum = 0.0;
};

// --- CONFIGURATION STRUCT FOR OPTIMIZATION ---
struct Config
{
    double aco_inv_mult;  // Default: 3.0
    double aco_obi_mult;  // Default: 2.0
    double ipr_ema_alpha; // Default: 0.5
    double ipr_inv_mult;  // Default: 2.0
    double ipr_obi_mult;  // Default: 2.0
    double ipr_mom_decay; // Default: 0.6
};

struct OptResult
{
    double pnl;
    Config config;
};

// --- Parsers & Loaders ---
static double parseDouble(const std::string &s) { return s.empty() ? 0.0 : std::stod(s); }
static int parseInt(const std::string &s) { return s.empty() ? 0 : std::stoi(s); }
static std::vector<std::string> split(const std::string &line, char delim)
{
    std::vector<std::string> tokens;
    std::stringstream ss(line);
    std::string token;
    while (std::getline(ss, token, delim))
        tokens.push_back(token);
    return tokens;
}

std::vector<PriceRow> loadPrices(const std::string &filepath)
{
    std::vector<PriceRow> rows;
    std::ifstream file(filepath);
    if (!file.is_open())
        return rows;
    std::string line;
    std::getline(file, line);
    while (std::getline(file, line))
    {
        if (line.empty())
            continue;
        auto t = split(line, ';');
        if (t.size() < 16)
            continue;
        PriceRow r;
        r.day = parseInt(t[0]);
        r.timestamp = parseInt(t[1]);
        r.product = t[2];
        r.bid_price_1 = parseDouble(t[3]);
        r.bid_volume_1 = parseDouble(t[4]);
        r.ask_price_1 = parseDouble(t[9]);
        r.ask_volume_1 = parseDouble(t[10]);
        if (!t[5].empty())
        {
            r.bid_price_2 = parseDouble(t[5]);
            r.bid_volume_2 = parseDouble(t[6]);
            r.has_bp2 = true;
        }
        if (!t[7].empty())
        {
            r.bid_price_3 = parseDouble(t[7]);
            r.bid_volume_3 = parseDouble(t[8]);
            r.has_bp3 = true;
        }
        if (!t[11].empty())
        {
            r.ask_price_2 = parseDouble(t[11]);
            r.ask_volume_2 = parseDouble(t[12]);
            r.has_ap2 = true;
        }
        if (!t[13].empty())
        {
            r.ask_price_3 = parseDouble(t[13]);
            r.ask_volume_3 = parseDouble(t[14]);
            r.has_ap3 = true;
        }
        r.mid_price = parseDouble(t[15]);
        rows.push_back(r);
    }
    return rows;
}

std::vector<TradeRow> loadTrades(const std::string &filepath, int day)
{
    std::vector<TradeRow> rows;
    std::ifstream file(filepath);
    if (!file.is_open())
        return rows;
    std::string line;
    std::getline(file, line);
    while (std::getline(file, line))
    {
        if (line.empty())
            continue;
        auto t = split(line, ';');
        if (t.size() < 7)
            continue;
        TradeRow r;
        r.timestamp = parseInt(t[0]);
        r.symbol = t[3];
        r.price = parseDouble(t[5]);
        r.quantity = parseInt(t[6]);
        r.day = day;
        rows.push_back(r);
    }
    return rows;
}

OrderDepth buildOrderDepth(const PriceRow &row)
{
    OrderDepth od;
    if (row.bid_price_1 > 0)
        od.buy_orders[(int)row.bid_price_1] = (int)row.bid_volume_1;
    if (row.has_bp2)
        od.buy_orders[(int)row.bid_price_2] = (int)row.bid_volume_2;
    if (row.has_bp3)
        od.buy_orders[(int)row.bid_price_3] = (int)row.bid_volume_3;
    if (row.ask_price_1 > 0)
        od.sell_orders[(int)row.ask_price_1] = -(int)row.ask_volume_1;
    if (row.has_ap2)
        od.sell_orders[(int)row.ask_price_2] = -(int)row.ask_volume_2;
    if (row.has_ap3)
        od.sell_orders[(int)row.ask_price_3] = -(int)row.ask_volume_3;
    return od;
}

double vwap_l2_mid(const OrderDepth &od)
{
    if (od.buy_orders.empty() || od.sell_orders.empty())
        return -1.0;
    double bid_vwap = 0;
    int bid_vol = 0, count = 0;
    for (auto it = od.buy_orders.rbegin(); it != od.buy_orders.rend() && count < 2; ++it, ++count)
    {
        bid_vwap += it->first * it->second;
        bid_vol += it->second;
    }
    bid_vwap = bid_vol > 0 ? bid_vwap / bid_vol : od.buy_orders.rbegin()->first;

    double ask_vwap = 0;
    int ask_vol = 0;
    count = 0;
    for (auto it = od.sell_orders.begin(); it != od.sell_orders.end() && count < 2; ++it, ++count)
    {
        ask_vwap += it->first * std::abs(it->second);
        ask_vol += std::abs(it->second);
    }
    ask_vwap = ask_vol > 0 ? ask_vwap / ask_vol : od.sell_orders.begin()->first;
    return (bid_vwap + ask_vwap) / 2.0;
}

// --- Strategy Functions (Parameter Injected) ---
std::vector<Order> tradeACO(const OrderDepth &od, int pos, const Config &config)
{
    std::vector<Order> orders;
    int fair = 10000;
    int limit = 80;

    double obi = 0.0;
    if (!od.buy_orders.empty() && !od.sell_orders.empty())
    {
        int bid_vol = 0, ask_vol = 0;
        for (auto &kv : od.buy_orders)
            bid_vol += kv.second;
        for (auto &kv : od.sell_orders)
            ask_vol += std::abs(kv.second);
        int total = bid_vol + ask_vol;
        if (total > 0)
            obi = (double)(bid_vol - ask_vol) / total;
    }

    int total_buy_qty = 0, total_sell_qty = 0;
    int max_buy = limit - pos;
    int max_sell = limit + pos;

    // Phase 1: Mispriced
    for (auto &kv : od.sell_orders)
    {
        if (kv.first < fair && total_buy_qty < max_buy)
        {
            int qty = std::min(std::abs(kv.second), max_buy - total_buy_qty);
            if (qty > 0)
            {
                orders.push_back({"ASH_COATED_OSMIUM", kv.first, qty});
                total_buy_qty += qty;
            }
        }
    }
    for (auto it = od.buy_orders.rbegin(); it != od.buy_orders.rend(); ++it)
    {
        if (it->first > fair && total_sell_qty < max_sell)
        {
            int qty = std::min(it->second, max_sell - total_sell_qty);
            if (qty > 0)
            {
                orders.push_back({"ASH_COATED_OSMIUM", it->first, -qty});
                total_sell_qty += qty;
            }
        }
    }

    int eff_pos = pos + total_buy_qty - total_sell_qty;

    // Phase 2: Inventory reduction
    if (eff_pos > 10)
    {
        int rem_sell = max_sell - total_sell_qty;
        int target = std::max(0, eff_pos - 5);
        for (auto it = od.buy_orders.rbegin(); it != od.buy_orders.rend(); ++it)
        {
            if (it->first >= fair && rem_sell > 0 && target > 0)
            {
                int qty = std::min({it->second, rem_sell, target});
                if (qty > 0)
                {
                    orders.push_back({"ASH_COATED_OSMIUM", it->first, -qty});
                    total_sell_qty += qty;
                    rem_sell -= qty;
                    target -= qty;
                    eff_pos -= qty;
                }
            }
        }
    }
    else if (eff_pos < -10)
    {
        int rem_buy = max_buy - total_buy_qty;
        int target = std::max(0, std::abs(eff_pos) - 5);
        for (auto &kv : od.sell_orders)
        {
            if (kv.first <= fair && rem_buy > 0 && target > 0)
            {
                int qty = std::min({std::abs(kv.second), rem_buy, target});
                if (qty > 0)
                {
                    orders.push_back({"ASH_COATED_OSMIUM", kv.first, qty});
                    total_buy_qty += qty;
                    rem_buy -= qty;
                    target -= qty;
                    eff_pos += qty;
                }
            }
        }
    }

    // Emergency Reduction
    if (eff_pos > 25)
    {
        int rem_sell = max_sell - total_sell_qty;
        int target = std::max(0, eff_pos - 15);
        for (auto it = od.buy_orders.rbegin(); it != od.buy_orders.rend(); ++it)
        {
            if (it->first >= fair - 1 && rem_sell > 0 && target > 0)
            {
                int qty = std::min({it->second, rem_sell, target});
                if (qty > 0)
                {
                    orders.push_back({"ASH_COATED_OSMIUM", it->first, -qty});
                    total_sell_qty += qty;
                    rem_sell -= qty;
                    target -= qty;
                    eff_pos -= qty;
                }
            }
        }
    }
    else if (eff_pos < -25)
    {
        int rem_buy = max_buy - total_buy_qty;
        int target = std::max(0, std::abs(eff_pos) - 15);
        for (auto &kv : od.sell_orders)
        {
            if (kv.first <= fair + 1 && rem_buy > 0 && target > 0)
            {
                int qty = std::min({std::abs(kv.second), rem_buy, target});
                if (qty > 0)
                {
                    orders.push_back({"ASH_COATED_OSMIUM", kv.first, qty});
                    total_buy_qty += qty;
                    rem_buy -= qty;
                    target -= qty;
                    eff_pos += qty;
                }
            }
        }
    }

    // Phase 3: Quotes (Applying Optimized Variables)
    int rem_buy = max_buy - total_buy_qty;
    int rem_sell = max_sell - total_sell_qty;
    int inv_skew = std::round(eff_pos * config.aco_inv_mult / limit);
    int obi_adj = std::round(obi * config.aco_obi_mult);

    int our_bid = 9993 - inv_skew + obi_adj;
    int our_ask = 10007 - inv_skew + obi_adj;

    our_bid = std::max(9992, std::min(our_bid, 9999));
    our_ask = std::max(10001, std::min(our_ask, 10008));

    // Penny-jump
    int max_in_bid = -1, min_in_ask = 99999;
    for (auto &kv : od.buy_orders)
        if (kv.first > 9991 && kv.first < fair && kv.first > max_in_bid)
            max_in_bid = kv.first;
    for (auto &kv : od.sell_orders)
        if (kv.first < 10009 && kv.first > fair && kv.first < min_in_ask)
            min_in_ask = kv.first;

    if (max_in_bid != -1 && (max_in_bid + 1) < fair && (max_in_bid + 1) > our_bid)
        our_bid = max_in_bid + 1;
    if (min_in_ask != 99999 && (min_in_ask - 1) > fair && (min_in_ask - 1) < our_ask)
        our_ask = min_in_ask - 1;

    if (rem_buy > 0)
        orders.push_back({"ASH_COATED_OSMIUM", our_bid, rem_buy});
    if (rem_sell > 0)
        orders.push_back({"ASH_COATED_OSMIUM", our_ask, -rem_sell});

    return orders;
}

std::vector<Order> tradeIPR(const OrderDepth &od, int pos, State &state, const std::vector<TradeRow> &mkt_trades, const Config &config)
{
    std::vector<Order> orders;
    int limit = 80;

    double raw_fair = vwap_l2_mid(od);
    if (raw_fair < 0)
        raw_fair = (state.ipr_ema > 0) ? state.ipr_ema : 12000.0;
    if (state.ipr_ema < 0)
        state.ipr_ema = raw_fair;

    // Apply Optimized Variable
    state.ipr_ema = config.ipr_ema_alpha * raw_fair + (1.0 - config.ipr_ema_alpha) * state.ipr_ema;
    int fair_int = std::round(state.ipr_ema);

    // Apply Optimized Variable
    state.ipr_momentum *= config.ipr_mom_decay;
    for (const auto &mt : mkt_trades)
    {
        if (mt.price > fair_int)
            state.ipr_momentum += mt.quantity * 0.5;
        else if (mt.price < fair_int)
            state.ipr_momentum -= mt.quantity * 0.5;
    }
    int mom_adj = std::max(-2, std::min(2, (int)std::round(state.ipr_momentum / 4.0)));

    double obi = 0.0;
    if (!od.buy_orders.empty() && !od.sell_orders.empty())
    {
        int bid_vol = 0, ask_vol = 0;
        for (auto &kv : od.buy_orders)
            bid_vol += kv.second;
        for (auto &kv : od.sell_orders)
            ask_vol += std::abs(kv.second);
        int total = bid_vol + ask_vol;
        if (total > 0)
            obi = (double)(bid_vol - ask_vol) / total;
    }

    // Apply Optimized Variable
    int obi_adj = std::round(obi * config.ipr_obi_mult);

    int max_buy = limit - pos;
    int max_sell = limit + pos;
    int total_buy_qty = 0, total_sell_qty = 0;

    for (auto &kv : od.sell_orders)
    {
        if (kv.first < fair_int && total_buy_qty < max_buy)
        {
            int qty = std::min(std::abs(kv.second), max_buy - total_buy_qty);
            if (qty > 0)
            {
                orders.push_back({"INTARIAN_PEPPER_ROOT", kv.first, qty});
                total_buy_qty += qty;
            }
        }
    }
    for (auto it = od.buy_orders.rbegin(); it != od.buy_orders.rend(); ++it)
    {
        if (it->first > fair_int && total_sell_qty < max_sell)
        {
            int qty = std::min(it->second, max_sell - total_sell_qty);
            if (qty > 0)
            {
                orders.push_back({"INTARIAN_PEPPER_ROOT", it->first, -qty});
                total_sell_qty += qty;
            }
        }
    }

    int eff_pos = pos + total_buy_qty - total_sell_qty;

    if (eff_pos > 30)
    {
        int rem_sell = max_sell - total_sell_qty;
        int target = std::max(0, eff_pos - 20);
        for (auto it = od.buy_orders.rbegin(); it != od.buy_orders.rend(); ++it)
        {
            if (it->first >= fair_int && rem_sell > 0 && target > 0)
            {
                int qty = std::min({it->second, rem_sell, target});
                if (qty > 0)
                {
                    orders.push_back({"INTARIAN_PEPPER_ROOT", it->first, -qty});
                    total_sell_qty += qty;
                    rem_sell -= qty;
                    target -= qty;
                    eff_pos -= qty;
                }
            }
        }
    }
    else if (eff_pos < -30)
    {
        int rem_buy = max_buy - total_buy_qty;
        int target = std::max(0, std::abs(eff_pos) - 20);
        for (auto &kv : od.sell_orders)
        {
            if (kv.first <= fair_int && rem_buy > 0 && target > 0)
            {
                int qty = std::min({std::abs(kv.second), rem_buy, target});
                if (qty > 0)
                {
                    orders.push_back({"INTARIAN_PEPPER_ROOT", kv.first, qty});
                    total_buy_qty += qty;
                    rem_buy -= qty;
                    target -= qty;
                    eff_pos += qty;
                }
            }
        }
    }

    int rem_buy = max_buy - total_buy_qty;
    int rem_sell = max_sell - total_sell_qty;

    int best_bid = od.buy_orders.empty() ? fair_int - 7 : od.buy_orders.rbegin()->first;
    int best_ask = od.sell_orders.empty() ? fair_int + 7 : od.sell_orders.begin()->first;
    int spread = best_ask - best_bid;

    // Apply Optimized Variable
    int skew = std::round((eff_pos / (double)limit) * config.ipr_inv_mult);

    int half = (spread >= 16) ? 7 : (spread >= 14) ? 6
                                : (spread >= 12)   ? 5
                                : (spread >= 10)   ? 4
                                : (spread >= 6)    ? 3
                                                   : 2;

    int our_bid = fair_int - half - skew + mom_adj + obi_adj;
    int our_ask = fair_int + half - skew + mom_adj + obi_adj;

    int max_in_bid = -1, min_in_ask = 99999;
    int norm_bid = fair_int - 7, norm_ask = fair_int + 7;
    for (auto &kv : od.buy_orders)
        if (kv.first > norm_bid && kv.first < fair_int && kv.first > max_in_bid)
            max_in_bid = kv.first;
    for (auto &kv : od.sell_orders)
        if (kv.first < norm_ask && kv.first > fair_int && kv.first < min_in_ask)
            min_in_ask = kv.first;

    if (max_in_bid != -1 && (max_in_bid + 1) < fair_int && (max_in_bid + 1) > our_bid)
        our_bid = max_in_bid + 1;
    if (min_in_ask != 99999 && (min_in_ask - 1) > fair_int && (min_in_ask - 1) < our_ask)
        our_ask = min_in_ask - 1;

    our_bid = std::min(our_bid, fair_int - 1);
    our_ask = std::max(our_ask, fair_int + 1);
    our_bid = std::max(our_bid, best_bid + 1);
    our_ask = std::min(our_ask, best_ask - 1);
    our_bid = std::min(our_bid, fair_int - 1);
    our_ask = std::max(our_ask, fair_int + 1);

    if (rem_buy > 0)
    {
        int tight = std::max(1, (int)(rem_buy * 0.6));
        int wide = rem_buy - tight;
        orders.push_back({"INTARIAN_PEPPER_ROOT", our_bid, tight});
        if (wide > 0)
            orders.push_back({"INTARIAN_PEPPER_ROOT", our_bid - 2, wide});
    }
    if (rem_sell > 0)
    {
        int tight = std::max(1, (int)(rem_sell * 0.6));
        int wide = rem_sell - tight;
        orders.push_back({"INTARIAN_PEPPER_ROOT", our_ask, -tight});
        if (wide > 0)
            orders.push_back({"INTARIAN_PEPPER_ROOT", our_ask + 2, -wide});
    }
    return orders;
}

// --- Matching Engine ---
void matchRestingOrders(std::vector<Order> &resting, const std::vector<TradeRow> &trades, int &pos, double &cash)
{
    for (const auto &trade : trades)
    {
        int trade_remaining = trade.quantity;
        for (auto &ord : resting)
        {
            if (trade_remaining <= 0)
                break;
            if (ord.symbol != trade.symbol || ord.quantity == 0)
                continue;

            if (ord.quantity > 0 && trade.price <= ord.price)
            {
                int fill = std::min({ord.quantity, trade_remaining});
                pos += fill;
                cash -= fill * ord.price;
                ord.quantity -= fill;
                trade_remaining -= fill;
            }
            else if (ord.quantity < 0 && trade.price >= ord.price)
            {
                int fill = std::min({std::abs(ord.quantity), trade_remaining});
                pos -= fill;
                cash += fill * ord.price;
                ord.quantity += fill;
                trade_remaining -= fill;
            }
        }
    }
}

std::vector<Order> matchTakerOrders(std::vector<Order> &orders, OrderDepth &od, int &pos, double &cash)
{
    std::vector<Order> unfulfilled;
    for (auto &ord : orders)
    {
        if (ord.quantity > 0)
        {
            while (ord.quantity > 0 && !od.sell_orders.empty())
            {
                auto best_ask = od.sell_orders.begin();
                if (ord.price >= best_ask->first)
                {
                    int fill = std::min(ord.quantity, std::abs(best_ask->second));
                    pos += fill;
                    cash -= fill * best_ask->first;
                    ord.quantity -= fill;
                    best_ask->second += fill;
                    if (best_ask->second == 0)
                        od.sell_orders.erase(best_ask);
                }
                else
                    break;
            }
        }
        else if (ord.quantity < 0)
        {
            while (ord.quantity < 0 && !od.buy_orders.empty())
            {
                auto best_bid = od.buy_orders.rbegin();
                if (ord.price <= best_bid->first)
                {
                    int fill = std::min(std::abs(ord.quantity), best_bid->second);
                    pos -= fill;
                    cash += fill * best_bid->first;
                    ord.quantity += fill;
                    best_bid->second -= fill;
                    if (best_bid->second == 0)
                        od.buy_orders.erase(std::next(best_bid).base());
                }
                else
                    break;
            }
        }
        if (ord.quantity != 0)
            unfulfilled.push_back(ord);
    }
    return unfulfilled;
}

// --- Optimization Runner ---
double runBacktest(const Config &config,
                   const std::vector<long long> &timestamps,
                   const std::map<long long, std::vector<const PriceRow *>> &pricesByTs,
                   const std::map<long long, std::vector<TradeRow>> &tradesByTs,
                   double final_aco_mid, double final_ipr_mid)
{

    int aco_pos = 0, ipr_pos = 0;
    double aco_cash = 0.0, ipr_cash = 0.0;
    State state;

    std::vector<Order> resting_aco, resting_ipr;

    for (long long ts : timestamps)
    {
        auto trade_it = tradesByTs.find(ts);
        if (trade_it != tradesByTs.end())
        {
            matchRestingOrders(resting_aco, trade_it->second, aco_pos, aco_cash);
            matchRestingOrders(resting_ipr, trade_it->second, ipr_pos, ipr_cash);
        }

        resting_aco.clear();
        resting_ipr.clear();

        auto price_it = pricesByTs.find(ts);
        if (price_it != pricesByTs.end())
        {
            for (const auto *row : price_it->second)
            {
                OrderDepth od = buildOrderDepth(*row);

                if (row->product == "ASH_COATED_OSMIUM")
                {
                    auto orders = tradeACO(od, aco_pos, config);
                    auto unfulfilled = matchTakerOrders(orders, od, aco_pos, aco_cash);
                    resting_aco.insert(resting_aco.end(), unfulfilled.begin(), unfulfilled.end());
                }
                else if (row->product == "INTARIAN_PEPPER_ROOT")
                {
                    std::vector<TradeRow> sym_trades;
                    if (trade_it != tradesByTs.end())
                    {
                        for (const auto &t : trade_it->second)
                            if (t.symbol == "INTARIAN_PEPPER_ROOT")
                                sym_trades.push_back(t);
                    }
                    auto orders = tradeIPR(od, ipr_pos, state, sym_trades, config);
                    auto unfulfilled = matchTakerOrders(orders, od, ipr_pos, ipr_cash);
                    resting_ipr.insert(resting_ipr.end(), unfulfilled.begin(), unfulfilled.end());
                }
            }
        }
    }

    double aco_pnl = aco_cash + aco_pos * final_aco_mid;
    double ipr_pnl = ipr_cash + ipr_pos * final_ipr_mid;
    return aco_pnl + ipr_pnl;
}

int main(int argc, char *argv[])
{
    if (argc < 3 || argc % 2 != 1)
    {
        std::cerr << "Usage: optimizer <prices1.csv> <trades1.csv> [prices2.csv trades2.csv ...]" << std::endl;
        return 1;
    }

    // 1. Pre-load all data into memory
    std::vector<PriceRow> all_prices;
    std::vector<TradeRow> all_trades;

    // Prevent reallocation to safely store pointers
    all_prices.reserve(500000);

    for (int i = 1; i < argc; i += 2)
    {
        int day = (i - 1) / 2;
        auto prices = loadPrices(argv[i]);
        auto trades = loadTrades(argv[i + 1], day);
        std::cout << "Loaded " << prices.size() << " prices & " << trades.size() << " trades (Day " << day << ")" << std::endl;

        for (auto &p : prices)
            all_prices.push_back(p);
        for (auto &t : trades)
            all_trades.push_back(t);
    }

    std::map<long long, std::vector<const PriceRow *>> pricesByTs;
    std::map<long long, std::vector<TradeRow>> tradesByTs;

    for (const auto &p : all_prices)
    {
        pricesByTs[(long long)p.day * 1000000LL + p.timestamp].push_back(&p);
    }
    for (const auto &t : all_trades)
    {
        tradesByTs[t.compositeKey()].push_back(t);
    }

    std::vector<long long> timestamps;
    timestamps.reserve(pricesByTs.size());
    for (auto &kv : pricesByTs)
        timestamps.push_back(kv.first);
    std::sort(timestamps.begin(), timestamps.end());

    double final_aco_mid = 10000.0, final_ipr_mid = 11500.0;
    for (auto it = all_prices.rbegin(); it != all_prices.rend(); ++it)
    {
        if (it->product == "ASH_COATED_OSMIUM")
        {
            final_aco_mid = it->mid_price;
            break;
        }
    }
    for (auto it = all_prices.rbegin(); it != all_prices.rend(); ++it)
    {
        if (it->product == "INTARIAN_PEPPER_ROOT")
        {
            final_ipr_mid = it->mid_price;
            break;
        }
    }

    // 2. Generate Parameter Grid
    std::vector<Config> configs;
    std::cout << "\nGenerating parameter grid..." << std::endl;
    for (double aco_inv = 1.0; aco_inv <= 5.0; aco_inv += 1.0)
    {
        for (double aco_obi = 0.0; aco_obi <= 4.0; aco_obi += 1.0)
        {
            for (double ipr_ema = 0.1; ipr_ema <= 0.9; ipr_ema += 0.2)
            {
                for (double ipr_inv = 1.0; ipr_inv <= 4.0; ipr_inv += 1.0)
                {
                    for (double ipr_obi = 0.0; ipr_obi <= 4.0; ipr_obi += 1.0)
                    {
                        for (double ipr_mom = 0.4; ipr_mom <= 0.8; ipr_mom += 0.2)
                        {
                            configs.push_back({aco_inv, aco_obi, ipr_ema, ipr_inv, ipr_obi, ipr_mom});
                        }
                    }
                }
            }
        }
    }

    std::cout << "Testing " << configs.size() << " combinations. This will take a moment...\n"
              << std::endl;

    // 3. Run Optimization
    std::vector<OptResult> results;
    results.resize(configs.size());

    // If using a compiler with OpenMP support, uncomment the line below to parallelize
    // #pragma omp parallel for
    for (size_t i = 0; i < configs.size(); ++i)
    {
        double pnl = runBacktest(configs[i], timestamps, pricesByTs, tradesByTs, final_aco_mid, final_ipr_mid);
        results[i] = {pnl, configs[i]};
    }

    // 4. Sort and Print Results
    std::sort(results.begin(), results.end(), [](const OptResult &a, const OptResult &b)
              {
                  return a.pnl > b.pnl; // Sort descending
              });

    std::cout << "=== TOP 25 STRATEGY CONFIGURATIONS ===" << std::endl;
    std::cout << std::left << std::setw(12) << "Total PnL"
              << std::setw(12) << "ACO_Inv" << std::setw(12) << "ACO_OBI"
              << std::setw(12) << "IPR_EMA" << std::setw(12) << "IPR_Inv"
              << std::setw(12) << "IPR_OBI" << std::setw(12) << "IPR_MomDecay" << std::endl;
    std::cout << std::string(80, '-') << std::endl;

    int print_count = std::min((int)results.size(), 25);
    for (int i = 0; i < print_count; ++i)
    {
        std::cout << std::left << std::setw(12) << std::fixed << std::setprecision(2) << results[i].pnl
                  << std::setw(12) << std::setprecision(1) << results[i].config.aco_inv_mult
                  << std::setw(12) << results[i].config.aco_obi_mult
                  << std::setw(12) << results[i].config.ipr_ema_alpha
                  << std::setw(12) << results[i].config.ipr_inv_mult
                  << std::setw(12) << results[i].config.ipr_obi_mult
                  << std::setw(12) << results[i].config.ipr_mom_decay << std::endl;
    }

    return 0;
}