#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "polymarket/ws_fixed_token_feed.hpp"
#include "telemetry/pipeline.hpp"

namespace {
std::atomic<bool> g_stop{false};
void on_sig(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);

  if (argc < 2) {
    std::cerr << "usage: polymarket_quote_probe <token_id>\n";
    return 2;
  }

  const std::string token = argv[1];

  ll::telemetry::Pipeline tel;
  ll::polymarket::WsFixedTokenQuoteFeed feed(&tel);

  feed.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
    std::cout << "local_mono_ns=" << q.local_mono_ns << " bid=" << q.best_bid << " ask=" << q.best_ask
              << " token=" << q.token_id << "\n";
  });

  std::string err;
  if (!feed.start(token, &err)) {
    std::cerr << "start failed: " << err << "\n";
    return 1;
  }

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  feed.stop();
  std::cerr << "telemetry: " << tel.summary() << "\n";
  return 0;
}
