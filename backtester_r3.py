"""
Round 3 Backtester Wrapper
===========================
Runs trader against all 3 days of Round 3 data.

Usage:
    python backtester_r3.py [trader_file.py]
"""

import sys
import os
import importlib.util

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from backtester import Backtester


def load_trader(filepath):
    spec = importlib.util.spec_from_file_location("trader_module", filepath)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.Trader()


def run_single_day(trader_file, day_label, prices_file, trades_file):
    print(f"\n{'#'*60}")
    print(f"  DAY {day_label}")
    print(f"{'#'*60}")
    trader = load_trader(trader_file)
    bt = Backtester(prices_file, trades_file)
    # Let trader's LIMITS dict handle position limits (auto-detected by backtester)
    results = bt.run(trader)
    return results


if __name__ == "__main__":
    trader_file = sys.argv[1] if len(sys.argv) > 1 else "_test_r3_initial.py"
    data_dir = "ROUND_3"

    days = ["0", "1", "2"]
    all_results = {}
    total_pnl = 0

    for day in days:
        pf = os.path.join(data_dir, f"prices_round_3_day_{day}.csv")
        tf = os.path.join(data_dir, f"trades_round_3_day_{day}.csv")
        results = run_single_day(trader_file, day, pf, tf)
        all_results[day] = results
        day_pnl = results["final_pnl"].get("total", 0)
        total_pnl += day_pnl

    print(f"\n{'='*60}")
    print(f"  MULTI-DAY SUMMARY")
    print(f"{'='*60}")

    # Get all products from results
    products = sorted(set(k for r in all_results.values()
                          for k in r["final_pnl"].keys() if k != "total"))

    for day in days:
        fp = all_results[day]["final_pnl"]
        parts = "  ".join(f"{p}={fp.get(p, 0):>10.2f}" for p in products)
        print(f"  Day {day:>2}: {parts}  Total={fp.get('total', 0):>10.2f}")

    print(f"\n  GRAND TOTAL PnL: {total_pnl:.2f}")
    print(f"  AVERAGE PnL/day: {total_pnl/len(days):.2f}")
    print(f"{'='*60}")
