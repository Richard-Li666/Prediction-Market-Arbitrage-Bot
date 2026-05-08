
# Prediction market low latency arbitrage bot

*Lead–lag trading stack*

This repository is a **C++/Python** system for **low-latency lead–lag trading** between **Binance** spot BTC (WebSocket) and **Polymarket** short-horizon BTC “up/down” prediction markets. It streams both venues in parallel, estimates fair values from the spot path (with optional realized-vol / **GARCH**-style inputs), and compares them to Polymarket order-book quotes to spot mispricing—then either **paper-trades** with simulated fills (**`paper_trader --live`**) or, when built with live execution, submits **real CLOB orders** via **`poly_daemon.py`**. Utilities record feeds to **JSONL**, expose **terminal probes** for demos, and support offline tooling for analysis.

---

## 1. Clone, build, and deploy

Clone the repo, create the Python virtualenv (for **`poly_daemon.py`** and optional GARCH), then configure CMake and compile—release binaries are written to **`build/`** (e.g. **`build/paper_trader`**).

```bash
git clone <your-repo>
cd final-proj-26sp-cis3990-richard-li666

python3 -m venv .venv
source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -U pip py-clob-client-v2 arch numpy   # daemon + optional GARCH sigma
```

Configure and build (release binaries land in **`build/`**, e.g. **`build/live_trader`**):

```bash
cmake -S . -B build -DBUILD_LIVE_TRADER=ON    # ON = real CLOB orders in strategy mode; OFF = paper-only binary
cmake --build build --target live_trader paper_trader binance_logger polymarket_logger live_pipeline binance_ws_probe polymarket_quote_probe thread_pool_smoke
```

On Ubuntu you can use **`bash scripts/bootstrap_ubuntu.sh`** for venv + default **`cmake -S . -B build`** + `live_trader` / `paper_trader`. If you need live execution, run **`cmake -S . -B build -DBUILD_LIVE_TRADER=ON`** once, then build again.

Dependencies: **CMake ≥ 3.16**, **OpenSSL**, **libcurl**, compiler with **C++20** (`std::jthread` for **`ll::core::ThreadPool`**). Python **`poly_daemon.py`** is used for live orders and optional **`garch_forecast`** (needs **`arch`**).

---

## 2. Paper trader (simulated fills)

### 2a. Live simulation (`--live`)

Runs the BTC 5m bucket strategy against **live Binance + Polymarket WebSockets**; **no real Polymarket orders**.

**Quick try (from repo root, after build)** — **Binance.US** WebSocket for testing from the United States (`stream.binance.us`):

```bash
cd final-proj-26sp-cis3990-richard-li666
./build/paper_trader --live --host stream.binance.us --fixed-spend-usd 1 --initial-cash 100 --out-prefix paper_demo
```

Stop with `Ctrl+C`. Logs: `data/paper_demo_trades.jsonl`, `data/paper_demo_series.jsonl`.

Common knobs:

| Flag | Meaning |
|------|---------|
| `--initial-cash X` | Starting cash (USD) |
| `--risk-frac F` | Fraction of cash per entry BUY if `--fixed-spend-usd` not set (default `0.01`) |
| `--fixed-spend-usd X` | Fixed USDC per BUY (overrides `--risk-frac`; Polymarket min ~ $1) |
| `--entry X` | BUY when `theo - ask ≥ X` (default `0.15`) |
| `--close X` | SELL when `|theo - mid| ≤ X` (default `0.005`) |
| `--lat-ms N` | Simulated execution delay (default **1000** ms for paper; live binary uses **0** by default in §3) |
| `--fee-rate R` | Polymarket-style taker fee scale (default `0.072`) |
| `--sigma S` | Fallback annualized vol if estimation unavailable |
| `--sigma-step-ms N` | Resampling step for realized / GARCH input window |
| `--host` / `--port` / `--stream trade\|bookTicker` | Binance WebSocket (US: **`stream.binance.us`**) |
| `--parse-workers N` | Binance JSON parse thread pool (0 = inline) |
| `--poly-parse-workers N` | Polymarket parse thread pool |
| `--out-prefix NAME` | Writes `data/<NAME>_trades.jsonl` and `data/<NAME>_series.jsonl` (default `paper`) |
| `--poly-token TOKEN` | Single Polymarket asset id (disables bucket discovery) |
| `--disable-rollover-force-sell` | Skip automatic FORCE_SELL on rollover (see env `POLY_DISABLE_ROLLOVER_FORCE_SELL`) |

The process loads **`.env`** from the current directory when unset (same helper as live).

---

## 3. Live trader (real CLOB orders)

**Real-money / strategy execution is still under active development and is not stable.** Graders and reviewers should **not** run **`live_trader`** or configure wallet keys for grading—use **§2 paper simulation** and **§4 data logging** instead. Behavior on real Polymarket is demonstrated in the **submitted demo video**.

The codebase still builds **`live_trader`** when **`cmake -DBUILD_LIVE_TRADER=ON`** so **`LL_ENABLE_LIVE_TRADER`** is defined. Running it requires a **`.env`** with Polymarket / wallet fields for **`poly_daemon.py`**, **`POLY_DAEMON_CMD`** pointing at **`poly_daemon.py`**, and the same strategy flags as **`paper_trader --live`** (arguments after **`--strategy`**). Use **`--host stream.binance.us`** for Binance.US WebSockets when testing from the United States—details only for developers maintaining this path, not for ad‑hoc trials.

---

## 4. WebSocket: terminal preview vs JSONL logging

**Probes** print human-readable lines to the terminal (good for a quick live demo to a grader). **Loggers** append **JSONL** to disk for notebooks and offline work. Build includes **`binance_ws_probe`** and **`polymarket_quote_probe`** (see §1).

### 4a. Terminal preview (probes)

**Binance.US** — parsed book ticker or trades stream to **stdout** (Ctrl+C to stop):

```bash
cd final-proj-26sp-cis3990-richard-li666
./build/binance_ws_probe --host stream.binance.us --stream bookTicker
```

**Polymarket** — requires an outcome **`token_id`** (same asset id the CLOB WebSocket subscribes to). Obtain one from **`polymarket_logger`** startup logs, the Polymarket UI, or your recorded JSONL; then:

```bash
cd final-proj-26sp-cis3990-richard-li666
./build/polymarket_quote_probe <TOKEN_ID>
```

### 4b. Record live streams (JSONL)

**Binance.US** book ticker to a file:

```bash
cd final-proj-26sp-cis3990-richard-li666
./build/binance_logger data/bin_live.jsonl --host stream.binance.us --stream bookTicker
```

**Polymarket** — discovers current **`btc-updown-5m-*`** bucket and follows rollovers:

```bash
cd final-proj-26sp-cis3990-richard-li666
./build/polymarket_logger --discover data/poly_live.jsonl
```

**Recommended for offline analysis / notebooks** — record **Binance + Polymarket** together (same clocks in one process). Stop with `Ctrl+C` when you have enough history (full backtests need substantial runtime / disk):

```bash
cd final-proj-26sp-cis3990-richard-li666
./build/live_pipeline data/bin_live.jsonl data/poly_live.jsonl --host stream.binance.us
```

That writes **`data/bin_live.jsonl`** and **`data/poly_live.jsonl`**, which **`data/leag_lag.ipynb`** loads by default.

Alternatively use **`binance_logger`** + **`polymarket_logger`** separately into the same paths.

Extra CMake targets: **`replay_align`**, **`signal_smoke`**, **`backtest_latency_sweep`** (see **`CMakeLists.txt`**).

---

## WebSocket and thread pool

### WebSocket

This project uses **IXWebSocket** (`ix::WebSocket`) for two live feeds. **Binance** connects to `wss://{host}:{port}{stream_path}`. Defaults in `StreamClientConfig` are **`stream.binance.com`**, port **`9443`**, path **`/ws/btcusdt@bookTicker`**. For **Binance.US**, pass **`--host stream.binance.us`** or set **`LL_BINANCE_WS_HOST=stream.binance.us`** (see `apply_stream_env_overrides` in `src/binance/stream_env.cpp`). CLI also supports **`--port`**, **`--stream`** / **`--binance-stream`**. **Polymarket** uses **`wss://ws-subscriptions-clob.polymarket.com/ws/market`**. **Orders and balances** use **`poly_daemon.py`** + **CLOB HTTP REST**, not WebSocket.

### ThreadPool and multithreading

**Pool:** `include/core/thread_pool.hpp`, `src/core/thread_pool.cpp`; tasks via `dispatch(Task)`; workers dequeue from a queue and track `inflight`; `wait_idle()` before tearing down WS. CMake uses `std::jthread` when available, else `std::thread` + join.

**Pool work (one task per message when workers > 0; else same work inline on the WS thread):**

| Feed | Flag | Task |
|------|------|------|
| Binance `StreamClient` | `--parse-workers` | JSON parse → `BookTickerTick` / `TradeTick` → `on_bookticker` / `on_trade` |
| Polymarket `WsFixedTokenQuoteFeed` | `--poly-parse-workers` | JSON parse / quote path (`on_ws_message_inner`) |

Strategy, orders, and the `Event` consumer are **not** on this pool.

**Other threads:** `apps/btc_poly_runner.cpp` — main; `consumer_thr` (single consumer on `Event` queue); `bin_thread` (Binance `StreamClient`). `live_pipeline` — two `AsyncJsonlWriter` threads + `bin_thread`. `ws_market_client.cpp` — `ping_thr`. IXWebSocket runs its own internal threads for each socket.

**Quick test:** from repo root, `ctest --test-dir build -R thread_pool_smoke` (or `cd build && ctest -R thread_pool_smoke`); or run `./build/thread_pool_smoke` directly. Live smoke: `./build/paper_trader --live --parse-workers 2 --poly-parse-workers 2`.

---

## Backtest results

Offline analysis is in **`data/leag_lag.ipynb`**. **First** record **`data/bin_live.jsonl`** and **`data/poly_live.jsonl`** with **`live_pipeline`** (§4b), install **`numpy pandas matplotlib scipy tqdm`** ( **`arch`** for GARCH cells), then from **`data/`** run **`jupyter notebook leag_lag.ipynb`** (paths assume that cwd).

**Example run** (author snapshot; yours will differ): latency study on **1203** overlapping buckets — median lag **~299 ms**, **~11.6%** censored within **W = 3 s**. Strategy sim (**~$100** start) comparing **realized 1h σ** vs **GARCH(1,1)**:

| Metric | Realized σ | GARCH |
|--------|------------|-------|
| Trades | 1542 | 1765 |
| Total PnL (USDC) | 170.55 | 152.31 |
| Final equity | 270.55 | 252.31 |
| Win rate | 0.552 | 0.522 |

---

## Model & statistics

**Setup:** **S** = Binance mid, **K** = first mid after 5m bucket open, **T** = years to expiry, **r = 0.035**, **σ** = annualized vol (**~1h realized** resampled grid, default **`--sigma-step-ms` 300 ms**, clamp **[0.05, 5]**; or **GARCH(1,1)** via **`arch`** / daemon). **UP** and **DOWN** share the same **d₂**.

**Digital prices (used in code):**

$$
d_2 = \frac{\ln(S/K) + \bigl(r - \tfrac{1}{2}\sigma^2\bigr)\,T}{\sigma\sqrt{T}}, \qquad
P_{\mathrm{UP}} = e^{-rT}\mathcal{N}(d_2), \quad
P_{\mathrm{DOWN}} = e^{-rT}\mathcal{N}(-d_2)
$$

*(Vanilla European call reference: $d_1 = d_2 + \sigma\sqrt{T}$, $C = S\mathcal{N}(d_1) - K e^{-rT}\mathcal{N}(d_2)$.)*

**Trading:** **BUY** when **theo − ask ≥ `--entry`** (default **0.15**); pick **UP** vs **DOWN** by larger edge if both qualify. **SELL** when **|theo − mid| ≤ `--close`** (default **0.005**). Paper adds **`--lat-ms`** (default **1000**). See **`apps/btc_poly_runner.cpp`**.
