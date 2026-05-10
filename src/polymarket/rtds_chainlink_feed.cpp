#include "polymarket/rtds_chainlink_feed.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "core/clock.hpp"
#include "logging/jsonl_writer.hpp"

namespace ll::polymarket {

namespace {

std::once_flag net_once;

/// RTDS `crypto_prices_chainlink` subscription (btc/usd). Sent on every successful `Open` (initial + reconnect).
static constexpr char kChainlinkSub[] =
    R"({"action":"subscribe","subscriptions":[{"topic":"crypto_prices_chainlink","type":"*","filters":"{\"symbol\":\"btc/usd\"}"}]})";

constexpr int kConnectWaitSec = 30;
constexpr int kWatchdogPeriodSec = 10;
/// If we once received ticks but none for this long, assume stalled WS (silent drop / half-open).
constexpr std::int64_t kStaleTickMs = 120000;
/// Connection reported open but no parsed ticks (auth/schema/subscription issue).
constexpr std::int64_t kNoTickAfterOpenMs = 90000;

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

std::once_flag rtds_err_writer_once;
std::unique_ptr<ll::logging::JsonlWriter> rtds_err_writer;

/// Appends one JSONL row to `data/errors/rtds_chainlink_errors.jsonl` (or `RTDS_CHAINLINK_ERROR_LOG`) and mirrors to stderr.
void append_rtds_chainlink_error_jsonl(const nlohmann::json& fields, const std::string& cerr_line) {
  std::call_once(rtds_err_writer_once, [] {
    std::string path = "data/errors/rtds_chainlink_errors.jsonl";
    if (const char* ev = std::getenv("RTDS_CHAINLINK_ERROR_LOG")) {
      if (ev[0] != '\0') {
        path = ev;
      }
    }
    try {
      const std::filesystem::path p(path);
      if (const auto parent = p.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent);
      }
      rtds_err_writer = std::make_unique<ll::logging::JsonlWriter>(std::move(path));
    } catch (...) {
    }
  });

  nlohmann::json row = fields;
  row["schema_version"] = 1;
  row["source"] = "rtds_chainlink";
  row["local_ts_wall_ms"] = ll::core::system_ms();
  if (rtds_err_writer) {
    try {
      rtds_err_writer->append(row);
    } catch (...) {
    }
  }
  std::cerr << cerr_line << '\n';
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

  std::mutex start_mu_;
  std::atomic<bool> started_{false};
  std::atomic<bool> stop_ping_{false};
  std::atomic<bool> stop_feed_{false};

  std::atomic<std::int64_t> last_tick_wall_ms_{0};
  std::atomic<std::int64_t> session_start_wall_ms_{0};
  std::atomic<bool> ever_got_tick_{false};

  std::atomic<bool> watchdog_stop_{false};
  std::thread watchdog_thr_;

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
    ever_got_tick_.store(true, std::memory_order_relaxed);
    last_tick_wall_ms_.store(ll::core::system_ms(), std::memory_order_relaxed);
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

  bool wait_until_open(std::string* err) {
    const auto connect_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(kConnectWaitSec);
    while (std::chrono::steady_clock::now() < connect_deadline &&
           ws_.getReadyState() != ix::ReadyState::Open) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (ws_.getReadyState() != ix::ReadyState::Open) {
      if (err) {
        *err = "RTDS feed websocket did not open in time";
      }
      return false;
    }
    return true;
  }

  void install_ws_handlers() {
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (!msg) {
        return;
      }
      if (msg->type == ix::WebSocketMessageType::Open) {
        ws_.sendText(std::string(kChainlinkSub));
        return;
      }
      if (msg->type == ix::WebSocketMessageType::Close) {
        nlohmann::json ev;
        ev["event_type"] = "ws_close";
        ev["close_code"] = msg->closeInfo.code;
        ev["close_reason"] = msg->closeInfo.reason;
        ev["remote"] = msg->closeInfo.remote;
        const std::string line = std::string("[rtds_chainlink] ws close code=") +
                                 std::to_string(msg->closeInfo.code) + " reason=" + msg->closeInfo.reason +
                                 " remote=" + (msg->closeInfo.remote ? "true" : "false");
        append_rtds_chainlink_error_jsonl(ev, line);
        return;
      }
      if (msg->type == ix::WebSocketMessageType::Error) {
        nlohmann::json ev;
        ev["event_type"] = "ws_error";
        ev["error_reason"] = msg->errorInfo.reason;
        append_rtds_chainlink_error_jsonl(ev,
                                          "[rtds_chainlink] ws error reason=" + msg->errorInfo.reason);
        return;
      }
      if (msg->type != ix::WebSocketMessageType::Message) {
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
  }

  void start_ping_worker() {
    stop_ping_.store(false, std::memory_order_relaxed);
    if (ping_thr_.joinable()) {
      ping_thr_.join();
    }
    ping_thr_ = std::thread([this]() { run_ping(); });
  }

  void reconnect_inner(const char* reason) {
    std::lock_guard<std::mutex> lk(start_mu_);
    if (stop_feed_.load(std::memory_order_relaxed)) {
      return;
    }
    if (!started_.load(std::memory_order_acquire)) {
      return;
    }

    {
      nlohmann::json ev;
      ev["event_type"] = "reconnect";
      ev["reason"] = reason;
      append_rtds_chainlink_error_jsonl(ev, std::string("[rtds_chainlink] reconnect: ") + reason);
    }

    ever_got_tick_.store(false, std::memory_order_relaxed);
    last_tick_wall_ms_.store(0, std::memory_order_relaxed);
    session_start_wall_ms_.store(0, std::memory_order_relaxed);

    {
      std::lock_guard<std::mutex> lk_buf(buf_mu_);
      buffer_.clear();
    }
    cv_.notify_all();

    stop_ping_.store(true, std::memory_order_relaxed);
    if (ping_thr_.joinable()) {
      ping_thr_.join();
    }
    ws_.stop();

    std::string reopen_err;
    ws_.start();
    if (!wait_until_open(&reopen_err)) {
      nlohmann::json ev;
      ev["event_type"] = "reconnect_failed";
      ev["detail"] = reopen_err;
      append_rtds_chainlink_error_jsonl(ev, "[rtds_chainlink] reconnect failed: " + reopen_err);
      started_.store(false, std::memory_order_release);
      return;
    }

    start_ping_worker();
    session_start_wall_ms_.store(ll::core::system_ms(), std::memory_order_relaxed);
  }

  void run_watchdog() {
    while (!watchdog_stop_.load(std::memory_order_relaxed)) {
      for (int i = 0; i < kWatchdogPeriodSec * 10; ++i) {
        if (watchdog_stop_.load(std::memory_order_relaxed)) {
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (!started_.load(std::memory_order_acquire)) {
        continue;
      }
      if (stop_feed_.load(std::memory_order_relaxed)) {
        continue;
      }

      const std::int64_t now = ll::core::system_ms();
      const std::int64_t last = last_tick_wall_ms_.load(std::memory_order_relaxed);
      const std::int64_t sess = session_start_wall_ms_.load(std::memory_order_relaxed);
      const bool ever = ever_got_tick_.load(std::memory_order_relaxed);

      const char* why = nullptr;
      if (ws_.getReadyState() != ix::ReadyState::Open) {
        why = "ws_not_open";
      } else if (ever && last > 0 && (now - last) > kStaleTickMs) {
        why = "stale_ticks";
      } else if (!ever && sess > 0 && (now - sess) > kNoTickAfterOpenMs) {
        why = "no_ticks_after_open";
      }
      if (why) {
        reconnect_inner(why);
      }
    }
  }

  void ensure_started_inner(std::string* err) {
    if (started_.load(std::memory_order_acquire)) {
      return;
    }
    std::lock_guard<std::mutex> lk(start_mu_);
    if (started_.load(std::memory_order_acquire)) {
      return;
    }
    ensure_net();
    ws_.setUrl("wss://ws-live-data.polymarket.com");
    install_ws_handlers();

    ws_.start();

    if (!wait_until_open(err)) {
      const std::string detail = (err && !err->empty()) ? *err : "RTDS feed websocket did not open in time";
      nlohmann::json ev;
      ev["event_type"] = "connect_failed";
      ev["detail"] = detail;
      append_rtds_chainlink_error_jsonl(ev, "[rtds_chainlink] connect failed: " + detail);
      return;
    }

    start_ping_worker();
    session_start_wall_ms_.store(ll::core::system_ms(), std::memory_order_relaxed);
    started_.store(true, std::memory_order_release);

    if (!watchdog_thr_.joinable()) {
      watchdog_stop_.store(false, std::memory_order_relaxed);
      watchdog_thr_ = std::thread([this]() { run_watchdog(); });
    }
  }

  void stop_inner() {
    stop_feed_.store(true, std::memory_order_relaxed);
    watchdog_stop_.store(true, std::memory_order_relaxed);
    if (watchdog_thr_.joinable()) {
      watchdog_thr_.join();
    }
    stop_ping_.store(true, std::memory_order_relaxed);
    ws_.stop();
    if (ping_thr_.joinable()) {
      ping_thr_.join();
    }
    started_.store(false, std::memory_order_release);
    ever_got_tick_.store(false, std::memory_order_relaxed);
    last_tick_wall_ms_.store(0, std::memory_order_relaxed);
    session_start_wall_ms_.store(0, std::memory_order_relaxed);
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
      {
        nlohmann::json ev;
        ev["event_type"] = "wait_strike_fail";
        ev["fail_kind"] = "skew_too_large";
        ev["target_ms"] = boundary_ms;
        ev["now_wall_ms"] = now_wall_ms;
        ev["latest_payload_ts_ms"] = latest_payload_ts_ms;
        ev["latest_dt_ms"] = latest_dt_ms;
        ev["best_any_ts_ms"] = sel.best_any_ts;
        ev["best_any_skew_ms"] = sel.best_any_skew;
        ev["max_skew_ms"] = max_skew_ms;
        ev["timeout_ms"] = timeout_ms;
        ev["buf_n"] = snap.size();
        std::ostringstream oss;
        oss << "[rtds_chainlink][wait_strike] FAIL skew_too_large"
            << " target_ms=" << boundary_ms << " now_wall_ms=" << now_wall_ms
            << " latest_payload_ts_ms=" << latest_payload_ts_ms << " latest_dt_ms=" << latest_dt_ms
            << " best_any_ts_ms=" << sel.best_any_ts << " best_any_skew_ms=" << sel.best_any_skew
            << " max_skew_ms=" << max_skew_ms << " timeout_ms=" << timeout_ms << " buf_n=" << snap.size();
        append_rtds_chainlink_error_jsonl(ev, oss.str());
      }
    } else if (error_message && error_message->empty()) {
      *error_message = "no RTDS crypto_prices_chainlink btc/usd samples within timeout";
      {
        nlohmann::json ev;
        ev["event_type"] = "wait_strike_fail";
        ev["fail_kind"] = "no_samples";
        ev["target_ms"] = boundary_ms;
        ev["now_wall_ms"] = now_wall_ms;
        ev["latest_payload_ts_ms"] = latest_payload_ts_ms;
        ev["latest_dt_ms"] = latest_dt_ms;
        ev["max_skew_ms"] = max_skew_ms;
        ev["timeout_ms"] = timeout_ms;
        ev["buf_n"] = snap.size();
        std::ostringstream oss;
        oss << "[rtds_chainlink][wait_strike] FAIL no_samples"
            << " target_ms=" << boundary_ms << " now_wall_ms=" << now_wall_ms
            << " latest_payload_ts_ms=" << latest_payload_ts_ms << " latest_dt_ms=" << latest_dt_ms
            << " max_skew_ms=" << max_skew_ms << " timeout_ms=" << timeout_ms << " buf_n=" << snap.size();
        append_rtds_chainlink_error_jsonl(ev, oss.str());
      }
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
