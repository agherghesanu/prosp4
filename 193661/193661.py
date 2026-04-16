from datamodel import OrderDepth, TradingState, Order
from typing import List, Dict
import json


class Trader:
    LIMITS = {"ASH_COATED_OSMIUM": 80, "INTARIAN_PEPPER_ROOT": 80}

    def run(self, state: TradingState):
        result: Dict[str, List[Order]] = {}
        conversions = 0
        ts = self._load(state.traderData)

        for sym in state.order_depths:
            if sym == "ASH_COATED_OSMIUM":
                result[sym] = self._ash_coated_osmium(state, sym, ts)
            elif sym == "INTARIAN_PEPPER_ROOT":
                result[sym], ts = self._intarian_pepper_root(state, sym, ts)
            else:
                result[sym] = []

        return result, conversions, json.dumps(ts)

    # =====================================================================
    # ACO: Fixed fair value at 10000, market-making with OBI skew
    # Live analysis: spread is mostly 16 (62%), fair is stable at 10000
    # Earned ~2763 on live day, optimize further
    # =====================================================================
    def _ash_coated_osmium(self, state: TradingState, sym: str, ts: dict) -> List[Order]:
        orders: List[Order] = []
        od = state.order_depths[sym]
        pos = state.position.get(sym, 0)
        fair = 10000
        limit = self.LIMITS[sym]

        # OBI calculation
        obi = 0.0
        if od.buy_orders and od.sell_orders:
            bid_vol = sum(od.buy_orders.values())
            ask_vol = sum(abs(v) for v in od.sell_orders.values())
            total = bid_vol + ask_vol
            if total > 0:
                obi = (bid_vol - ask_vol) / total

        total_buy_qty = 0
        total_sell_qty = 0
        max_buy = limit - pos
        max_sell = limit + pos

        # ---- Phase 1: Take mispriced orders ----
        for ask_p in sorted(od.sell_orders.keys()):
            if ask_p < fair and total_buy_qty < max_buy:
                vol = abs(od.sell_orders[ask_p])
                qty = min(vol, max_buy - total_buy_qty)
                if qty > 0:
                    orders.append(Order(sym, ask_p, qty))
                    total_buy_qty += qty

        for bid_p in sorted(od.buy_orders.keys(), reverse=True):
            if bid_p > fair and total_sell_qty < max_sell:
                vol = od.buy_orders[bid_p]
                qty = min(vol, max_sell - total_sell_qty)
                if qty > 0:
                    orders.append(Order(sym, bid_p, -qty))
                    total_sell_qty += qty

        eff_pos = pos + total_buy_qty - total_sell_qty

        # ---- Phase 2: Inventory reduction (tighter thresholds for live) ----
        if eff_pos > 10:
            remaining_sell = max_sell - total_sell_qty
            target_reduce = max(0, eff_pos - 5)
            for bid_p in sorted(od.buy_orders.keys(), reverse=True):
                if bid_p >= fair and remaining_sell > 0 and target_reduce > 0:
                    vol = od.buy_orders[bid_p]
                    qty = min(vol, remaining_sell, target_reduce)
                    if qty > 0:
                        orders.append(Order(sym, bid_p, -qty))
                        total_sell_qty += qty
                        remaining_sell -= qty
                        target_reduce -= qty
                        eff_pos -= qty
        elif eff_pos < -10:
            remaining_buy = max_buy - total_buy_qty
            target_reduce = max(0, abs(eff_pos) - 5)
            for ask_p in sorted(od.sell_orders.keys()):
                if ask_p <= fair and remaining_buy > 0 and target_reduce > 0:
                    vol = abs(od.sell_orders[ask_p])
                    qty = min(vol, remaining_buy, target_reduce)
                    if qty > 0:
                        orders.append(Order(sym, ask_p, qty))
                        total_buy_qty += qty
                        remaining_buy -= qty
                        target_reduce -= qty
                        eff_pos += qty

        # Emergency inventory reduction
        if eff_pos > 25:
            remaining_sell = max_sell - total_sell_qty
            target_reduce = max(0, eff_pos - 15)
            for bid_p in sorted(od.buy_orders.keys(), reverse=True):
                if bid_p >= fair - 1 and remaining_sell > 0 and target_reduce > 0:
                    vol = od.buy_orders[bid_p]
                    qty = min(vol, remaining_sell, target_reduce)
                    if qty > 0:
                        orders.append(Order(sym, bid_p, -qty))
                        total_sell_qty += qty
                        remaining_sell -= qty
                        target_reduce -= qty
                        eff_pos -= qty
        elif eff_pos < -25:
            remaining_buy = max_buy - total_buy_qty
            target_reduce = max(0, abs(eff_pos) - 15)
            for ask_p in sorted(od.sell_orders.keys()):
                if ask_p <= fair + 1 and remaining_buy > 0 and target_reduce > 0:
                    vol = abs(od.sell_orders[ask_p])
                    qty = min(vol, remaining_buy, target_reduce)
                    if qty > 0:
                        orders.append(Order(sym, ask_p, qty))
                        total_buy_qty += qty
                        remaining_buy -= qty
                        target_reduce -= qty
                        eff_pos += qty

        # ---- Phase 3: Market-making quotes ----
        remaining_buy = max_buy - total_buy_qty
        remaining_sell = max_sell - total_sell_qty

        inv_skew = round(eff_pos * 3 / limit) if limit > 0 else 0
        obi_adj = round(obi * 2)

        # Base quotes: spread of 14 (bid at 9993, ask at 10007)
        our_bid = 9993 - inv_skew + obi_adj
        our_ask = 10007 - inv_skew + obi_adj

        # Clamp
        our_bid = max(our_bid, 9992)
        our_bid = min(our_bid, 9999)
        our_ask = min(our_ask, 10008)
        our_ask = max(our_ask, 10001)

        # Penny-jump
        inside_bids = [p for p in od.buy_orders if 9991 < p < fair]
        inside_asks = [p for p in od.sell_orders if fair < p < 10009]
        if inside_bids:
            candidate = max(inside_bids) + 1
            if candidate < fair and candidate > our_bid:
                our_bid = candidate
        if inside_asks:
            candidate = min(inside_asks) - 1
            if candidate > fair and candidate < our_ask:
                our_ask = candidate

        if remaining_buy > 0:
            orders.append(Order(sym, our_bid, remaining_buy))
        if remaining_sell > 0:
            orders.append(Order(sym, our_ask, -remaining_sell))

        return orders

    # =====================================================================
    # IPR: Adaptive fair value, pure market-making (NO trend bias)
    # Live analysis shows trend is only ~100/day, not 1000/day
    # Spread is mostly 13 (58%), so we need to be inside that
    # Strategy: EMA fair value, symmetric MM, inventory management
    # =====================================================================
    def _intarian_pepper_root(self, state: TradingState, sym: str, ts: dict) -> tuple:
        orders: List[Order] = []
        od = state.order_depths[sym]
        pos = state.position.get(sym, 0)
        limit = self.LIMITS[sym]

        # --- Fair value estimation ---
        # Use weighted mid price from L1+L2
        raw_fair = self._vwap_l2_mid(od)
        if raw_fair is None:
            raw_fair = ts.get("ipr_ema", 12000)

        # EMA with alpha=0.5 for moderate responsiveness
        EMA_ALPHA = 0.5
        prev = ts.get("ipr_ema", raw_fair)
        ema = EMA_ALPHA * raw_fair + (1 - EMA_ALPHA) * prev
        ts["ipr_ema"] = ema
        fair = ema
        fair_int = int(round(fair))

        # --- Momentum signal (short-term) ---
        momentum = ts.get("ipr_momentum", 0.0)
        momentum *= 0.6  # decay
        market_trades = state.market_trades.get(sym, [])
        for mt in market_trades:
            if mt.price > fair_int:
                momentum += mt.quantity * 0.5
            elif mt.price < fair_int:
                momentum -= mt.quantity * 0.5
        ts["ipr_momentum"] = momentum
        mom_adj = max(-2, min(2, round(momentum / 4)))

        # --- OBI signal ---
        obi = 0.0
        if od.buy_orders and od.sell_orders:
            bid_vol = sum(od.buy_orders.values())
            ask_vol = sum(abs(v) for v in od.sell_orders.values())
            total = bid_vol + ask_vol
            if total > 0:
                obi = (bid_vol - ask_vol) / total
        obi_adj = round(obi * 2)

        # --- Phase 1: Take mispriced orders ---
        max_buy = limit - pos
        max_sell = limit + pos
        total_buy_qty = 0
        total_sell_qty = 0

        if od.sell_orders:
            for ask_p in sorted(od.sell_orders.keys()):
                remaining = max_buy - total_buy_qty
                if ask_p < fair and remaining > 0:
                    vol = abs(od.sell_orders[ask_p])
                    qty = min(vol, remaining)
                    if qty > 0:
                        orders.append(Order(sym, ask_p, qty))
                        total_buy_qty += qty

        if od.buy_orders:
            for bid_p in sorted(od.buy_orders.keys(), reverse=True):
                remaining = max_sell - total_sell_qty
                if bid_p > fair and remaining > 0:
                    vol = od.buy_orders[bid_p]
                    qty = min(vol, remaining)
                    if qty > 0:
                        orders.append(Order(sym, bid_p, -qty))
                        total_sell_qty += qty

        eff_pos = pos + total_buy_qty - total_sell_qty

        # --- Phase 2: Inventory reduction ---
        if eff_pos > 30 and od.buy_orders:
            remaining_sell = max_sell - total_sell_qty
            target_reduce = max(0, eff_pos - 20)
            for bid_p in sorted(od.buy_orders.keys(), reverse=True):
                if bid_p >= fair_int and remaining_sell > 0 and target_reduce > 0:
                    vol = od.buy_orders[bid_p]
                    qty = min(vol, remaining_sell, target_reduce)
                    if qty > 0:
                        orders.append(Order(sym, bid_p, -qty))
                        total_sell_qty += qty
                        remaining_sell -= qty
                        target_reduce -= qty
                        eff_pos -= qty
        elif eff_pos < -30 and od.sell_orders:
            remaining_buy = max_buy - total_buy_qty
            target_reduce = max(0, abs(eff_pos) - 20)
            for ask_p in sorted(od.sell_orders.keys()):
                if ask_p <= fair_int and remaining_buy > 0 and target_reduce > 0:
                    vol = abs(od.sell_orders[ask_p])
                    qty = min(vol, remaining_buy, target_reduce)
                    if qty > 0:
                        orders.append(Order(sym, ask_p, qty))
                        total_buy_qty += qty
                        remaining_buy -= qty
                        target_reduce -= qty
                        eff_pos += qty

        # --- Phase 3: Market-making quotes ---
        remaining_buy = max_buy - total_buy_qty
        remaining_sell = max_sell - total_sell_qty

        best_bid = max(od.buy_orders.keys()) if od.buy_orders else fair_int - 7
        best_ask = min(od.sell_orders.keys()) if od.sell_orders else fair_int + 7
        spread = best_ask - best_bid

        # Inventory skew: bigger skew to manage risk
        inv_fraction = eff_pos / limit if limit > 0 else 0
        skew = round(inv_fraction * 2)

        # Dynamic half-spread based on market spread
        if spread >= 16:
            half = 7
        elif spread >= 14:
            half = 6
        elif spread >= 12:
            half = 5
        elif spread >= 10:
            half = 4
        elif spread >= 6:
            half = 3
        else:
            half = 2

        our_bid = fair_int - half
        our_ask = fair_int + half

        # Apply skew (negative skew shifts quotes down when long)
        our_bid -= skew
        our_ask -= skew

        # Apply signals
        our_bid += mom_adj + obi_adj
        our_ask += mom_adj + obi_adj

        # Penny-jump
        normal_bid = fair_int - 7
        normal_ask = fair_int + 7
        inside_bids = [p for p in od.buy_orders if p > normal_bid and p < fair_int]
        inside_asks = [p for p in od.sell_orders if p < normal_ask and p > fair_int]

        if inside_bids:
            candidate = max(inside_bids) + 1
            if candidate < fair_int and candidate > our_bid:
                our_bid = candidate
        if inside_asks:
            candidate = min(inside_asks) - 1
            if candidate > fair_int and candidate < our_ask:
                our_ask = candidate

        # Safety clamps
        our_bid = min(our_bid, fair_int - 1)
        our_ask = max(our_ask, fair_int + 1)
        our_bid = max(our_bid, best_bid + 1)
        our_ask = min(our_ask, best_ask - 1)
        our_bid = min(our_bid, fair_int - 1)
        our_ask = max(our_ask, fair_int + 1)

        # Place orders with tiered sizing
        if remaining_buy > 0:
            tight = max(1, int(remaining_buy * 0.6))
            wide = remaining_buy - tight
            orders.append(Order(sym, our_bid, tight))
            if wide > 0:
                orders.append(Order(sym, our_bid - 2, wide))

        if remaining_sell > 0:
            tight = max(1, int(remaining_sell * 0.6))
            wide = remaining_sell - tight
            orders.append(Order(sym, our_ask, -tight))
            if wide > 0:
                orders.append(Order(sym, our_ask + 2, -wide))

        return orders, ts

    def _vwap_l2_mid(self, od) -> float:
        """VWAP of L1+L2 for each side, then midpoint."""
        if not od.buy_orders or not od.sell_orders:
            return None

        # Bid side: VWAP of top 2 levels
        bids = sorted(od.buy_orders.keys(), reverse=True)[:2]
        bid_total_vol = sum(od.buy_orders[p] for p in bids)
        bid_vwap = sum(p * od.buy_orders[p] for p in bids) / bid_total_vol if bid_total_vol > 0 else bids[0]

        # Ask side: VWAP of top 2 levels
        asks = sorted(od.sell_orders.keys())[:2]
        ask_total_vol = sum(abs(od.sell_orders[p]) for p in asks)
        ask_vwap = sum(p * abs(od.sell_orders[p]) for p in asks) / ask_total_vol if ask_total_vol > 0 else asks[0]

        return (bid_vwap + ask_vwap) / 2.0

    def _load(self, td: str) -> dict:
        if td and td.strip():
            try: return json.loads(td)
            except: pass
        return {}