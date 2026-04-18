"""
Round 2 Backtester Wrapper
===========================
Runs trader against all 3 days of Round 2 data.

Usage:
    python backtester_r2.py [trader_file.py]
"""

import sys
import os
import importlib.util

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from backtester import Backtester, plot_results


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
    results = bt.run(trader, position_limits={
        "ASH_COATED_OSMIUM": 80,
        "INTARIAN_PEPPER_ROOT": 80,
    })
    return results


if __name__ == "__main__":
    trader_file = sys.argv[1] if len(sys.argv) > 1 else "trader_r2.py"
    data_dir = "round_2"

    days = ["-1", "0", "1"]
    all_results = {}
    total_pnl = 0

    for day in days:
        pf = os.path.join(data_dir, f"prices_round_2_day_{day}.csv")
        tf = os.path.join(data_dir, f"trades_round_2_day_{day}.csv")
        results = run_single_day(trader_file, day, pf, tf)
        all_results[day] = results
        day_pnl = results["final_pnl"].get("total", 0)
        total_pnl += day_pnl

    print(f"\n{'='*60}")
    print(f"  MULTI-DAY SUMMARY")
    print(f"{'='*60}")
    for day in days:
        fp = all_results[day]["final_pnl"]
        print(f"  Day {day:>2}: ACO={fp.get('ASH_COATED_OSMIUM', 0):>10.2f}  "
              f"IPR={fp.get('INTARIAN_PEPPER_ROOT', 0):>10.2f}  "
              f"Total={fp.get('total', 0):>10.2f}")
    print(f"\n  GRAND TOTAL PnL: {total_pnl:.2f}")
    print(f"  AVERAGE PnL/day: {total_pnl/len(days):.2f}")
    try:
        import importlib.util as _ilu
        _spec = _ilu.spec_from_file_location("_t", trader_file)
        _mod = _ilu.module_from_spec(_spec); _spec.loader.exec_module(_mod)
        maf = _mod.Trader().bid() if hasattr(_mod.Trader, "bid") else 0
    except Exception:
        maf = 0
    print(f"\n  MAF bid: {maf}")
    print(f"  Net after MAF (if accepted): {total_pnl - maf:.2f}")
    print(f"{'='*60}")
