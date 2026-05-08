#pragma once

namespace ll::btc_poly {

/// Runs the BTC 5m Up/Down bucket strategy (same loop as `paper_trader --live`).
/// @param live_execution  If true, sends market orders via `LiveExecutor` / poly_daemon.
int run_strategy_main(int argc, char** argv, bool live_execution);

}  // namespace ll::btc_poly
