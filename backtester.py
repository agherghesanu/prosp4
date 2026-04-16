"""
IMC Prosperity 4 - Local Backtester
=====================================
Simulates the Prosperity trading environment using CSV price/trade data.
Compatible with any Trader class that follows the Prosperity API.

Usage:
    python backtester.py trader.py prices_round_0_day_-1.csv trades_round_0_day_-1.csv

Or programmatically:
    from backtester import Backtester
    bt = Backtester("prices.csv", "trades.csv")
    bt.run(MyTrader())

Outputs:
    - PnL per product and total
    - Trade log
    - Position over time
    - Optional: saves detailed results to CSV
"""

import sys
import os
import csv
import json
import importlib.util
from collections import defaultdict
from typing import Dict, List, Optional, Tuple
from datamodel import (
    OrderDepth, TradingState, Order, Trade, Listing,
    Observation, ProsperityEncoder
)


class Backtester:
    """
    Simulates the Prosperity trading environment.
    
    Matching logic:
    1. Build order book from CSV data each timestamp
    2. Let trader submit orders via run()
    3. Match trader orders against the order book
    4. Track positions, PnL, trades
    """

    def __init__(self, prices_file: str, trades_file: str,
                 match_market_trades: bool = True):
        """
        Args:
            prices_file: Path to prices CSV
            trades_file: Path to trades CSV
            match_market_trades: If True, also match against market trades
        """
        self.prices_data = self._load_prices(prices_file)
        self.trades_data = self._load_trades(trades_file)
        self.match_market_trades = match_market_trades

        # Discover products
        self.products = sorted(set(row["product"] for row in self.prices_data))
        self.timestamps = sorted(set(row["timestamp"] for row in self.prices_data))

        # Pre-build mappings for O(1) lookups
        self._price_map = {}
        for r in self.prices_data:
            ts = r["timestamp"]
            prod = r["product"]
            if ts not in self._price_map:
                self._price_map[ts] = {}
            self._price_map[ts][prod] = r
            
        self._trades_map = defaultdict(lambda: defaultdict(list))
        for t in self.trades_data:
            ts = t["timestamp"]
            prod = t["symbol"]
            self._trades_map[ts][prod].append(Trade(
                symbol=prod,
                price=int(t["price"]),
                quantity=t["quantity"],
                buyer=t["buyer"],
                seller=t["seller"],
                timestamp=ts,
            ))

        print(f"Backtester initialized:")
        print(f"  Products: {self.products}")
        print(f"  Timestamps: {len(self.timestamps)} ({self.timestamps[0]} to {self.timestamps[-1]})")

    def _load_prices(self, filepath: str) -> List[dict]:
        """Load prices CSV."""
        rows = []
        with open(filepath, "r") as f:
            reader = csv.DictReader(f, delimiter=";")
            for row in reader:
                parsed = {
                    "day": int(row["day"]),
                    "timestamp": int(row["timestamp"]),
                    "product": row["product"],
                    "mid_price": float(row["mid_price"]),
                }
                # Parse order book levels
                for level in [1, 2, 3]:
                    bp = row.get(f"bid_price_{level}", "")
                    bv = row.get(f"bid_volume_{level}", "")
                    ap = row.get(f"ask_price_{level}", "")
                    av = row.get(f"ask_volume_{level}", "")
                    parsed[f"bid_price_{level}"] = int(bp) if bp else None
                    parsed[f"bid_volume_{level}"] = int(bv) if bv else None
                    parsed[f"ask_price_{level}"] = int(ap) if ap else None
                    parsed[f"ask_volume_{level}"] = int(av) if av else None
                rows.append(parsed)
        return rows

    def _load_trades(self, filepath: str) -> List[dict]:
        """Load trades CSV."""
        rows = []
        with open(filepath, "r") as f:
            reader = csv.DictReader(f, delimiter=";")
            for row in reader:
                rows.append({
                    "timestamp": int(row["timestamp"]),
                    "buyer": row.get("buyer", ""),
                    "seller": row.get("seller", ""),
                    "symbol": row["symbol"],
                    "currency": row.get("currency", ""),
                    "price": float(row["price"]),
                    "quantity": int(row["quantity"]),
                })
        return rows

    def _build_order_depth(self, timestamp: int, product: str) -> OrderDepth:
        """Build OrderDepth from precomputed dict."""
        od = OrderDepth()
        row = self._price_map.get(timestamp, {}).get(product)
        if not row:
            return od

        # Add bid levels
        for level in [1, 2, 3]:
            bp = row.get(f"bid_price_{level}")
            bv = row.get(f"bid_volume_{level}")
            if bp is not None and bv is not None:
                od.buy_orders[bp] = bv

        # Add ask levels
        for level in [1, 2, 3]:
            ap = row.get(f"ask_price_{level}")
            av = row.get(f"ask_volume_{level}")
            if ap is not None and av is not None:
                od.sell_orders[ap] = -av  # Negative for sells
                
        return od

    def _get_market_trades(self, timestamp: int, product: str) -> List[Trade]:
        """Get market trades for a timestamp and product from precomputed dict."""
        # Return a copy to avoid mutation leaking
        trades = self._trades_map.get(timestamp, {}).get(product, [])
        return [Trade(t.symbol, t.price, t.quantity, t.buyer, t.seller, t.timestamp) for t in trades]

    def _get_mid_price(self, timestamp: int, product: str) -> float:
        """Get the mid price for PnL calculation."""
        row = self._price_map.get(timestamp, {}).get(product)
        if row:
            return row["mid_price"]
        return 0.0

    def _match_orders(self, orders: List[Order], order_depth: OrderDepth,
                      market_trades: List[Trade],
                      position: int, limit: int) -> Tuple[List[Trade], int]:
        """
        Match trader orders against the order book.
        Returns (executed_trades, new_position).
        """
        executed = []
        pos = position

        for order in orders:
            if order.quantity > 0:  # BUY order
                # Check capacity
                max_buy = limit - pos
                if max_buy <= 0:
                    continue
                remaining = min(order.quantity, max_buy)

                # Match against sell orders (ascending price)
                for ask_price in sorted(order_depth.sell_orders.keys()):
                    if remaining <= 0:
                        break
                    if order.price >= ask_price:
                        available = abs(order_depth.sell_orders[ask_price])
                        fill_qty = min(remaining, available)
                        if fill_qty > 0:
                            executed.append(Trade(
                                symbol=order.symbol,
                                price=ask_price,
                                quantity=fill_qty,
                                buyer="SUBMISSION",
                                seller="",
                                timestamp=0,
                            ))
                            pos += fill_qty
                            remaining -= fill_qty
                            order_depth.sell_orders[ask_price] += fill_qty  # reduce (toward 0)
                            if order_depth.sell_orders[ask_price] == 0:
                                del order_depth.sell_orders[ask_price]

                # Match remaining against market trades if enabled
                if remaining > 0 and self.match_market_trades:
                    for mt in market_trades:
                        if remaining <= 0:
                            break
                        if order.price >= mt.price and mt.quantity > 0:
                            fill_qty = min(remaining, mt.quantity)
                            executed.append(Trade(
                                symbol=order.symbol,
                                price=order.price,  # Match at our price
                                quantity=fill_qty,
                                buyer="SUBMISSION",
                                seller="MARKET",
                                timestamp=0,
                            ))
                            pos += fill_qty
                            remaining -= fill_qty
                            mt.quantity -= fill_qty

            elif order.quantity < 0:  # SELL order
                # Check capacity
                max_sell = limit + pos
                if max_sell <= 0:
                    continue
                remaining = min(abs(order.quantity), max_sell)

                # Match against buy orders (descending price)
                for bid_price in sorted(order_depth.buy_orders.keys(), reverse=True):
                    if remaining <= 0:
                        break
                    if order.price <= bid_price:
                        available = order_depth.buy_orders[bid_price]
                        fill_qty = min(remaining, available)
                        if fill_qty > 0:
                            executed.append(Trade(
                                symbol=order.symbol,
                                price=bid_price,
                                quantity=-fill_qty,
                                buyer="",
                                seller="SUBMISSION",
                                timestamp=0,
                            ))
                            pos -= fill_qty
                            remaining -= fill_qty
                            order_depth.buy_orders[bid_price] -= fill_qty
                            if order_depth.buy_orders[bid_price] == 0:
                                del order_depth.buy_orders[bid_price]

                # Match remaining against market trades if enabled
                if remaining > 0 and self.match_market_trades:
                    for mt in market_trades:
                        if remaining <= 0:
                            break
                        if order.price <= mt.price and mt.quantity > 0:
                            fill_qty = min(remaining, mt.quantity)
                            executed.append(Trade(
                                symbol=order.symbol,
                                price=order.price,
                                quantity=-fill_qty,
                                buyer="MARKET",
                                seller="SUBMISSION",
                                timestamp=0,
                            ))
                            pos -= fill_qty
                            remaining -= fill_qty
                            mt.quantity -= fill_qty

        return executed, pos

    def run(self, trader, position_limits: Optional[Dict[str, int]] = None,
            verbose: bool = True) -> dict:
        """
        Run the backtest.
        
        Args:
            trader: A Trader instance with a run() method
            position_limits: Dict of product -> limit. Default 50 for all.
            verbose: Print progress
            
        Returns:
            Dict with PnL, trades, positions data
        """
        if position_limits is None:
            # Auto-detect from trader's LIMITS attribute if available
            if hasattr(trader, 'LIMITS') and isinstance(trader.LIMITS, dict):
                position_limits = {p: trader.LIMITS.get(p, 50) for p in self.products}
                if verbose:
                    print(f"  Auto-detected position limits from trader: {position_limits}")
            else:
                position_limits = {p: 50 for p in self.products}

        # State
        positions: Dict[str, int] = {p: 0 for p in self.products}
        cash: Dict[str, float] = {p: 0.0 for p in self.products}
        traderData = ""
        all_trades: List[dict] = []
        position_history: List[dict] = []
        pnl_history: List[dict] = []
        own_trades_prev: Dict[str, List[Trade]] = {p: [] for p in self.products}

        total_iterations = len(self.timestamps)

        for i, timestamp in enumerate(self.timestamps):
            # Build state
            order_depths = {}
            market_trades_state = {}
            listings = {}

            for product in self.products:
                order_depths[product] = self._build_order_depth(timestamp, product)
                market_trades_state[product] = self._get_market_trades(timestamp, product)
                listings[product] = Listing(product, product, "SEASHELLS")

            state = TradingState(
                traderData=traderData,
                timestamp=timestamp,
                listings=listings,
                order_depths=order_depths,
                own_trades=own_trades_prev,
                market_trades=market_trades_state,
                position=dict(positions),
                observations=Observation(),
            )

            # Call trader
            try:
                result = trader.run(state)
                if isinstance(result, tuple):
                    if len(result) == 3:
                        orders_dict, conversions, traderData = result
                    elif len(result) == 2:
                        orders_dict, traderData = result
                    else:
                        orders_dict = result[0]
                else:
                    orders_dict = result
                    traderData = ""
            except Exception as e:
                if verbose:
                    print(f"  ERROR at t={timestamp}: {e}")
                orders_dict = {}

            # Match orders and update positions
            own_trades_this = {}
            for product in self.products:
                product_orders = orders_dict.get(product, [])
                if not product_orders:
                    own_trades_this[product] = []
                    continue

                # Rebuild order depth for matching (deep copy)
                od = self._build_order_depth(timestamp, product)
                mt = self._get_market_trades(timestamp, product)

                executed, new_pos = self._match_orders(
                    product_orders, od, mt,
                    positions[product], position_limits.get(product, 50)
                )

                own_trades_this[product] = executed

                # Update cash from trades
                for trade in executed:
                    if trade.quantity > 0:  # We bought
                        cash[product] -= trade.price * trade.quantity
                    else:  # We sold
                        cash[product] += trade.price * abs(trade.quantity)

                positions[product] = new_pos

                # Log trades
                for trade in executed:
                    all_trades.append({
                        "timestamp": timestamp,
                        "product": product,
                        "price": trade.price,
                        "quantity": trade.quantity,
                        "position_after": new_pos,
                    })

            own_trades_prev = own_trades_this

            # Calculate PnL (cash + position * mid_price)
            pnl_row = {"timestamp": timestamp}
            total_pnl = 0
            for product in self.products:
                mid = self._get_mid_price(timestamp, product)
                product_pnl = cash[product] + positions[product] * mid
                pnl_row[product] = product_pnl
                total_pnl += product_pnl
            pnl_row["total"] = total_pnl
            pnl_history.append(pnl_row)

            position_history.append({"timestamp": timestamp, **positions})

            # Progress
            if verbose and (i + 1) % 2000 == 0:
                print(f"  Progress: {i+1}/{total_iterations} "
                      f"(PnL: {total_pnl:.0f})")

        # ---- Final Results ----
        final_pnl = pnl_history[-1] if pnl_history else {}

        if verbose:
            print(f"\n{'='*50}")
            print(f"  BACKTEST RESULTS")
            print(f"{'='*50}")
            print(f"  Total trades: {len(all_trades)}")
            for product in self.products:
                prod_trades = [t for t in all_trades if t["product"] == product]
                print(f"\n  {product}:")
                print(f"    Trades: {len(prod_trades)}")
                print(f"    Final position: {positions[product]}")
                print(f"    PnL: {final_pnl.get(product, 0):.2f}")
            print(f"\n  TOTAL PnL: {final_pnl.get('total', 0):.2f}")
            print(f"{'='*50}")

        return {
            "pnl": pnl_history,
            "trades": all_trades,
            "positions": position_history,
            "final_pnl": final_pnl,
            "final_positions": dict(positions),
        }


def plot_results(results: dict, products: List[str]):
    """Plot PnL and positions over time."""
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed, skipping plots")
        return

    pnl_df = results["pnl"]
    pos_df = results["positions"]
    trades = results["trades"]

    timestamps = [r["timestamp"] for r in pnl_df]

    fig, axes = plt.subplots(len(products) + 1, 2, figsize=(16, 4 * (len(products) + 1)))

    # PnL plots
    for i, product in enumerate(products):
        ax = axes[i][0]
        pnl_vals = [r[product] for r in pnl_df]
        ax.plot(timestamps, pnl_vals, linewidth=0.8)
        ax.set_title(f"{product} - PnL")
        ax.set_ylabel("PnL (SeaShells)")
        ax.grid(True, alpha=0.3)

        ax = axes[i][1]
        pos_vals = [r[product] for r in pos_df]
        ax.plot(timestamps, pos_vals, linewidth=0.8, color="green")
        ax.set_title(f"{product} - Position")
        ax.set_ylabel("Position")
        ax.grid(True, alpha=0.3)

    # Total PnL
    ax = axes[-1][0]
    total_pnl = [r["total"] for r in pnl_df]
    ax.plot(timestamps, total_pnl, linewidth=1.0, color="black")
    ax.set_title("Total PnL")
    ax.set_xlabel("Timestamp")
    ax.set_ylabel("PnL (SeaShells)")
    ax.grid(True, alpha=0.3)

    axes[-1][1].axis("off")

    plt.tight_layout()
    plt.savefig("backtest_results.png", dpi=150, bbox_inches="tight")
    plt.close()
    print("Saved: backtest_results.png")


# ============================================================
# CLI ENTRY POINT
# ============================================================
if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: python backtester.py <trader.py> <prices.csv> <trades.csv>")
        print()
        print("Example:")
        print("  python backtester.py trader.py prices_round_0_day_-1.csv trades_round_0_day_-1.csv")
        sys.exit(1)

    trader_file = sys.argv[1]
    prices_file = sys.argv[2]
    trades_file = sys.argv[3]

    # Dynamically import trader module
    spec = importlib.util.spec_from_file_location("trader_module", trader_file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    trader = mod.Trader()

    # Run backtest
    bt = Backtester(prices_file, trades_file)
    results = bt.run(trader)

    # Plot if matplotlib available
    plot_results(results, bt.products)

    # Save detailed results
    import csv as csv_mod
    with open("backtest_trades.csv", "w", newline="") as f:
        if results["trades"]:
            writer = csv_mod.DictWriter(f, fieldnames=results["trades"][0].keys())
            writer.writeheader()
            writer.writerows(results["trades"])
            print("Saved: backtest_trades.csv")

    with open("backtest_pnl.csv", "w", newline="") as f:
        if results["pnl"]:
            writer = csv_mod.DictWriter(f, fieldnames=results["pnl"][0].keys())
            writer.writeheader()
            writer.writerows(results["pnl"])
            print("Saved: backtest_pnl.csv")
