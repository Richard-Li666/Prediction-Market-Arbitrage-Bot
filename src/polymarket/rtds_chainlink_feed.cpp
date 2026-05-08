#include "polymarket/rtds_chainlink_feed.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "core/clock.hpp"

namespace ll::polymarket {

namespace {

std::once_flag net_once;

void ensure_net() {
  std::call_once(net_once, [] { ix::initNetSystem(); });
}

std::string lower_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool btc_usd_symbol_match(const std::string& raw) {
  const std::string s = lower_ascii(raw);
  return s == "btc/usd" || s == "btcusd";
}

bool json_to_ms(const nlohmann::json& j, std::int64_t* out_ms) {
  if (j.is_number_integer()) {
    *out_ms = j.get<std::int64_t>();
    return true;
  }
  if (j.is_number_unsigned()) {
    *out_ms = static_cast<std::int64_t>(j.get<std::uint64_t>());
    return true;
  }
  if (j.is_number_float()) {
    *out_ms = static_cast<std::int64_t>(j.get<double>());
    return true;
  }
  return false;
}

bool json_to_double(const nlohmann::json& j, double* out) {
  if (j.is_number()) {
    *out = j.get<double>();
    return true;
  }
  if (j.is_string()) {
    try {
      *out = std::stod(j.get<std::string>());
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}

/// Same selection semantics as `rtds_chainlink_strike.cpp` (single-threaded replay; no mutex).
struct TickSel {
  bool have_before = false;
  std::int64_t best_before_ts = 0;
  double best_before_px = 0.0;
  bool have_after = false;
  std::int64_t best_after_ts = 0;
  double best_after_px = 0.0;
  bool have_any = false;
  std::int64_t best_any_skew = std::numeric_limits<std::int64_t>::max();
  std::int64_t best_any_ts = 0;
  double best_any_px = 0.0;

  void consider(std::int64_t ts, double val, std::int64_t target_ms, std::int64_t max_skew_ms) {
    const std::int64_t skew = std::llabs(ts - target_ms);
    if (ts <= target_ms) {
      const std::int64_t dt = target_ms - ts;
      if (dt <= max_skew_ms && (!have_before || ts > best_before_ts)) {
        have_before = true;
        best_before_ts = ts;
        best_before_px = val;
      }
    }
    if (ts >= target_ms) {
      const std::int64_t dt = ts - target_ms;
      if (dt <= max_skew_ms && (!have_after || ts < best_after_ts)) {
        have_after = true;
        best_after_ts = ts;
        best_after_px = val;
      }
    }
    if (!have_any || skew < best_any_skew) {
      have_any = true;
      best_any_skew = skew;
      best_any_ts = ts;
      best_any_px = val;
    }
  }

  bool have_first_after() const { return have_after; }

  bool assign_fallback(double* out_px, std::int64_t* out_ts, std::int64_t max_skew_ms, const char** out_pick) const {
    if (have_after) {
      *out_px = best_after_px;
      *out_ts = best_after_ts;
      *out_pick = "after";
      return true;
    }
    if (have_before) {
      *out_px = best_before_px;
      *out_ts = best_before_ts;
      *out_pick = "before";
      return true;
    }
    if (have_any && best_any_skew <= max_skew_ms) {
      *out_px = best_any_px;
      *out_ts = best_any_ts;
      *out_pick = "nearest";
      return true;
    }
    return false;
  }
};

void collect_ticks_from_payload(const nlohmann::json& p, std::vector<std::pair<std::int64_t, double>>* out) {
  if (!p.is_object()) {
    return;
  }
  if (p.contains("data") && p["data"].is_array()) {
    for (const auto& row : p["data"]) {
      if (!row.is_object()) {
        continue;
      }
      if (!row.contains("symbol") || !btc_usd_symbol_match(row["symbol"].get<std::string>())) {
        continue;
      }
      std::int64_t ts = 0;
      double val = 0.0;
      if (!json_to_ms(row["timestamp"], &ts) || !json_to_double(row["value"], &val)) {
        continue;
      }
      if (!std::isfinite(val) || val <= 0.0) {
        continue;
      }
      out->push_back({ts, val});
    }
    return;
  }
  if (!p.contains("symbol") || !p["symbol"].is_string()) {
    return;
  }
  if (!btc_usd_symbol_match(p["symbol"].get<std::string>())) {
    return;
  }
  std::int64_t ts = 0;
  double val = 0.0;
  if (!json_to_ms(p["timestamp"], &ts) || !json_to_double(p["value"], &val)) {
    return;
  }
  if (!std::isfinite(val) || val <= 0.0) {
    return;
  }
  out->push_back({ts, val});
}

void collect_ticks_from_message(const nlohmann::json& j, std::vector<std::pair<std::int64_t, double>>* out) {
  auto one = [&](const nlohmann::json& o) {
    if (!o.is_object() || !o.contains("topic") || !o["topic"].is_string()) {
      return;
    }
    const std::string topic = lower_ascii(o["topic"].get<std::string>());
    if (topic != "crypto_prices_chainlink") {
      return;
    }
    if (!o.contains("payload")) {
      return;
    }
    collect_ticks_from_payload(o["payload"], out);
  };
  if (j.is_array()) {
    for (const auto& x : j) {
      one(x);
    }
    return;
  }
  one(j);
}

}  // namespace

struct RtdsChainlinkFeedImpl {
  std::mutex wait_serial_;
  std::mutex buf_mu_;
  std::condition_variable cv_;
  std::deque<std::pair<std::int64_t, double>> buffer_;
  static constexpr std::size_t kMaxBuffer = 900;

  std::mutex on_tick_mu_;
  std::function<void(std::int64_t, double)> on_tick_;

  std::atomic<bool> started_{false};
  std::atomic<bool> stop_ping_{false};
  std::atomic<bool> stop_feed_{false};

  ix::WebSocket ws_;
  std::thread ping_thr_;

  void notify_tick_batch(const std::vector<std::pair<std::int64_t, double>>& ticks) {
    std::function<void(std::int64_t, double)> cb;
    {
      std::lock_guard<std::mutex> lk(on_tick_mu_);
      cb = on_tick_;
    }
    if (!cb) {
      return;
    }
    for (const auto& pr : ticks) {
      cb(pr.first, pr.second);
    }
  }

  void push_ticks(const std::vector<std::pair<std::int64_t, double>>& ticks) {
    if (ticks.empty()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lk(buf_mu_);
      for (const auto& pr : ticks) {
        buffer_.push_back(pr);
        while (buffer_.size() > kMaxBuffer) {
          buffer_.pop_front();
        }
      }
      cv_.notify_all();
    }
    notify_tick_batch(ticks);
  }

  void run_ping() {
    while (!stop_ping_.load(std::memory_order_relaxed)) {
      for (int i = 0; i < 50; ++i) {
        if (stop_ping_.load(std::memory_order_relaxed)) {
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (ws_.getReadyState() == ix::ReadyState::Open) {
        ws_.sendText("PING");
      }
    }
  }

  void ensure_started_inner(std::string* err) {
    if (started_.load(std::memory_order_acquire)) {
      return;
    }
    ensure_net();
    ws_.setUrl("wss://ws-live-data.polymarket.com");
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (!msg || msg->type != ix::WebSocketMessageType::Message) {
        return;
      }
      try {
        const auto j = nlohmann::json::parse(msg->str);
        std::vector<std::pair<std::int64_t, double>> ticks;
        collect_ticks_from_message(j, &ticks);
        push_ticks(ticks);
      } catch (...) {
      }
    });

    ws_.start();

    const auto connect_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < connect_deadline &&
           ws_.getReadyState() != ix::ReadyState::Open) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (ws_.getReadyState() != ix::ReadyState::Open) {
      if (err) {
        *err = "RTDS feed websocket did not open in time";
      }
      return;
    }

    static constexpr char kSub[] =
        R"({"action":"subscribe","subscriptions":[{"topic":"crypto_prices_chainlink","type":"*","filters":"{\"symbol\":\"btc/usd\"}"}]})";
    ws_.sendText(kSub);

    stop_ping_.store(false, std::memory_order_relaxed);
    if (ping_thr_.joinable()) {
      ping_thr_.join();
    }
    ping_thr_ = std::thread([this]() { run_ping(); });

    started_.store(true, std::memory_order_release);
  }

  void stop_inner() {
    stop_feed_.store(true, std::memory_order_relaxed);
    stop_ping_.store(true, std::memory_order_relaxed);
    ws_.stop();
    if (ping_thr_.joinable()) {
      ping_thr_.join();
    }
    started_.store(false, std::memory_order_release);
  }

  bool wait_strike_inner(std::int64_t boundary_ms, std::int64_t max_skew_ms, std::int64_t timeout_ms,
                         double* out_px, std::int64_t* out_payload_ts, const char** out_pick, bool* out_immediate,
                         std::string* error_message) {
    std::lock_guard<std::mutex> serial(wait_serial_);

    std::string estart_err;
    ensure_started_inner(&estart_err);
    if (!started_.load(std::memory_order_acquire)) {
      if (error_message) {
        *error_message = estart_err.empty() ? "RTDS feed failed to start" : estart_err;
      }
      return false;
    }

    if (timeout_ms < 500) {
      timeout_ms = 500;
    }
    if (max_skew_ms < 1000) {
      max_skew_ms = 1000;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
      std::vector<std::pair<std::int64_t, double>> snap;
      {
        std::lock_guard<std::mutex> lk(buf_mu_);
        snap.assign(buffer_.begin(), buffer_.end());
      }
      TickSel sel;
      for (const auto& pr : snap) {
        sel.consider(pr.first, pr.second, boundary_ms, max_skew_ms);
      }
      if (sel.have_first_after()) {
        *out_px = sel.best_after_px;
        *out_payload_ts = sel.best_after_ts;
        *out_pick = "after";
        *out_immediate = true;
        return true;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      const auto remain_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      const auto sleep_ms = std::min<std::int64_t>(remain_ms, 40);
      std::unique_lock<std::mutex> lk(buf_mu_);
      cv_.wait_for(lk, std::chrono::milliseconds(sleep_ms));
    }

    std::vector<std::pair<std::int64_t, double>> snap;
    {
      std::lock_guard<std::mutex> lk(buf_mu_);
      snap.assign(buffer_.begin(), buffer_.end());
    }
    TickSel sel;
    for (const auto& pr : snap) {
      sel.consider(pr.first, pr.second, boundary_ms, max_skew_ms);
    }
    double px = 0.0;
    std::int64_t pts = 0;
    const char* pick = "";
    if (sel.assign_fallback(&px, &pts, max_skew_ms, &pick)) {
      *out_px = px;
      *out_payload_ts = pts;
      *out_pick = pick;
      *out_immediate = false;
      return true;
    }

    // Failure diagnostics: helps distinguish "boundary wrong" vs "ticks not aligned" vs "no ticks".
    const std::int64_t now_wall_ms = ll::core::system_ms();
    std::int64_t latest_payload_ts_ms = -1;
    if (!snap.empty()) {
      latest_payload_ts_ms = snap.back().first;
    }
    const std::int64_t latest_dt_ms = (latest_payload_ts_ms >= 0) ? (latest_payload_ts_ms - boundary_ms) : 0;
    if (sel.have_any) {
      if (error_message) {
        *error_message = "RTDS btc/usd skew too large (best |dt|=" + std::to_string(sel.best_any_skew) +
                         " ms vs max " + std::to_string(max_skew_ms) + " ms)";
      }
      std::cerr << "[rtds_chainlink][wait_strike] FAIL skew_too_large"
                << " target_ms=" << boundary_ms
                << " now_wall_ms=" << now_wall_ms
                << " latest_payload_ts_ms=" << latest_payload_ts_ms
                << " latest_dt_ms=" << latest_dt_ms
                << " best_any_ts_ms=" << sel.best_any_ts
                << " best_any_skew_ms=" << sel.best_any_skew
                << " max_skew_ms=" << max_skew_ms
                << " timeout_ms=" << timeout_ms
                << " buf_n=" << snap.size()
                << "\n";
    } else if (error_message && error_message->empty()) {
      *error_message = "no RTDS crypto_prices_chainlink btc/usd samples within timeout";
      std::cerr << "[rtds_chainlink][wait_strike] FAIL no_samples"
                << " target_ms=" << boundary_ms
                << " now_wall_ms=" << now_wall_ms
                << " latest_payload_ts_ms=" << latest_payload_ts_ms
                << " latest_dt_ms=" << latest_dt_ms
                << " max_skew_ms=" << max_skew_ms
                << " timeout_ms=" << timeout_ms
                << " buf_n=" << snap.size()
                << "\n";
    }
    return false;
  }
};

static RtdsChainlinkFeedImpl g_feed;

RtdsChainlinkFeed& RtdsChainlinkFeed::instance() {
  static RtdsChainlinkFeed inst;
  return inst;
}

void RtdsChainlinkFeed::ensure_started() {
  std::string err;
  g_feed.ensure_started_inner(&err);
}

void RtdsChainlinkFeed::set_on_tick(std::function<void(std::int64_t, double)> cb) {
  std::lock_guard<std::mutex> lk(g_feed.on_tick_mu_);
  g_feed.on_tick_ = std::move(cb);
}

void RtdsChainlinkFeed::stop() { g_feed.stop_inner(); }

bool RtdsChainlinkFeed::wait_strike(std::int64_t boundary_ms, std::int64_t max_skew_ms,
                                    std::int64_t timeout_ms, double* out_px, std::int64_t* out_payload_ts,
                                    const char** out_pick, bool* out_immediate, std::string* error_message) {
  return g_feed.wait_strike_inner(boundary_ms, max_skew_ms, timeout_ms, out_px, out_payload_ts, out_pick,
                                  out_immediate, error_message);
}

}  // namespace ll::polymarket
