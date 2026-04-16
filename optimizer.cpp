/*
 * IMC Prosperity 4 - C++ Parameter Optimizer for ROUND 1
 * Uses ASH_COATED_OSMIUM and INTARIAN_PEPPER_ROOT
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
#include <chrono>
#include <iomanip>

struct PriceRow {
    int day;
    int timestamp;
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

struct TradeRow {
    int timestamp;
    std::string buyer, seller, symbol, currency;
    double price;
    int quantity;
    int day;
};

struct OrderDepth {
    std::map<int, int> buy_orders;
    std::map<int, int> sell_orders;
};

struct Order {
    std::string symbol;
    int price;
    int quantity;
};

struct Params {
    int aco_edge;
    int aco_skew;
    double ipr_ema_alpha;
    int ipr_edge;
    int ipr_skew;
};

struct BacktestResult {
    double aco_pnl;
    double ipr_pnl;
    double total_pnl;
    int total_trades;
};

static double parseDouble(const std::string& s) { return s.empty() ? 0.0 : std::stod(s); }
static int parseInt(const std::string& s) { return s.empty() ? 0 : std::stoi(s); }

static std::vector<std::string> split(const std::string& line, char delim) {
    std::vector<std::string> tokens;
    std::stringstream ss(line);
    std::string token;
    while (std::getline(ss, token, delim)) tokens.push_back(token);
    return tokens;
}

std::vector<PriceRow> loadPrices(const std::string& filepath) {
    std::vector<PriceRow> rows;
    std::ifstream file(filepath);
    if (!file.is_open()) return rows;
    std::string line;
    std::getline(file, line);
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto t = split(line, ';');
        if (t.size() < 16) continue;
        PriceRow r;
        r.day = parseInt(t[0]); r.timestamp = parseInt(t[1]); r.product = t[2];
        r.bid_price_1 = parseDouble(t[3]); r.bid_volume_1 = parseDouble(t[4]);
        r.ask_price_1 = parseDouble(t[9]); r.ask_volume_1 = parseDouble(t[10]);
        if (!t[5].empty()) { r.bid_price_2 = parseDouble(t[5]); r.bid_volume_2 = parseDouble(t[6]); r.has_bp2 = true; }
        if (!t[7].empty()) { r.bid_price_3 = parseDouble(t[7]); r.bid_volume_3 = parseDouble(t[8]); r.has_bp3 = true; }
        if (!t[11].empty()) { r.ask_price_2 = parseDouble(t[11]); r.ask_volume_2 = parseDouble(t[12]); r.has_ap2 = true; }
        if (!t[13].empty()) { r.ask_price_3 = parseDouble(t[13]); r.ask_volume_3 = parseDouble(t[14]); r.has_ap3 = true; }
        r.mid_price = parseDouble(t[15]);
        rows.push_back(r);
    }
    return rows;
}

std::vector<TradeRow> loadTrades(const std::string& filepath, int day) {
    std::vector<TradeRow> rows;
    std::ifstream file(filepath);
    if (!file.is_open()) return rows;
    std::string line;
    std::getline(file, line);
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto t = split(line, ';');
        if (t.size() < 7) continue;
        TradeRow r;
        r.timestamp = parseInt(t[0]); r.symbol = t[3]; r.price = parseDouble(t[5]); r.quantity = parseInt(t[6]); r.day = day;
        rows.push_back(r);
    }
    return rows;
}

OrderDepth buildOrderDepth(const PriceRow& row) {
    OrderDepth od;
    if (row.bid_price_1 > 0) od.buy_orders[(int)row.bid_price_1] = (int)row.bid_volume_1;
    if (row.has_bp2) od.buy_orders[(int)row.bid_price_2] = (int)row.bid_volume_2;
    if (row.has_bp3) od.buy_orders[(int)row.bid_price_3] = (int)row.bid_volume_3;
    if (row.ask_price_1 > 0) od.sell_orders[(int)row.ask_price_1] = -(int)row.ask_volume_1;
    if (row.has_ap2) od.sell_orders[(int)row.ask_price_2] = -(int)row.ask_volume_2;
    if (row.has_ap3) od.sell_orders[(int)row.ask_price_3] = -(int)row.ask_volume_3;
    return od;
}

double wallMid(const OrderDepth& od) {
    if (od.buy_orders.empty() || od.sell_orders.empty()) return -1;
    int bid_wall = od.buy_orders.begin()->first;
    int bid_max = 0;
    for (auto& [p, v] : od.buy_orders) { if (v > bid_max) { bid_max = v; bid_wall = p; } }
    int ask_wall = od.sell_orders.begin()->first;
    int ask_max = 0;
    for (auto& [p, v] : od.sell_orders) { if (std::abs(v) > ask_max) { ask_max = std::abs(v); ask_wall = p; } }
    return (bid_wall + ask_wall) / 2.0;
}

std::vector<Order> tradeACO(const OrderDepth& od, int position, const Params& params) {
    std::vector<Order> orders;
    const int fair = 10000;
    const int limit = 80;
    int buy_room = limit - position;
    int sell_room = limit + position;

    for (auto& [ask_p, ask_v] : od.sell_orders) {
        int vol = std::abs(ask_v);
        if (ask_p < fair && buy_room > 0) {
            int qty = std::min(vol, buy_room);
            orders.push_back({"ASH_COATED_OSMIUM", ask_p, qty});
            buy_room -= qty;
        }
    }
    for (auto it = od.buy_orders.rbegin(); it != od.buy_orders.rend(); ++it) {
        if (it->first > fair && sell_room > 0) {
            int qty = std::min(it->second, sell_room);
            orders.push_back({"ASH_COATED_OSMIUM", it->first, -qty});
            sell_room -= qty;
        }
    }

    int skew = std::lround((double)position * params.aco_skew / limit);
    int our_bid = fair - params.aco_edge - skew;
    int our_ask = fair + params.aco_edge - skew;

    our_bid = std::min(our_bid, fair - 1);
    our_ask = std::max(our_ask, fair + 1);

    if (!od.buy_orders.empty()) {
        int best_bid = od.buy_orders.rbegin()->first;
        if (our_bid <= best_bid && best_bid < fair - 1) our_bid = best_bid + 1;
    }
    if (!od.sell_orders.empty()) {
        int best_ask = od.sell_orders.begin()->first;
        if (our_ask >= best_ask && best_ask > fair + 1) our_ask = best_ask - 1;
    }

    if (buy_room > 0) orders.push_back({"ASH_COATED_OSMIUM", our_bid, buy_room});
    if (sell_room > 0) orders.push_back({"ASH_COATED_OSMIUM", our_ask, -sell_room});
    return orders;
}

std::vector<Order> tradeIPR(const OrderDepth& od, int position, double& ema, const Params& params) {
    std::vector<Order> orders;
    const int limit = 80;
    double raw = wallMid(od);
    if (raw < 0) raw = (ema > 0) ? ema : 11500.0;
    if (ema <= 0) ema = raw;
    else ema = params.ipr_ema_alpha * raw + (1.0 - params.ipr_ema_alpha) * ema;
    int fair = std::lround(ema);

    int buy_room = limit - position;
    int sell_room = limit + position;

    for (auto& [ask_p, ask_v] : od.sell_orders) {
        if (ask_p < fair && buy_room > 0) {
            int qty = std::min(std::abs(ask_v), buy_room);
            orders.push_back({"INTARIAN_PEPPER_ROOT", ask_p, qty});
            buy_room -= qty;
        }
    }
    for (auto it = od.buy_orders.rbegin(); it != od.buy_orders.rend(); ++it) {
        if (it->first > fair && sell_room > 0) {
            int qty = std::min(it->second, sell_room);
            orders.push_back({"INTARIAN_PEPPER_ROOT", it->first, -qty});
            sell_room -= qty;
        }
    }

    int skew = std::lround((double)position * params.ipr_skew / limit);
    int our_bid = fair - params.ipr_edge - skew;
    int our_ask = fair + params.ipr_edge - skew;

    our_bid = std::min(our_bid, fair - 1);
    our_ask = std::max(our_ask, fair + 1);

    if (!od.buy_orders.empty()) {
        int best_bid = od.buy_orders.rbegin()->first;
        if (our_bid <= best_bid && best_bid < fair - 1) our_bid = best_bid + 1;
    }
    if (!od.sell_orders.empty()) {
        int best_ask = od.sell_orders.begin()->first;
        if (our_ask >= best_ask && best_ask > fair + 1) our_ask = best_ask - 1;
    }

    if (buy_room > 0) orders.push_back({"INTARIAN_PEPPER_ROOT", our_bid, buy_room});
    if (sell_room > 0) orders.push_back({"INTARIAN_PEPPER_ROOT", our_ask, -sell_room});
    return orders;
}

struct Fill { int price; int quantity; };
std::vector<Fill> matchOrders(const std::vector<Order>& orders, OrderDepth od, int position, int limit) {
    std::vector<Fill> fills;
    int pos = position;
    for (auto& order : orders) {
        if (order.quantity > 0) {
            int rem = std::min(order.quantity, limit - pos);
            for (auto it = od.sell_orders.begin(); it != od.sell_orders.end() && rem > 0;) {
                if (order.price >= it->first) {
                    int qty = std::min(rem, std::abs(it->second));
                    if (qty > 0) {
                        fills.push_back({it->first, qty});
                        pos += qty; rem -= qty; it->second += qty;
                        if (it->second == 0) it = od.sell_orders.erase(it);
                        else ++it;
                    } else ++it;
                } else break;
            }
        } else if (order.quantity < 0) {
            int rem = std::min(std::abs(order.quantity), limit + pos);
            for (auto it = od.buy_orders.rbegin(); it != od.buy_orders.rend() && rem > 0;) {
                if (order.price <= it->first) {
                    int qty = std::min(rem, it->second);
                    if (qty > 0) {
                        fills.push_back({it->first, -qty});
                        pos -= qty; rem -= qty; it->second -= qty;
                        if (it->second == 0) {
                            od.buy_orders.erase(std::next(it).base());
                            it = od.buy_orders.rbegin();
                        } else ++it;
                    } else ++it;
                } else break;
            }
        }
    }
    return fills;
}

BacktestResult runBacktest(const std::vector<PriceRow>& prices, const std::vector<TradeRow>& trades, const Params& params) {
    std::map<int, std::vector<const PriceRow*>> pricesByTs;
    for (auto& row : prices) pricesByTs[row.timestamp].push_back(&row);
    std::vector<int> timestamps;
    for (auto& [ts, _] : pricesByTs) timestamps.push_back(ts);
    std::sort(timestamps.begin(), timestamps.end());

    int aco_pos = 0, ipr_pos = 0;
    double aco_cash = 0.0, ipr_cash = 0.0;
    double ipr_ema = 0.0;
    int total = 0;

    for (int ts : timestamps) {
        auto& rows = pricesByTs[ts];
        for (auto* row : rows) {
            OrderDepth od = buildOrderDepth(*row);
            if (row->product == "ASH_COATED_OSMIUM") {
                auto orders = tradeACO(od, aco_pos, params);
                OrderDepth od_match = buildOrderDepth(*row);
                auto fills = matchOrders(orders, od_match, aco_pos, 80);
                for (auto& f : fills) {
                    if (f.quantity > 0) { aco_cash -= f.price * f.quantity; aco_pos += f.quantity; }
                    else { aco_cash += f.price * std::abs(f.quantity); aco_pos -= std::abs(f.quantity); }
                    total++;
                }
            } else if (row->product == "INTARIAN_PEPPER_ROOT") {
                double ema_b = ipr_ema;
                auto orders = tradeIPR(od, ipr_pos, ipr_ema, params);
                ipr_ema = ema_b; // Reset for matching
                double raw = wallMid(od);
                if (raw < 0) raw = (ipr_ema > 0) ? ipr_ema : 11500.0;
                if (ipr_ema <= 0) ipr_ema = raw;
                else ipr_ema = params.ipr_ema_alpha * raw + (1.0 - params.ipr_ema_alpha) * ipr_ema;

                OrderDepth od_match = buildOrderDepth(*row);
                auto fills = matchOrders(orders, od_match, ipr_pos, 80);
                for (auto& f : fills) {
                    if (f.quantity > 0) { ipr_cash -= f.price * f.quantity; ipr_pos += f.quantity; }
                    else { ipr_cash += f.price * std::abs(f.quantity); ipr_pos -= std::abs(f.quantity); }
                    total++;
                }
            }
        }
    }

    double aco_mid = 10000.0, ipr_mid = 11500.0;
    for (auto it = prices.rbegin(); it != prices.rend(); ++it) {
        if (it->product == "ASH_COATED_OSMIUM") { aco_mid = it->mid_price; break; }
    }
    for (auto it = prices.rbegin(); it != prices.rend(); ++it) {
        if (it->product == "INTARIAN_PEPPER_ROOT") { ipr_mid = it->mid_price; break; }
    }

    double aco_pnl = aco_cash + aco_pos * aco_mid;
    double ipr_pnl = ipr_cash + ipr_pos * ipr_mid;
    return {aco_pnl, ipr_pnl, aco_pnl + ipr_pnl, total};
}

int main(int argc, char* argv[]) {
    if (argc < 3 || argc % 2 != 1) {
        std::cerr << "Usage: optimizer <prices1.csv> <trades1.csv> ..." << std::endl;
        return 1;
    }
    struct DayData { std::vector<PriceRow> p; std::vector<TradeRow> t; };
    std::vector<DayData> days;
    for (int i = 1; i < argc; i += 2) {
        days.push_back({loadPrices(argv[i]), loadTrades(argv[i+1], i)});
        std::cout << "Loaded " << argv[i] << " & " << argv[i+1] << std::endl;
    }

    std::vector<int> aco_edges = {2, 3, 4, 5, 6, 7, 8, 9};
    std::vector<int> aco_skews = {1, 2, 3, 4, 5};
    std::vector<double> ipr_emas = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
    std::vector<int> ipr_edges = {2, 3, 4, 5, 6, 7};
    std::vector<int> ipr_skews = {1, 2, 3, 4, 5};

    int total_combos = aco_edges.size() * aco_skews.size() * ipr_emas.size() * ipr_edges.size() * ipr_skews.size();
    std::cout << "Evaluating " << total_combos << " combinations..." << std::endl;

    double best_avg = -1e18; Params best_params;
    int count = 0;
    for(int ae : aco_edges) for(int as : aco_skews) for(double ie : ipr_emas) for(int ied : ipr_edges) for(int isk : ipr_skews) {
        Params p = {ae, as, ie, ied, isk};
        double sum = 0;
        for(auto& d : days) sum += runBacktest(d.p, d.t, p).total_pnl;
        double avg = sum / days.size();
        if (avg > best_avg) { best_avg = avg; best_params = p; }
        count++;
        if (count % 100 == 0) std::cout << "Progress: " << count << "/" << total_combos << " | Best Avg: " << best_avg << std::endl;
    }

    std::cout << "\n=== OPTIMIZATION COMPLETE ===" << std::endl;
    std::cout << "Best Avg PnL: " << best_avg << std::endl;
    std::cout << "ACO: Edge=" << best_params.aco_edge << ", Skew=" << best_params.aco_skew << std::endl;
    std::cout << "IPR: EMA_Alpha=" << best_params.ipr_ema_alpha << ", Edge=" << best_params.ipr_edge << ", Skew=" << best_params.ipr_skew << std::endl;
    return 0;
}