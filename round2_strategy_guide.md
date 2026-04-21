# IMC Prosperity 4 — Round 2 Strategy Guide
> Full technical reference for a new Claude Code instance. Covers data structure, algorithms, backtester, Monte Carlo simulator, empirical findings, and how to approach Round n(3,4,5 remaining round).

---

## 1. Competition Overview

**Platform**: IMC Prosperity 4 trading competition.
**Goal**: Maximise PnL (measured in SeaShells) by submitting a `Trader` class.
**Submission**: A single Python file containing a `class Trader` with a `run(state: TradingState)` method.
**Evaluation**: The platform runs the trader against live market data tick-by-tick. Each round introduces new products.

### Round 2 Products

| Symbol | Full Name | Limit | Behaviour |
|--------|-----------|-------|-----------|
| `ASH_COATED_OSMIUM` | ACO | ±80 | Mean-reverting around ~10,000. Spread typically 14–20 ticks. |
| `INTARIAN_PEPPER_ROOT` | IPR | ±80 | Drifting UP ~+1,000 SeaShells per day. Pure trend. |

**Market Access Fee (MAF)**: Round 2 has a blind auction where you submit a `bid()` return value. The top 50% of bidders pay their bid and receive 25% extra order-book volume. Current bid: **10,000** (break-even estimate). The `bid()` method is separate from `run()`.

---

## 2. File Structure

```
prosp/
├── trader_r2.py              # CURRENT BEST SUBMISSION (287,667 PnL 3-day backtest)
├── backtester.py             # Core backtester engine (Backtester class)
├── backtester_r2.py          # Round 2 wrapper (runs 3 days, prints summary)
├── monte_carlo_r2.py         # Monte Carlo risk simulator (block bootstrap)
├── datamodel.py              # Platform data structures (OrderDepth, TradingState, etc.)
├── ROUND_2/
│   ├── prices_round_2_day_{-1,0,1}.csv   # Price/order book data (~10,000 ticks/day/product)
│   └── trades_round_2_day_{-1,0,1}.csv   # Market trade events (~790/day)
├── 292660/292660.py          # Live submission 1 (ACO_OBI_SLOPE=6.0, TAKE_EDGE=1)
├── 292957/292957.py          # Live submission 2 (ACO_OBI_SLOPE=5.0, TAKE_EDGE=0)
└── docs/
    └── round2_strategy_guide.md  # This file
```

---

## 3. Data Format

### prices CSV (semicolon-delimited)

```
day;timestamp;product;bid_price_1;bid_volume_1;bid_price_2;bid_volume_2;bid_price_3;bid_volume_3;
ask_price_1;ask_volume_1;ask_price_2;ask_volume_2;ask_price_3;ask_volume_3;mid_price;profit_and_loss
```

- `timestamp`: 0, 100, 200, … (100 units = 1 tick, 10,000 ticks/day)
- Up to 3 bid levels and 3 ask levels. Missing levels are empty strings.
- `mid_price` = (best_bid + best_ask) / 2 when both sides present.
- Rows interleaved: ACO and IPR share the same timestamp column.

### trades CSV (semicolon-delimited)

```
timestamp;buyer;seller;symbol;currency;price;quantity
```

- Sparse: ~790 rows/day vs 10,000 price ticks.
- `buyer`/`seller` often empty (XIRECS = platform maker).
- These are **market trades** — not our orders. Used for passive fill simulation.

---

## 4. Platform API (datamodel.py)

```python
class TradingState:
    traderData: str           # JSON string passed from previous tick
    timestamp: int
    listings: Dict[str, Listing]
    order_depths: Dict[str, OrderDepth]
    own_trades: Dict[str, List[Trade]]   # our fills from last tick
    market_trades: Dict[str, List[Trade]]
    position: Dict[str, int]             # current position per product
    observations: Observation

class OrderDepth:
    buy_orders:  Dict[int, int]   # price -> positive volume
    sell_orders: Dict[int, int]   # price -> NEGATIVE volume (convention)

class Order:
    symbol: str
    price: int
    quantity: int  # positive = buy, negative = sell

# Trader contract:
class Trader:
    def run(self, state: TradingState) -> Tuple[Dict[str, List[Order]], int, str]:
        # returns: (orders_dict, conversions, traderData_json)
    
    def bid(self) -> int:
        # MAF bid (Round 2 only)
```

---

## 5. ACO Strategy — Market Making

File: `trader_r2.py`, method `_ash_coated_osmium()`

### Key Empirical Facts (from data analysis)

| Finding | Value |
|---------|-------|
| Lag-1 price return autocorrelation | **-0.48** (strong mean-reversion) |
| OBI L1 correlation with next tick price | **+0.586** |
| Spread distribution | normal=16 (63.7%), wide≥18 (25.5%), narrow<15 (7.8%) |
| OBI slope by regime | spread<15 → 6.2, spread=16 → 0 (zero predictive!), spread≥18 → 3.5 |
| Phase 1 takes occurrence | Only in narrow-spread (<15) regime — zero takes in normal/wide |
| Adverse selection cost | ~2.6 ticks avg — far less than half-spread revenue (~8 ticks) |

### OBI L1 Definition

```python
def _obi_l1(self, od: OrderDepth) -> float:
    bb = max(od.buy_orders)
    ba = min(od.sell_orders)
    bv = od.buy_orders[bb]        # best bid volume
    av = abs(od.sell_orders[ba])  # best ask volume
    t = bv + av
    return (bv - av) / t          # range [-1, +1]; positive = bid-heavy
```

**Critical**: Use L1 only. Full-book OBI is NEGATIVELY correlated (-0.09) because market makers place walls adversarially on the wrong side.

### Fair Value Calculation

```python
obi      = self._obi_l1(od)
fair_raw = (bb + ba) / 2.0 + ACO_OBI_SLOPE * obi   # ACO_OBI_SLOPE = 5.0
fair     = int(round(fair_raw))
```

The OBI adjustment shifts fair value toward the imbalance direction.

### Phase Structure (executed in order each tick)

**Phase 1 — Take mispriced orders**
```python
TAKE_EDGE = 0  # take at fair or better
# BUY: take any ask at or below fair
# SELL: take any bid at or above fair
```
- In practice triggers only in narrow-spread regime (ask = mid+7 ≤ fair → possible when spread=14)
- With spread=16, asks are always at mid+8 > fair → never triggered

**Phase 2 — Soft inventory reduction**
```python
# Trigger: |eff| > 25, Target: ±10
# Sell into bids at or above fair (long reduction)
# Buy from asks at or below fair (short reduction)
```

**Phase 3 — Emergency inventory reduction**
```python
ACO_HARD_THRESH = 40  # trigger
# Target: ±20
# Sell into bids ≥ fair-1 (one tick looser)
# Buy from asks ≤ fair+1 (one tick looser)
```

**Phase 4 — Passive quote (main PnL source)**
```python
ACO_K_BASE = 20  # large value ensures penny-jump always wins

skew = _exp_skew(eff, limit, max_skew=5.0)  # exponential position skew
our_bid = fair - ACO_K_BASE - skew
our_ask = fair + ACO_K_BASE - skew

# Penny-jump: improve to best_bid+1 or best_ask-1 if possible
if bb + 1 < fair: our_bid = max(our_bid, bb + 1)
if ba - 1 > fair: our_ask = min(our_ask, ba - 1)

# Clamp: always quote inside fair (never cross it)
our_bid = min(our_bid, fair - 1)
our_ask = max(our_ask, fair + 1)
```

**Why K_BASE=20 (large) works**: With K_BASE=20 and typical fair≈mid, `fair - 20` is far inside the book. The penny-jump correction then pulls `our_bid` to `bb+1` (one tick above best bid), effectively making us the price-improved best bid. This gets filled much more often than a wide quote.

**Exponential inventory skew**:
```python
def _exp_skew(eff_pos, limit, max_skew):
    ratio = eff_pos / limit            # [-1, +1]
    curve = (exp(|ratio| * 2.5) - 1) / (exp(2.5) - 1)
    return sign(ratio) * curve * max_skew  # max ±5 ticks
```
When long (eff > 0): skew > 0 → both bid and ask shift DOWN → we buy cheaper, sell sooner.

**OBI_EXTREME** (currently disabled at 99): Originally suppressed wrong-side quotes when |OBI| > 0.4. Disabled because suppressing quotes HURTS — adverse selection cost (~2.6 ticks) is far less than the spread revenue (~8 ticks half-spread) we'd lose.

### Current Parameters (Optimised)

```python
ACO_OBI_SLOPE   = 5.0   # optimizer peak; smooth curve 3→8
ACO_K_BASE      = 20    # large → penny-jump enforces bb+1/ba-1
TAKE_EDGE       = 0     # take at fair or better (not edge=1)
OBI_EXTREME     = 99    # disabled (was 0.4)
ACO_HARD_THRESH = 40    # emergency threshold; lower = more aggressive
```

### What Was Tested and Failed

| Test | PnL | vs Baseline | Conclusion |
|------|-----|-------------|------------|
| Regime-based OBI slope + OBI_EXTREME=0.4 | 285,108 | -2,559 | OBI suppression kills spread income |
| Regime-based slope only | 287,121 | -546 | Setting slope=0 in normal regime hurts |
| IPR_BUY_PREMIUM=8 | 287,550 | -117 | Slightly worse than 7 |

---

## 6. IPR Strategy — Buy and Hold

File: `trader_r2.py`, method `_intarian_pepper_root()`

### Key Insight

IPR drifts **+1,000 SeaShells per day**. The optimal strategy is to reach position=+80 (the limit) as fast as possible from tick 1 and hold forever. Never sell.

Selling a unit of IPR sacrifices all future drift on that unit (~1,000/day × remaining days).

### Implementation

```python
IPR_BUY_PREMIUM = 7  # take any ask up to mid + 7

# Step 1: Aggressively take asks up to buy_threshold
buy_threshold = int(round(mid)) + IPR_BUY_PREMIUM
for ask_p in sorted(od.sell_orders.keys()):
    if ask_p <= buy_threshold:
        # fill as much as possible

# Step 2: Passive bid at best_bid+1 (capped at buy_threshold)
our_bid = max(od.buy_orders) + 1
our_bid = min(our_bid, min(od.sell_orders) - 1)  # stay inside spread
our_bid = min(our_bid, buy_threshold)
orders.append(Order(sym, our_bid, remaining))
```

**Why premium=7**: The typical IPR spread is ~15 ticks (mid±7.5). Setting premium=7 captures asks at mid+7 = ba (the best ask), filling immediately. Premium=8 also works (slightly different) but 7 is marginally better in backtests.

**Typical daily PnL**: ~79,300 per day (position=80, drift≈991 SeaShells/unit/day).

---

## 7. Backtester

### Core Engine: `backtester.py`

```python
from backtester import Backtester

bt = Backtester("prices.csv", "trades.csv")
results = bt.run(trader, position_limits={"ACO": 80, "IPR": 80})
```

**Matching logic** (per tick):
1. Build `OrderDepth` from the CSV row (up to 3 bid/ask levels)
2. Call `trader.run(state)` → get orders
3. Match each order against the order book:
   - **Active orders**: match against book levels at our price or better
   - **Passive orders**: match against market trades if `trade.price` crosses our quote
4. Update `cash` and `position`
5. Mark PnL = `cash + position × mid_price`

### Round 2 Wrapper: `backtester_r2.py`

```bash
# Run current best strategy:
python backtester_r2.py trader_r2.py

# Run a test file:
python backtester_r2.py _test_obi_inv.py
```

Output format:
```
Day -1: ACO=16,428  IPR=79,321  Total=95,749
Day  0: ACO=16,935  IPR=79,414  Total=96,349
Day  1: ACO=16,286  IPR=79,364  Total=95,650
GRAND TOTAL PnL: 287,667
Net after MAF:   277,667
```

**Data directory**: The backtester_r2.py looks for data in `round_2/` (lowercase). The actual folder is `ROUND_2/` (uppercase). On Windows this doesn't matter (case-insensitive filesystem), but on Linux it would fail.

**Baseline**: `trader_r2.py` → **287,667** PnL (3-day). This is the number to beat.

---

## 8. Monte Carlo Simulator

File: `monte_carlo_r2.py`

### Purpose

Estimate the **distribution** of 3-day PnL outcomes using circular block bootstrap. Answers:
- How robust is the 287k baseline?
- What's the downside risk (5th percentile)?
- Which parameters are actually optimal vs overfit?

### Algorithm

1. **Load data**: All 3 days into numpy arrays (10,000 ticks × per product per day).
2. **IPR**: Always run on actual tick sequence (no bootstrap) — IPR is a trending buy-and-hold, bootstrapping scrambles the trend and gives meaningless results.
3. **ACO**: Circular block bootstrap.
   - Sample `ceil(N / block_size)` random start indices from [0, N)
   - Concatenate blocks of `block_size` consecutive ticks, wrapping circularly
   - Result: shuffled tick sequence of same length N, preserving short-range autocorrelation
4. Run inline ACO strategy on bootstrap ticks → compute PnL
5. Repeat N_sims times → distribution

### Usage

```bash
# Basic 300-sim run:
python monte_carlo_r2.py

# 1000 sims with max parallelism:
python monte_carlo_r2.py --sims 1000 --workers 0

# With parameter sensitivity scan:
python monte_carlo_r2.py --sims 500 --params

# Larger blocks = more autocorrelation preserved:
python monte_carlo_r2.py --block 500 --sims 300
```

### Interpreting Results

```
Baseline (actual data):    ACO=49,649  IPR=238,099  TOTAL=287,748
MC Mean:                   ACO=47,119  IPR=238,099  TOTAL=285,218
MC Median:                 ACO=49,946  IPR=238,099  TOTAL=288,046
MC Std Dev:                ACO=44,426
Min:                       ACO=-666,990  ← rare catastrophic outlier (bad bootstrap)
5th pct:                   TOTAL=285,154
95th pct:                  TOTAL=290,877
P(total > 0):              99.6%
```

**Key**: Use **median** and **5th/95th pct** as primary metrics. The large std dev and extreme min come from rare catastrophic bootstrap sequences (pathological inventory blowup) — not realistic live scenarios. The median (288,046) tracks the baseline well.

### Parameter Sensitivity (from MC scan with N=100 paired sims)

| Parameter | Tested Range | Best Value | Current | MC Mean Diff |
|-----------|-------------|------------|---------|-------------|
| obi_slope | 3.0–7.0 | **6.0** (+960) | 5.0 | +960 |
| hard_thresh | 25–80 | **25–30** | 40 | +225 |
| take_edge | 0–2 | **0** | 0 | confirmed |
| buy_premium | 5–9 | **7** | 7 | confirmed |

**Action item**: Test `ACO_OBI_SLOPE=6.0` in the full backtester — MC suggests +960 improvement over current 5.0.

---

## 9. Development Workflow

### Standard Alpha Testing Process

1. **Copy** `trader_r2.py` to `_test_<name>.py`
2. **Modify** the test file (change parameters or logic)
3. **Backtest** and compare to baseline:
   ```bash
   py backtester_r2.py _test_<name>.py 2>&1 | grep -E "(Day|GRAND|Net)"
   ```
4. **Decide**: keep if GRAND TOTAL > 287,667
5. **MC verify**: run `monte_carlo_r2.py --params` to check it's not overfit

### Key Rules

- **Never test on the submission file** (`trader_r2.py`). Always copy to `_test_*.py`.
- Always compare exact GRAND TOTAL vs **287,667** (3-day baseline).
- A change that improves by <500 is likely noise; require >1,000 improvement to act.
- Run `--params` MC scan to verify the improvement holds across bootstrap samples.

### Encoding Warning (Windows Python 3.14)

When writing Python scripts with Unicode characters (→, ←, ±), add this at the top:
```python
import sys
sys.stdout.reconfigure(encoding='utf-8')
```
Or avoid Unicode arrows entirely — use `->`  in comments as ASCII.

---

## 10. Live Submission History

| Submission | File | Key Params | Live Day 1 PnL | ACO Pos | IPR Pos |
|------------|------|-----------|----------------|---------|---------|
| 292660 | 292660.py | slope=6.0, edge=1, thresh=50 | 8,834 | +8 | +80 |
| 292957 | 292957.py | slope=5.0, edge=0, thresh=40 | 8,811 | -10 | +80 |

**Live vs backtest**: Live has ~1,000 timestamps/day vs backtester's 10,000. Live PnL ≈ 1/10th of backtest prediction. Both submissions are consistent with their backtest predictions.

---

## 11. How to Approach Round 3

### Step 1: Read the Round 3 Problem Statement

Round 3 will introduce new products. Read the description carefully:
- Is the product mean-reverting or trending?
- What are the position limits?
- Are there conversion mechanisms? (conversions are the 2nd return value of `run()`)
- Is there a MAF or other fee structure?

### Step 2: Analyse the New Data

```bash
ls ROUND_3/  # check what CSV files exist
head -5 ROUND_3/prices_round_3_day_-1.csv  # inspect format
```

Run a quick analysis script to extract:
```python
# Key statistics to compute:
# 1. ACF lag-1 of mid-price returns (negative = mean-revert, positive = trend)
# 2. OBI L1 vs next-tick return correlation (OBI slope)
# 3. Spread distribution (normal, wide, narrow regimes)
# 4. OBI by spread regime (slope varies!)
# 5. Volume profile (how many levels, typical volume at L1)
# 6. Adverse selection cost at L1 (does high OBI predict fills being bad?)
```

### Step 3: Classify Each New Product

| Behaviour | Strategy Template |
|-----------|------------------|
| Mean-reverting (ACF < -0.3) | Market-making (ACO template) |
| Trending (ACF > +0.3) | Buy-and-hold or directional (IPR template) |
| Flat with regime changes | Regime-switching hybrid |
| Correlated with known product | Cross-product signal |

### Step 4: Build the Round 3 Trader

Structure: copy `trader_r2.py` and add new product handlers. The `run()` method dispatches by symbol:

```python
def run(self, state: TradingState):
    result = {}
    for sym in state.order_depths:
        if sym == "ASH_COATED_OSMIUM":
            result[sym] = self._aco(state, sym)
        elif sym == "INTARIAN_PEPPER_ROOT":
            result[sym] = self._ipr(state, sym)
        elif sym == "NEW_PRODUCT":
            result[sym] = self._new_product(state, sym)
        else:
            result[sym] = []
    return result, 0, json.dumps(ts)
```

### Step 5: Create Round 3 Backtester Wrapper

Copy `backtester_r2.py` to `backtester_r3.py` and update:
- `data_dir = "ROUND_3"`
- `days = ["-1", "0", "1"]` (or whatever days R3 provides)
- `position_limits` for new products

### Step 6: Optimise Parameters

Use the same MC pipeline:
1. Extend `monte_carlo_r2.py` → `monte_carlo_r3.py`
2. Add new product's fast inline strategy
3. Run `--params` scan to find optimal parameters
4. Verify improvements hold across bootstrap samples (not overfit to 3-day history)

### Step 7: Evaluate Submission Readiness

Checklist before submitting:
- [ ] GRAND TOTAL > previous best
- [ ] MC 5th percentile > 0 (no bankruptcy risk)
- [ ] All 3 days individually positive
- [ ] No position limit violations (check final positions)
- [ ] `bid()` returns correct MAF value

---

## 12. Useful Code Snippets

### Quick data inspection
```python
import csv, collections

rows = list(csv.DictReader(open("ROUND_2/prices_round_2_day_-1.csv"), delimiter=";"))
aco = [r for r in rows if r["product"] == "ASH_COATED_OSMIUM"]
print(f"Ticks: {len(aco)}")
mids = [float(r["mid_price"]) for r in aco if r["mid_price"]]
print(f"Mid range: {min(mids):.1f} – {max(mids):.1f}")
```

### Quick autocorrelation check
```python
import numpy as np
mids = np.array(mids)
rets = np.diff(mids)
acf1 = np.corrcoef(rets[:-1], rets[1:])[0, 1]
print(f"ACF lag-1: {acf1:.3f}")  # ACO: -0.48 (strong mean-revert)
```

### Quick OBI slope estimation
```python
# For each tick: compute OBI and next-tick mid change
# Then: slope = cov(obi, delta_mid) / var(obi)
obis, diffs = [], []
for i in range(len(aco)-1):
    r, r2 = aco[i], aco[i+1]
    bb = int(r["bid_price_1"] or 0)
    ba = int(r["ask_price_1"] or 0)
    bv = int(r["bid_volume_1"] or 0)
    av = int(r["ask_volume_1"] or 0)
    if bb and ba and bv + av > 0:
        obi = (bv - av) / (bv + av)
        dm = float(r2["mid_price"] or 0) - float(r["mid_price"] or 0)
        obis.append(obi); diffs.append(dm)
slope = np.cov(obis, diffs)[0,1] / np.var(obis)
print(f"OBI slope: {slope:.2f}")  # ACO: ~5.0-6.0
```

### Running a quick backtest
```bash
py backtester_r2.py trader_r2.py 2>&1 | grep -E "(Day|GRAND|Net)"
```

### Running the MC simulator
```bash
py monte_carlo_r2.py --sims 300 --params
```

---

## 13. Current Best Numbers (End of Round 2)

| Metric | Value |
|--------|-------|
| Best backtest PnL (3-day) | **287,667** |
| ACO contribution | ~49,649 |
| IPR contribution | ~238,099 |
| MAF bid | 10,000 |
| Net after MAF | ~277,667 |
| MC median | ~288,046 |
| MC 5th percentile | ~285,154 |
| MC P(profit > 0) | 99.6% |
| Live Day 1 best (submission 292957) | 8,811 |

---

## 14. Important Warnings

1. **Do not bootstrap IPR**: IPR is a trending product. Block bootstrap scrambles the price trend, giving wildly wrong PnL estimates (can show -666k). Always run IPR on actual data.

2. **MC std dev is inflated**: The large std dev (~44k) in MC results is from rare catastrophic bootstrap sequences — not realistic risk. Use median and percentiles.

3. **Backtester ≠ live**: Live PnL ≈ backtest / 10. The backtester uses 10,000 ticks/day; live has ~1,000.

4. **Parameter optimisation overfits**: Only 3 days of data. A parameter that looks better by <1,000 PnL is likely noise. The MC paired-sim scan (same bootstrap seeds across parameters) is more reliable than single-path backtests.

5. **OBI full-book is NEGATIVE**: Only use L1 OBI. Full-book OBI (all levels) is negatively correlated with future price (-0.09) because MM walls are adversarial.

6. **Phase 1 takes are rare**: With ACO spread=16, the best ask is always mid+8 which is ≥ fair (since OBI adjustment ≤ 5 ticks). Takes only happen in narrow-spread regime (spread<15, 7.8% of ticks).

---

## 15. Advanced Frameworks: Options, Futures, and Correlated Instruments

> Use this section if Round 3+ introduces derivatives, futures curves, or multiple correlated underlyings.

---

### 15.1 Multi-Asset Price Dynamics (GBM)

For `n` correlated assets the standard risk-neutral dynamics are:

```
dS_i(t) = μ_i · S_i(t) dt + σ_i · S_i(t) dW_i(t)
dW_i · dW_j = ρ_ij dt
```

In the IMC context `μ_i` is NOT the risk-free rate — it is the **empirical drift** extracted from the price data (e.g. IPR's +1,000/day drift). Use historical drift, not risk-neutral drift, unless you are explicitly pricing options to sell back to the market.

---

### 15.2 Cholesky Correlation Sampling

```python
import numpy as np
from scipy.linalg import cholesky

class CorrelatedGBM:
    """Simulate correlated GBM paths for n assets."""

    def __init__(self, S0, mu, sigma, corr, dt=1/10000):
        self.S0    = np.array(S0, dtype=float)
        self.mu    = np.array(mu, dtype=float)
        self.sigma = np.array(sigma, dtype=float)
        self.dt    = dt
        self.L     = cholesky(np.array(corr), lower=True)

    def step(self, S_prev, rng):
        """One tick forward. Returns S_next array."""
        Z  = rng.standard_normal(len(S_prev))
        dW = self.L @ Z * np.sqrt(self.dt)
        return S_prev * np.exp(
            (self.mu - 0.5 * self.sigma**2) * self.dt
            + self.sigma * dW
        )

    def simulate(self, n_steps, n_sims, seed=0):
        """Returns (n_steps+1, n_sims, n_assets) array."""
        rng = np.random.default_rng(seed)
        n = len(self.S0)
        S = np.zeros((n_steps + 1, n_sims, n))
        S[0] = self.S0
        for t in range(1, n_steps + 1):
            Z  = rng.standard_normal((n_sims, n))
            dW = (self.L @ Z.T).T * np.sqrt(self.dt)
            S[t] = S[t-1] * np.exp(
                (self.mu - 0.5 * self.sigma**2) * self.dt
                + self.sigma * dW
            )
        return S
```

**Calibration from historical data** (first thing to run on new product data):

```python
def calibrate_gbm(mid_prices: np.ndarray, dt: float = 1/10000):
    """Estimate mu, sigma from tick-level mid prices."""
    log_rets = np.diff(np.log(mid_prices))
    mu    = log_rets.mean() / dt
    sigma = log_rets.std()  / np.sqrt(dt)
    return mu, sigma   # annualised / tick-normalised

def calibrate_correlation(price_matrix: np.ndarray):
    """
    price_matrix: (T, n_assets)
    Returns (n_assets, n_assets) correlation matrix.
    """
    log_rets = np.diff(np.log(price_matrix), axis=0)
    return np.corrcoef(log_rets.T)
```

---

### 15.3 Options Pricing

#### Black-Scholes (vanilla calls/puts on a single asset)

```python
from scipy.stats import norm
import numpy as np

def bs_price(S, K, T, sigma, r=0, flag='call'):
    """Black-Scholes price. T in same units as sigma^2·T."""
    if T <= 0:
        return max(S - K, 0) if flag == 'call' else max(K - S, 0)
    d1 = (np.log(S / K) + (r + 0.5 * sigma**2) * T) / (sigma * np.sqrt(T))
    d2 = d1 - sigma * np.sqrt(T)
    if flag == 'call':
        return S * norm.cdf(d1) - K * np.exp(-r * T) * norm.cdf(d2)
    return K * np.exp(-r * T) * norm.cdf(-d2) - S * norm.cdf(-d1)

def bs_delta(S, K, T, sigma, r=0, flag='call'):
    d1 = (np.log(S / K) + (r + 0.5 * sigma**2) * T) / (sigma * np.sqrt(T))
    return norm.cdf(d1) if flag == 'call' else norm.cdf(d1) - 1

def implied_vol(market_price, S, K, T, r=0, flag='call', tol=1e-6):
    """Newton-Raphson implied vol solver."""
    sigma = 0.3
    for _ in range(100):
        price = bs_price(S, K, T, sigma, r, flag)
        vega  = S * norm.pdf((np.log(S/K) + (r+0.5*sigma**2)*T) / (sigma*np.sqrt(T))) * np.sqrt(T)
        if vega < 1e-10:
            break
        sigma -= (price - market_price) / vega
        if abs(price - market_price) < tol:
            break
    return max(sigma, 1e-6)
```

**In the Trader context**: If the platform gives you option order books, the actionable move is:
1. Compute implied vol from each quoted option.
2. Compare to your estimated realised vol from tick history.
3. If market IV > realised vol: **sell options** (delta-hedge with underlying).
4. If market IV < realised vol: **buy options** (delta-hedge with underlying).

#### Margrabe's Exchange Option (payoff = max(S1 − S2, 0))

```python
def margrabe(S1, S2, T, sigma1, sigma2, rho):
    """
    Price of exchange option: right to receive S1 and deliver S2.
    sigma = sqrt(sigma1^2 + sigma2^2 - 2*rho*sigma1*sigma2)
    No strike, no interest rate needed.
    """
    sigma = np.sqrt(sigma1**2 + sigma2**2 - 2*rho*sigma1*sigma2)
    if sigma < 1e-10 or T <= 0:
        return max(S1 - S2, 0)
    d1 = (np.log(S1 / S2) + 0.5 * sigma**2 * T) / (sigma * np.sqrt(T))
    d2 = d1 - sigma * np.sqrt(T)
    return S1 * norm.cdf(d1) - S2 * norm.cdf(d2)

def margrabe_delta(S1, S2, T, sigma1, sigma2, rho):
    """Returns (delta_S1, delta_S2) hedge ratios."""
    sigma = np.sqrt(sigma1**2 + sigma2**2 - 2*rho*sigma1*sigma2)
    d1 = (np.log(S1 / S2) + 0.5 * sigma**2 * T) / (sigma * np.sqrt(T))
    d2 = d1 - sigma * np.sqrt(T)
    return norm.cdf(d1), -norm.cdf(d2)
```

**When to use**: Any round where you can trade two correlated products AND an instrument whose payoff depends on their spread. Example: long ACO call, short IPR call, delta-hedged with spot positions.

#### Spread Option (Kirk's Approximation — fast, closed-form)

For payoff `max(S1 − S2 − K, 0)` Kirk (1995) gives:

```python
def kirk_spread(S1, S2, K, T, sigma1, sigma2, rho, r=0):
    """
    Kirk's approximation for spread option.
    Accurate when K is small relative to S1, S2.
    """
    F1 = S1 * np.exp(r * T)
    F2 = S2 * np.exp(r * T)
    F  = F1 / (F2 + K)
    sigma = np.sqrt(
        sigma1**2
        + (sigma2 * F2 / (F2 + K))**2
        - 2 * rho * sigma1 * sigma2 * F2 / (F2 + K)
    )
    d1 = (np.log(F) + 0.5 * sigma**2 * T) / (sigma * np.sqrt(T))
    d2 = d1 - sigma * np.sqrt(T)
    return np.exp(-r * T) * ((F2 + K) * (F * norm.cdf(d1) - norm.cdf(d2)))
```

---

### 15.4 Mean-Reversion: Ornstein-Uhlenbeck

Use instead of GBM when the product shows strong mean-reversion (ACF lag-1 < -0.3, as ACO does).

```
dS = θ(μ - S)dt + σ dW
```

Parameters: `θ` (speed of reversion), `μ` (long-run mean), `σ` (vol).

```python
def calibrate_ou(prices: np.ndarray, dt: float = 1/10000):
    """
    Fit OU parameters via OLS on: dS_t = (a + b·S_t)dt + σ·dW_t
    where a = θμ, b = -θ.
    """
    S    = prices[:-1]
    dS   = np.diff(prices)
    X    = np.column_stack([np.ones_like(S), S])
    beta = np.linalg.lstsq(X, dS / dt, rcond=None)[0]
    a, b = beta
    theta = -b                    # mean-reversion speed (per tick)
    mu    = a / theta             # long-run mean
    resid = dS / dt - a - b * S
    sigma = resid.std() * np.sqrt(dt)
    half_life = np.log(2) / theta # ticks to revert half-way
    return {"theta": theta, "mu": mu, "sigma": sigma, "half_life": half_life}

def ou_fair_value(S_current, mu, theta, horizon_ticks):
    """Expected price at horizon under OU."""
    return mu + (S_current - mu) * np.exp(-theta * horizon_ticks)

def ou_trading_signal(S_current, mu, sigma, theta, z_thresh=1.5):
    """
    Z-score of current price vs OU mean.
    |z| > z_thresh → fade the move.
    """
    ou_std = sigma / np.sqrt(2 * theta)  # stationary std dev
    z = (S_current - mu) / ou_std
    if z > z_thresh:
        return "SELL", z
    elif z < -z_thresh:
        return "BUY", z
    return "FLAT", z
```

**For ACO specifically**: `ou_fair_value()` gives a better fair-value estimate than static mid+OBI when the position is extreme and you want to know when to expect mean-reversion. Use it to time Phase 2/3 inventory reduction.

---

### 15.5 Futures Curve Analysis

If the round introduces futures contracts expiring at different dates:

```python
def futures_curve_signals(spot: float, futures: dict) -> dict:
    """
    futures: {maturity_ticks: price}
    Returns: implied forward rates, contango/backwardation, roll yield.
    """
    maturities = sorted(futures)
    signals = {}

    for T in maturities:
        F = futures[T]
        # Cost-of-carry: F = S * exp((r - delta) * T)
        # => implied (r - delta) = log(F/S) / T
        implied_carry = np.log(F / spot) / (T / 10000)  # annualised
        contango = F > spot  # True = contango, False = backwardation
        signals[T] = {
            "implied_carry": implied_carry,
            "contango": contango,
            "basis": F - spot,          # futures - spot
            "roll_yield": -implied_carry # positive in backwardation
        }

    # Detect calendar spread opportunities
    for i in range(len(maturities) - 1):
        T1, T2 = maturities[i], maturities[i+1]
        F1, F2 = futures[T1], futures[T2]
        # Fair calendar spread = F2 - F1 (carry cost for period T1→T2)
        fair_spread = spot * (np.exp(implied_carry * (T2-T1)/10000) - 1)
        actual_spread = F2 - F1
        signals[f"cal_{T1}_{T2}"] = actual_spread - fair_spread  # misprice

    return signals
```

**Trading rule**: If `actual_spread > fair_spread` → sell the far contract, buy the near → calendar spread trade.

---

### 15.6 Delta-Hedging in the Trader Context

When trading options or spread products, you must hedge the directional exposure to stay market-neutral (or you're just taking a directional bet):

```python
class DeltaHedger:
    """
    Maintains delta-neutral inventory across correlated products.
    Tracks net delta per underlying and places offsetting spot orders.
    """

    def __init__(self, limits: dict):
        self.limits = limits
        self.option_deltas: dict = {}  # sym -> total delta exposure

    def register_option_fill(self, option_sym, underlying_sym, qty, delta_per_unit):
        """Call when an option fills. qty positive=long, negative=short."""
        exposure = qty * delta_per_unit
        self.option_deltas[underlying_sym] = (
            self.option_deltas.get(underlying_sym, 0) + exposure
        )

    def hedge_orders(self, state, underlyings: list) -> dict:
        """
        Returns spot orders needed to zero net delta.
        Net delta = option_delta + spot_position.
        Target: net_delta = 0.
        """
        orders = {}
        for sym in underlyings:
            pos = state.position.get(sym, 0)
            opt_delta = self.option_deltas.get(sym, 0)
            net_delta = pos + opt_delta          # total exposure to underlying
            hedge_qty = -int(round(net_delta))   # quantity to trade spot
            limit = self.limits.get(sym, 50)
            hedge_qty = max(min(hedge_qty, limit - pos), -(limit + pos))
            if hedge_qty != 0 and sym in state.order_depths:
                od = state.order_depths[sym]
                if hedge_qty > 0 and od.sell_orders:
                    best_ask = min(od.sell_orders)
                    orders[sym] = [Order(sym, best_ask, hedge_qty)]
                elif hedge_qty < 0 and od.buy_orders:
                    best_bid = max(od.buy_orders)
                    orders[sym] = [Order(sym, best_bid, hedge_qty)]
        return orders
```

---

### 15.7 Correlation Signal for Pair-Trading

If two products are correlated but one moves and the other hasn't yet:

```python
class PairTrader:
    """
    Cointegration-based pair trading for two correlated products.
    Trades the spread z = price_A - beta * price_B when it deviates.
    """

    def __init__(self, beta: float, mu_z: float, sigma_z: float,
                 entry_z: float = 2.0, exit_z: float = 0.5):
        self.beta    = beta      # hedge ratio (from OLS or Kalman)
        self.mu_z    = mu_z      # mean of spread
        self.sigma_z = sigma_z   # std of spread
        self.entry_z = entry_z   # z-score to enter trade
        self.exit_z  = exit_z    # z-score to exit

    def signal(self, price_A: float, price_B: float) -> str:
        spread = price_A - self.beta * price_B
        z = (spread - self.mu_z) / self.sigma_z
        if z > self.entry_z:
            return "SHORT_SPREAD"  # sell A, buy B
        elif z < -self.entry_z:
            return "LONG_SPREAD"   # buy A, sell B
        elif abs(z) < self.exit_z:
            return "EXIT"
        return "HOLD"

    @staticmethod
    def calibrate(prices_A: np.ndarray, prices_B: np.ndarray):
        """OLS hedge ratio + spread statistics."""
        beta = np.cov(prices_A, prices_B)[0, 1] / np.var(prices_B)
        spread = prices_A - beta * prices_B
        return {"beta": beta, "mu_z": spread.mean(), "sigma_z": spread.std()}
```

**Kalman Filter version** (adaptive beta — better when the relationship drifts):

```python
def kalman_hedge_ratio(prices_A, prices_B, Q=1e-5, R=1e-3):
    """
    Estimates time-varying beta via 1D Kalman filter.
    Returns beta_history array (same length as prices_A).
    """
    n = len(prices_A)
    beta = np.zeros(n)
    P    = 1.0  # initial variance
    beta[0] = prices_A[0] / prices_B[0]  # naive init

    for t in range(1, n):
        # Predict
        P_pred = P + Q
        # Update
        H   = prices_B[t]
        K   = P_pred * H / (H**2 * P_pred + R)  # Kalman gain
        innovation = prices_A[t] - H * beta[t-1]
        beta[t] = beta[t-1] + K * innovation
        P = (1 - K * H) * P_pred

    return beta
```

---

### 15.8 Volatility Regime Detection

Market vol can switch between regimes. Detecting the regime changes your quoting strategy:

```python
class VolRegimeDetector:
    """
    Two-state rolling vol detector.
    High vol → widen quotes / reduce size.
    Low vol  → tighten quotes / increase size.
    """

    def __init__(self, window: int = 200, high_vol_mult: float = 1.5):
        self.window       = window
        self.high_mult    = high_vol_mult
        self.ret_buffer   = []

    def update(self, new_mid: float, prev_mid: float):
        if prev_mid > 0:
            self.ret_buffer.append(new_mid / prev_mid - 1)
        if len(self.ret_buffer) > self.window:
            self.ret_buffer.pop(0)

    def current_vol(self) -> float:
        if len(self.ret_buffer) < 10:
            return 0.0
        return float(np.std(self.ret_buffer))

    def regime(self, long_window: int = 1000) -> str:
        short = float(np.std(self.ret_buffer[-self.window:]))
        # Compare to long-run vol stored in traderData
        # Simplified: compare to last known baseline
        return "HIGH" if short > self.current_vol() * self.high_mult else "LOW"

def aco_quote_by_regime(vol_regime: str, base_k: int = 20) -> tuple:
    """Adjust ACO quoting width by vol regime."""
    if vol_regime == "HIGH":
        return base_k + 4, 0.5   # wider spread, smaller size fraction
    return base_k, 1.0           # normal spread, full size
```

---

### 15.9 Implementation Architecture for a Complex Round

When a round has 3+ products with dependencies, structure the Trader like this:

```python
class Trader:
    LIMITS = {
        "SPOT_A": 80, "SPOT_B": 80,
        "OPTION_AB": 40,    # options usually have smaller limits
        "FUTURES_A": 60,
    }

    def __init__(self):
        # State that persists via traderData JSON
        self._vol_window_A = []
        self._vol_window_B = []
        self._ou_params    = None   # calibrated on first N ticks
        self._corr         = None

    def run(self, state):
        ts = self._load(state.traderData)
        result = {}

        # Step 1: Update vol estimators with new ticks
        self._update_vol(state, ts)

        # Step 2: Compute cross-asset signals
        signals = self._compute_signals(state, ts)

        # Step 3: Route to product handlers
        for sym in state.order_depths:
            if sym == "SPOT_A":
                result[sym] = self._market_make(state, sym, signals)
            elif sym == "SPOT_B":
                result[sym] = self._directional(state, sym, signals)
            elif sym.startswith("OPTION"):
                result[sym] = self._option_trade(state, sym, signals)
            elif sym.startswith("FUTURES"):
                result[sym] = self._futures_arb(state, sym, signals)
            else:
                result[sym] = []

        return result, 0, json.dumps(ts)
```

---

### 15.10 Risk Limits and Greeks Management

When trading derivatives, always track your Greeks to avoid blowing up:

```python
class GreeksTracker:
    """
    Aggregates option Greeks across all positions.
    Used to enforce risk limits before submitting orders.
    """

    def __init__(self):
        self.positions = {}  # sym -> (qty, S, K, T, sigma, flag)

    def net_delta(self, underlying: str) -> float:
        total = 0.0
        for sym, (qty, S, K, T, sigma, flag) in self.positions.items():
            if underlying in sym:
                total += qty * bs_delta(S, K, T, sigma, flag=flag)
        return total

    def net_gamma(self, underlying: str) -> float:
        from scipy.stats import norm
        total = 0.0
        for sym, (qty, S, K, T, sigma, flag) in self.positions.items():
            if underlying in sym and T > 0:
                d1 = (np.log(S/K) + 0.5*sigma**2*T) / (sigma*np.sqrt(T))
                gamma = norm.pdf(d1) / (S * sigma * np.sqrt(T))
                total += qty * gamma
        return total

    def delta_breach(self, underlying: str, spot_limit: int) -> bool:
        """True if net delta exceeds spot equivalent position limit."""
        return abs(self.net_delta(underlying)) > spot_limit * 0.8
```

---

### 15.11 Calibration Checklist for a New Round

When new products appear, run this analysis immediately before building any strategy:

```python
def full_calibration(prices_csv_path: str, product: str) -> dict:
    """
    Run on Day -1 data. Produces all parameters needed for strategy selection.
    """
    import csv
    rows = list(csv.DictReader(open(prices_csv_path), delimiter=";"))
    rows = [r for r in rows if r["product"] == product]
    mids = np.array([float(r["mid_price"]) for r in rows if r["mid_price"]])

    # 1. Drift and vol
    log_rets = np.diff(np.log(mids))
    mu    = log_rets.mean() / (1/10000)
    sigma = log_rets.std()  / np.sqrt(1/10000)

    # 2. Mean-reversion test
    acf1  = np.corrcoef(log_rets[:-1], log_rets[1:])[0,1]

    # 3. OU calibration
    ou = calibrate_ou(mids)

    # 4. OBI slope
    # (compute separately from order book columns)

    # 5. Spread regime
    bbs = [int(r["bid_price_1"]) for r in rows if r["bid_price_1"]]
    bas = [int(r["ask_price_1"]) for r in rows if r["ask_price_1"]]
    spreads = [ba - bb for bb, ba in zip(bbs, bas) if bb and ba]
    spread_dist = {
        "mean":   np.mean(spreads),
        "normal": sum(1 for s in spreads if s == round(np.mean(spreads))) / len(spreads),
    }

    # 6. Strategy recommendation
    if acf1 < -0.3:
        strategy = "MARKET_MAKE"
    elif mu > 0.05 * sigma:  # drift > 5% of vol → trending
        strategy = "BUY_HOLD" if mu > 0 else "SELL_HOLD"
    else:
        strategy = "FLAT_MM"

    return {
        "product": product,
        "mu_per_tick": mu,
        "sigma_per_tick": sigma,
        "acf_lag1": acf1,
        "ou_params": ou,
        "spread_mean": spread_dist["mean"],
        "recommended_strategy": strategy,
    }
```

---

### 15.12 Strategy Selection Matrix

| ACF lag-1 | Drift (μ/σ) | Spread | Recommended Approach |
|-----------|-------------|--------|----------------------|
| < -0.3 | ≈ 0 | wide (>10) | Market-making (ACO template) |
| < -0.3 | ≈ 0 | narrow (<5) | Aggressive takes + OU z-score |
| > +0.1 | high | any | Buy/sell and hold (IPR template) |
| ≈ 0 | ≈ 0 | any | Flat MM, wider quotes |
| (options present) | — | — | IV vs RV arb + delta hedge |
| (futures curve) | — | — | Basis trade + calendar spread |
| (two correlated) | any | any | Pair trade via Kalman beta |

---

### 15.13 Own Ideas and Extensions

#### 1. Adaptive OBI Slope via Online Linear Regression

Rather than fixing `ACO_OBI_SLOPE=5.0`, estimate it in real time using an exponential weighted OLS:

```python
class AdaptiveOBISlope:
    def __init__(self, decay=0.995):
        self.decay = decay
        self.sum_xy = 0.0
        self.sum_xx = 0.0
        self.slope  = 5.0  # fallback

    def update(self, obi: float, next_tick_move: float):
        self.sum_xy = self.decay * self.sum_xy + obi * next_tick_move
        self.sum_xx = self.decay * self.sum_xx + obi * obi
        if self.sum_xx > 1e-10:
            self.slope = self.sum_xy / self.sum_xx

    def fair(self, mid: float, obi: float) -> float:
        return mid + self.slope * obi
```

Store `sum_xy` and `sum_xx` in `traderData` so they persist across ticks.

#### 2. OBI-Conditioned Inventory Thresholds

Current thresholds (Phase 2 at ±25, Phase 3 at ±40) are static. A smarter version lowers the trigger when OBI opposes your position — reducing before the market moves against you:

```python
def adaptive_soft_thresh(pos: int, obi: float, base=25, obi_thresh=0.4) -> int:
    """
    If long AND OBI strongly negative (price falling), reduce earlier.
    If short AND OBI strongly positive (price rising), cover earlier.
    """
    if (pos > 0 and obi < -obi_thresh) or (pos < 0 and obi > obi_thresh):
        return base // 2   # 12 instead of 25
    return base
```

#### 3. Tick-Count Horizon for Option Expiry

IMC rounds last a fixed number of ticks. If options are introduced, you know exactly how many ticks remain:

```python
TICKS_PER_DAY = 10_000
TOTAL_DAYS    = 3

def ticks_remaining(current_ts: int, day: int) -> int:
    ticks_done = (day + 1) * TICKS_PER_DAY + current_ts // 100
    return TOTAL_DAYS * TICKS_PER_DAY - ticks_done

def T_years(ticks: int, ticks_per_year: float = 252 * 10_000) -> float:
    return ticks / ticks_per_year
```

#### 4. Skew-Adjusted Quoting

If the underlying shows positive skew in returns (fat right tail), sell OTM calls at a premium and buy OTM puts at a discount relative to Black-Scholes. Detect skew from historical return distribution:

```python
from scipy.stats import skew

def vol_skew_adjustment(returns: np.ndarray, moneyness: float) -> float:
    """
    Positive skewness → upside surprises likely → sell OTM calls cheaper premium.
    Negative skewness → downside surprises likely → demand put premium.
    """
    s = skew(returns)
    # Rough skew adjustment to IV: IV(K) ≈ IV_atm - skew_coef * log(K/S)
    skew_coef = -s * 0.1   # empirical scaling
    return skew_coef * np.log(moneyness)  # add to ATM IV

def adjusted_bs_price(S, K, T, sigma_atm, returns, flag='call'):
    moneyness = K / S
    adj = vol_skew_adjustment(returns, moneyness)
    return bs_price(S, K, T, sigma_atm + adj, flag=flag)
```

#### 5. Cross-Product Lead-Lag Signal

If one product moves before another (common with spot/futures or correlated commodities), detect the lag and trade it:

```python
def cross_correlation_lags(mid_A, mid_B, max_lag=10):
    """
    Returns the lag (in ticks) at which B leads A most strongly.
    Positive lag = B leads A (use B's current move to predict A's next move).
    """
    rets_A = np.diff(mid_A)
    rets_B = np.diff(mid_B)
    corrs = [np.corrcoef(rets_B[:-lag], rets_A[lag:])[0,1] for lag in range(1, max_lag+1)]
    best_lag = int(np.argmax(np.abs(corrs))) + 1
    return best_lag, corrs[best_lag - 1]

# In the trader: if B moved significantly in last 3 ticks, predict A follows
def lead_lag_signal(price_history_A, price_history_B, lag=3, z_thresh=1.5):
    B_move = price_history_B[-1] - price_history_B[-1 - lag]
    B_vol  = np.std(np.diff(price_history_B[-100:]))
    z = B_move / (B_vol * np.sqrt(lag)) if B_vol > 0 else 0
    if z > z_thresh:
        return "BUY_A"   # B went up, A will follow
    elif z < -z_thresh:
        return "SELL_A"
    return "FLAT"
```

#### 6. Storing Calibrated Parameters in traderData

Since `traderData` is the only persistence mechanism, use it to store running calibrations:

```python
def _load(self, td: str) -> dict:
    if td and td.strip():
        try:
            return json.loads(td)
        except:
            pass
    return {
        "ou_theta": None, "ou_mu": None, "ou_sigma": None,
        "obi_sum_xy": 0.0, "obi_sum_xx": 0.0,
        "price_history_A": [], "price_history_B": [],
        "tick_count": 0,
    }

# In run(): update ts dict, return json.dumps(ts) as 3rd element
```

**Warning**: `traderData` is a string. Keep it compact. Store only the running sufficient statistics (sums, not full history arrays) to avoid JSON overhead at 10,000 ticks/day.

---

## 16. IMC Prosperity 3 — Real Strategies for Correlated & Derivative Products

> Source: Frankfurt Hedgehogs team (`TimoDiehm/imc-prosperity-3`).
> These are production strategies that were used live. Adapt them directly for any future round with similar instruments.

---

### 16.1 Round 2 — ETF Basket Arbitrage (Picnic Baskets)

#### Product Structure

| Basket | Constituents | Weights |
|--------|-------------|---------|
| PICNIC_BASKET1 | Croissants, Jams, Djembes | 6, 3, 1 |
| PICNIC_BASKET2 | Croissants, Jams | 4, 2 |

#### Core Alpha: Mean-Reversion of Basket vs Synthetic Value

The basket price mean-reverts toward its synthetic (constituent-weighted) value. The constituents themselves do NOT mean-revert toward the basket — one-sided causality.

```python
ETF_CONSTITUENT_FACTORS = [[6, 3, 1], [4, 2, 0]]

def synthetic_value(croissant_mid, jam_mid, djembe_mid, basket_idx):
    factors = ETF_CONSTITUENT_FACTORS[basket_idx]
    return (factors[0] * croissant_mid
          + factors[1] * jam_mid
          + factors[2] * djembe_mid)

def raw_spread(basket_mid, synthetic):
    return basket_mid - synthetic   # positive = basket rich, short it
```

#### Running Premium Tracking

Baskets carry a small persistent premium over synthetic value. Remove it via online mean to avoid stale bias:

```python
class RunningPremium:
    """Tracks mean premium via Welford online algorithm (no buffer needed)."""
    INITIAL = [5.0, 53.0]   # empirical priors for basket 1 and 2

    def __init__(self, basket_idx):
        self.n = 1
        self.mean = self.INITIAL[basket_idx]

    def update(self, raw_spread: float):
        self.n += 1
        self.mean += (raw_spread - self.mean) / self.n

    def adjusted_spread(self, raw_spread: float) -> float:
        return raw_spread - self.mean
```

Store `n` and `mean` in `traderData` so they persist across ticks.

#### Trading Thresholds and Execution

```python
BASKET_THRESHOLDS = [80, 50]       # basket 1, basket 2
ETF_HEDGE_FACTOR  = 0.5            # hedge 50% of basket exposure with constituents

def basket_signal(adj_spread: float, threshold: float,
                  informed_adj: float = 0.0) -> str:
    """
    adj_spread = basket_mid - synthetic - running_premium
    informed_adj: shift thresholds when informed trader detected
    """
    if adj_spread > threshold + informed_adj:
        return "SHORT"   # sell basket, buy constituents
    elif adj_spread < -(threshold + informed_adj):
        return "LONG"    # buy basket, sell constituents
    elif abs(adj_spread) < 5:
        return "EXIT"
    return "HOLD"
```

#### Constituent Hedging

When shorting 1 unit of PICNIC_BASKET1, simultaneously buy partial constituent exposure:

```python
def constituent_hedge_orders(basket_qty: int, basket_idx: int,
                             constituent_positions: dict,
                             constituent_limits: dict) -> dict:
    """
    basket_qty: signed (negative = short basket)
    Returns target constituent positions for 50% hedge.
    """
    factors = ETF_CONSTITUENT_FACTORS[basket_idx]
    syms = ["CROISSANTS", "JAMS", "DJEMBES"]
    orders = {}
    for sym, factor in zip(syms, factors):
        hedge = -int(basket_qty * factor * ETF_HEDGE_FACTOR)
        current = constituent_positions.get(sym, 0)
        delta = hedge - current
        if delta != 0:
            orders[sym] = delta
    return orders
```

#### Informed Trader Detection (Olivia)

Olivia is a predictable bot whose orders reliably signal direction. In Prosperity 3 she traded Croissants aggressively. Detection in Round 2 (without trader IDs visible):

```python
class OliviaTracker:
    """
    Track running min/max of Croissant mid to infer Olivia's direction.
    Olivia buys when price is at recent minimum, sells at recent maximum.
    In Round 5, replace with direct trader ID check.
    """
    def __init__(self, window=50):
        self.window = window
        self.prices = []

    def update(self, mid: float):
        self.prices.append(mid)
        if len(self.prices) > self.window:
            self.prices.pop(0)

    def signal(self, mid: float) -> str:
        if not self.prices:
            return "NEUTRAL"
        if mid <= min(self.prices):
            return "OLIVIA_BUYING"    # she's buying → price will rise
        if mid >= max(self.prices):
            return "OLIVIA_SELLING"   # she's selling → price will fall
        return "NEUTRAL"

    def threshold_adjustment(self, signal: str, base_thr: float) -> float:
        """
        When Olivia is buying: tighten short entry (harder to short),
        loosen long entry (easier to go long with her).
        """
        adj = 10.0   # additional ticks
        if signal == "OLIVIA_BUYING":
            return adj    # shift both thresholds by +adj (helps long side)
        elif signal == "OLIVIA_SELLING":
            return -adj   # shift both thresholds by -adj (helps short side)
        return 0.0
```

In Round 5 when trader IDs are public, replace `signal()` with:
```python
def signal_from_id(buyer: str, seller: str) -> str:
    if buyer == "Olivia":   return "OLIVIA_BUYING"
    if seller == "Olivia":  return "OLIVIA_SELLING"
    return "NEUTRAL"
```

#### Performance (Prosperity 3 live results)

- Basket arbitrage: **40,000–60,000 SeaShells/round**
- Direct Croissants (with Olivia signal): **~20,000 SeaShells/round**
- Parameters chosen from stable plateau region of grid search, not peak

---

### 16.2 Round 3 — Volcanic Rock Options (IV Scalping)

#### Instrument Details

```python
OPTION_SYMBOLS = [
    'VOLCANIC_ROCK_VOUCHER_9500',
    'VOLCANIC_ROCK_VOUCHER_9750',
    'VOLCANIC_ROCK_VOUCHER_10000',   # ATM; primary focus
    'VOLCANIC_ROCK_VOUCHER_10250',
    'VOLCANIC_ROCK_VOUCHER_10500',
]
# All are European call options on VOLCANIC_ROCK (spot ~10,000)
# Position limit: 200 contracts each
# TTE: starts at 7/365, decreases to 2/365 by Round 5
DAY = 5   # update per round: 3=Round3, 4=Round4, 5=Round5
```

#### Time-to-Expiry Calculation

```python
def tte(state_timestamp: int, day: int = DAY,
        days_per_year: int = 365) -> float:
    """
    Fraction of a year remaining to expiry.
    Competition runs 7 days total; each round is one day.
    """
    days_elapsed = days_per_year - 8 + day + state_timestamp // 100 / 10_000
    return max((7 - days_elapsed + (days_per_year - 7)) / days_per_year, 1e-6)
    # Simplified equivalent used in code:
    # return 1 - days_elapsed / days_per_year
```

#### Black-Scholes Implementation

```python
from statistics import NormalDist
from math import log, exp, sqrt

_N = NormalDist()   # standard normal; use .cdf() and .pdf()

def bs_call(S: float, K: float, TTE: float, sigma: float, r: float = 0):
    """Returns (call_price, delta)."""
    if TTE <= 0:
        return max(S - K, 0.0), float(S > K)
    d1 = (log(S / K) + (r + 0.5 * sigma**2) * TTE) / (sigma * sqrt(TTE))
    d2 = d1 - sigma * sqrt(TTE)
    price = S * _N.cdf(d1) - K * exp(-r * TTE) * _N.cdf(d2)
    delta = _N.cdf(d1)
    return price, delta

def bs_vega(S: float, K: float, TTE: float, sigma: float, r: float = 0) -> float:
    """Sensitivity of call price to sigma (used for IV Newton-Raphson)."""
    d1 = (log(S / K) + (r + 0.5 * sigma**2) * TTE) / (sigma * sqrt(TTE))
    return S * _N.pdf(d1) * sqrt(TTE)

def implied_vol_nr(market_price: float, S: float, K: float, TTE: float,
                   r: float = 0, tol: float = 1e-6, max_iter: int = 100) -> float:
    """Newton-Raphson implied vol from observed call price."""
    sigma = 0.20   # initial guess
    for _ in range(max_iter):
        price, _ = bs_call(S, K, TTE, sigma, r)
        v = bs_vega(S, K, TTE, sigma, r)
        if v < 1e-10:
            break
        diff = price - market_price
        if abs(diff) < tol:
            break
        sigma -= diff / v
        sigma = max(sigma, 1e-6)   # keep positive
    return sigma
```

#### Volatility Smile Calibration

The volatility smile is fit as a **quadratic polynomial** in standardised moneyness `m = log(K/S) / sqrt(TTE)`:

```python
# Calibrated once from historical data (Round 3 specific):
SMILE_COEFFS = [0.27362531, 0.01007566, 0.14876677]
# iv(m) = 0.274 + 0.010*m + 0.149*m^2
# (roughly: ATM vol ~27.4%, positive curvature → standard vol smile)

def smile_iv(S: float, K: float, TTE: float,
             coeffs: list = SMILE_COEFFS) -> float:
    """
    Fair implied vol from the calibrated smile polynomial.
    Use this as the 'theoretical' IV to compare against observed IV.
    """
    m = log(K / S) / sqrt(TTE)
    return coeffs[0] + coeffs[1] * m + coeffs[2] * m**2

def smile_fair_price(S: float, K: float, TTE: float) -> tuple:
    """Returns (fair_price, fair_delta) using smile-calibrated IV."""
    iv = smile_iv(S, K, TTE)
    return bs_call(S, K, TTE, iv)
```

#### IV Scalping Strategy (Primary Alpha)

Principle: The **observed IV minus smile IV** mean-reverts strongly (confirmed by ACF analysis). Trade when the deviation is large.

```python
IV_SCALPING_THR      = 0.7    # fraction of window where deviation is same sign
IV_SCALPING_WINDOW   = 100    # ticks to compute switch_mean
THR_OPEN             = 0.5    # price deviation (SeaShells) to open
THR_CLOSE            = 0.0    # close when deviation → 0
LOW_VEGA_THR_ADJ     = 0.5    # extra buffer when vega is small

class IVScalper:
    def __init__(self):
        self.deviation_history = []
        self.position = 0

    def update(self, market_price: float, S: float, K: float, TTE: float):
        fair, _ = smile_fair_price(S, K, TTE)
        dev = market_price - fair
        self.deviation_history.append(dev)
        if len(self.deviation_history) > IV_SCALPING_WINDOW:
            self.deviation_history.pop(0)
        return dev

    def signal(self, dev: float, vega: float) -> str:
        if len(self.deviation_history) < IV_SCALPING_WINDOW:
            return "WAIT"

        # Fraction of recent window with same-sign deviation
        switch_mean = sum(
            1 for d in self.deviation_history if d * dev > 0
        ) / IV_SCALPING_WINDOW

        thr_adj = LOW_VEGA_THR_ADJ if vega <= 1.0 else 0.0
        thr_open = THR_OPEN + thr_adj

        if switch_mean < IV_SCALPING_THR:
            return "HOLD"   # deviation not persistent enough

        if dev > thr_open and self.position <= 0:
            return "BUY"    # option cheap vs smile fair → buy
        if dev < -thr_open and self.position >= 0:
            return "SELL"   # option rich vs smile fair → sell
        if self.position > 0 and dev < THR_CLOSE:
            return "CLOSE_LONG"
        if self.position < 0 and dev > -THR_CLOSE:
            return "CLOSE_SHORT"
        return "HOLD"
```

#### Delta Hedging (Gamma Scalping)

After taking an option position, delta-hedge the underlying to be directionally neutral. Rehedge every tick:

```python
def delta_hedge_order(option_qty: int, option_delta: float,
                      spot_position: int, spot_limit: int,
                      spot_od) -> list:
    """
    Net delta = option_qty * option_delta + spot_position
    Target: net delta = 0
    Returns list of Order for VOLCANIC_ROCK spot.
    """
    net_delta  = option_qty * option_delta + spot_position
    hedge_qty  = -int(round(net_delta))
    max_buy    = spot_limit - spot_position
    max_sell   = spot_limit + spot_position
    hedge_qty  = max(min(hedge_qty, max_buy), -max_sell)

    if hedge_qty == 0:
        return []
    if hedge_qty > 0 and spot_od.sell_orders:
        best_ask = min(spot_od.sell_orders)
        return [Order("VOLCANIC_ROCK", best_ask, hedge_qty)]
    if hedge_qty < 0 and spot_od.buy_orders:
        best_bid = max(spot_od.buy_orders)
        return [Order("VOLCANIC_ROCK", best_bid, hedge_qty)]
    return []
```

Gamma scalping (buying options + rehedging) gives positive expected value but modest absolute returns. More valuable as a risk-offset when IV scalping creates large option positions.

#### Mean Reversion on Underlying

EMA-based, **no volatility normalisation** (too little data for robust vol estimates in a 3-day window):

```python
UNDERLYING_MR_THR    = 15    # SeaShells
UNDERLYING_MR_WINDOW = 10    # ticks for EMA
OPTIONS_MR_WINDOW    = 30
THEO_NORM_WINDOW     = 20

class EMAMeanReversion:
    def __init__(self, window: int = 10, threshold: float = 15.0):
        self.ema = None
        self.alpha = 2 / (window + 1)
        self.thr = threshold

    def update(self, price: float):
        if self.ema is None:
            self.ema = price
        else:
            self.ema = self.alpha * price + (1 - self.alpha) * self.ema

    def signal(self, price: float) -> str:
        if self.ema is None:
            return "FLAT"
        dev = price - self.ema
        if dev > self.thr:
            return "SELL"   # price above EMA → expect reversion down
        if dev < -self.thr:
            return "BUY"    # price below EMA → expect reversion up
        return "FLAT"
```

#### Full Hybrid Architecture (Round 3)

```python
class VolcanicRockStrategy:
    """
    Three concurrent alpha sources:
      1. IV scalping across all 5 strikes
      2. Gamma scalping (delta hedge of option book)
      3. EMA mean reversion on spot
    """
    def __init__(self):
        self.iv_scalpers = {sym: IVScalper() for sym in OPTION_SYMBOLS}
        self.spot_mr     = EMAMeanReversion(window=10, threshold=15.0)
        self.spot_pos    = 0
        self.option_pos  = {sym: 0 for sym in OPTION_SYMBOLS}

    def run(self, state, day: int) -> dict:
        orders = {}
        S    = mid_price(state.order_depths["VOLCANIC_ROCK"])
        T    = tte(state.timestamp, day)

        # Update spot mean reversion
        self.spot_mr.update(S)
        mr_signal = self.spot_mr.signal(S)

        # Process each option strike
        net_option_delta = 0.0
        for sym in OPTION_SYMBOLS:
            K = int(sym.split("_")[-1])
            od = state.order_depths.get(sym)
            if not od:
                continue
            mkt = mid_price(od)
            fair, delta = smile_fair_price(S, K, T)
            vega = bs_vega(S, K, T, smile_iv(S, K, T))

            dev  = self.iv_scalpers[sym].update(mkt, S, K, T)
            sig  = self.iv_scalpers[sym].signal(dev, vega)

            # Execute IV scalp signal
            opt_orders = []
            pos = self.option_pos[sym]
            lim = 200
            if sig == "BUY" and pos < lim:
                qty = min(10, lim - pos)
                opt_orders.append(Order(sym, int(mkt) + 1, qty))
            elif sig == "SELL" and pos > -lim:
                qty = min(10, lim + pos)
                opt_orders.append(Order(sym, int(mkt) - 1, -qty))
            elif sig in ("CLOSE_LONG", "CLOSE_SHORT"):
                opt_orders.append(Order(sym, int(mkt), -pos))

            if opt_orders:
                orders[sym] = opt_orders

            net_option_delta += pos * delta

        # Spot orders: delta hedge + mean reversion
        spot_orders = []
        # Delta hedge
        hedge = delta_hedge_order(
            option_qty=1,                    # already baked into net_option_delta
            option_delta=net_option_delta,
            spot_position=self.spot_pos,
            spot_limit=400,
            spot_od=state.order_depths["VOLCANIC_ROCK"]
        )
        spot_orders.extend(hedge)
        # Mean reversion overlay
        spot_od = state.order_depths["VOLCANIC_ROCK"]
        if mr_signal == "BUY" and spot_od.sell_orders:
            spot_orders.append(Order("VOLCANIC_ROCK", min(spot_od.sell_orders), 5))
        elif mr_signal == "SELL" and spot_od.buy_orders:
            spot_orders.append(Order("VOLCANIC_ROCK", max(spot_od.buy_orders), -5))

        if spot_orders:
            orders["VOLCANIC_ROCK"] = spot_orders

        return orders
```

#### Performance (Prosperity 3 live results)

| Alpha Source | PnL/Round |
|-------------|-----------|
| IV Scalping | 100,000–150,000 |
| Gamma Scalping | small positive (~5,000–10,000) |
| Mean Reversion (spot) | volatile: +100k / -50k / -10k across rounds |

**Risk note**: The team reduced MR exposure after a −50,000 round. Final round used half-position MR to protect the 190k lead. Lesson: mean reversion on the underlying is risky when volatility is high or regime shifts.

---

### 16.3 Round 4 — Conversion Arbitrage (Magnificent Macarons)

Not a traditional futures/options product, but uses the `conversions` mechanism of the platform.

#### The Exploit

A hidden market maker bot fills sell orders placed at exactly `int(external_bid + 0.5)` with ~60% probability, even when no visible market participant exists.

```python
def macaron_orders(state, external_bid: float, pos: int,
                   limit: int = 75, quote_size: int = 10) -> tuple:
    """
    Place sell order at int(external_bid + 0.5).
    Use conversions to cover the short if filled.
    Returns (orders_dict, conversions).
    """
    target_sell = int(external_bid + 0.5)
    sell_qty    = min(quote_size, limit + pos)
    orders = {}
    conversions = 0

    if sell_qty > 0:
        orders["MAGNIFICENT_MACARONS"] = [
            Order("MAGNIFICENT_MACARONS", target_sell, -sell_qty)
        ]

    # Convert back to cover position if short (up to 10/tick limit)
    if pos < 0:
        conversions = min(abs(pos), 10)   # buy via conversion market

    return orders, conversions
```

**Performance**: 80,000–100,000 SeaShells/round (theoretical max 130,000–160,000 with larger sizes).

**Lesson**: Always probe whether hidden bots fill at specific price formulas. Test `int(X)`, `round(X)`, `floor(X)`, `ceil(X)` variants empirically in early ticks.

---

### 16.4 Round 5 — Informed Trader Integration

When trader IDs are revealed in the platform's `market_trades`, use them directly:

```python
KNOWN_INFORMED_TRADERS = {"Olivia", "Caesar", "Raj"}   # names from Prosperity lore

def detect_informed_signal(market_trades: dict, product: str) -> str:
    """Check if known informed traders are active this tick."""
    trades = market_trades.get(product, [])
    for t in trades:
        if t.buyer in KNOWN_INFORMED_TRADERS:
            return f"{t.buyer}_BUYING"
        if t.seller in KNOWN_INFORMED_TRADERS:
            return f"{t.seller}_SELLING"
    return "NEUTRAL"

def apply_informed_threshold_adj(signal: str, base_thr: float) -> float:
    """Tighten entries against informed direction, loosen with it."""
    if "BUYING" in signal:
        return base_thr * 1.6   # harder to short, easier to long
    if "SELLING" in signal:
        return base_thr * 0.7   # easier to short
    return base_thr
```

---

### 16.5 Key Lessons from Prosperity 3 for Future Rounds

1. **Basket causality is asymmetric**: Baskets mean-revert to synthetic value, NOT the other way around. Trade only the basket, hedge partially with constituents.

2. **Always track running premium**: Baskets carry a persistent premium above their synthetic NAV. Subtract it before computing the trade signal or you will have a biased entry threshold.

3. **Volatility smile is a parabola in moneyness**: Fit `iv = a + b*m + c*m^2` where `m = log(K/S)/sqrt(TTE)`. This is the fair IV surface. Trade deviations from it.

4. **IV scalping is more stable than directional MR on options**: IV deviations from the smile mean-revert very reliably. Spot MR is noisier and carried more tail risk in Prosperity 3.

5. **Mean reversion thresholds: don't normalise by vol with 3 days of data**: Too few samples to estimate vol robustly. Use fixed tick thresholds calibrated from the first day.

6. **Parameter stability trumps peak performance**: The team explicitly chose thresholds from stable plateau regions of the grid search, not the maximum. This protects against overfitting to 3 days of history.

7. **Probe for hidden bots early**: The external bid exploit in Round 4 was non-obvious. In the first 100 ticks of any new round, test unusual price points to detect fill patterns.

8. **Scale hedge conservatively**: ETF hedge factor was 50%, not 100%. Full hedging increased transaction costs and slippage without meaningfully improving alpha.
