#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <cstdint>
#include <deque>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <queue>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "binance/stream_client.hpp"
#include "binance/stream_env.hpp"
#include "core/clock.hpp"
#include "execution/executor.hpp"
#include "polymarket/bucket_market_discovery.hpp"
#include "polymarket/polymarket_web_ptb.hpp"
#include "polymarket/rtds_chainlink_feed.hpp"
#include "polymarket/rtds_chainlink_strike.hpp"
#include "polymarket/ws_fixed_token_feed.hpp"
#include "replay/timeline_merge.hpp"
#include "signals/engine.hpp"
#include "sim/latency_backtest.hpp"
#include "telemetry/pipeline.hpp"

#include "core/sync_cerr.hpp"
namespace {

constexpr std::int64_t kRtdsStrikeTimeoutMs = 15000;
/// Accept Chainlink RTDS tick if `|payload_ts - event_start|` ≤ this (Polymarket crypto stream has no long history buffer).
constexpr std::int64_t kRtdsStrikeMaxSkewMs = 180000;

bool bucket_disc_has_strike(const ll::polymarket::BtcFiveMinuteBucketDiscovery& d) {
  return std::isfinite(d.price_to_beat) && d.price_to_beat > 0.0;
}

void maybe_fill_strike_rtds(ll::polymarket::BtcFiveMinuteBucketDiscovery& disc, const char* log_tag) {
  if (disc.gamma_has_price_to_beat && bucket_disc_has_strike(disc)) {
    return;
  }
  std::string rerr;
  if (ll::polymarket::fill_strike_from_polymarket_rtds_chainlink(disc, kRtdsStrikeTimeoutMs,
                                                                 kRtdsStrikeMaxSkewMs, &rerr)) {
    ll::io::SyncCerrLock _;
    std::cerr << log_tag << "[rtds_chainlink] strike=" << disc.price_to_beat << " target_wall_ms="
              << disc.event_start_wall_ms << "\n";
    return;
  }
  {
    ll::io::SyncCerrLock _;
    std::cerr << log_tag << "[rtds_chainlink] strike unavailable: " << rerr << "\n";
  }
}

void maybe_fill_strike_web_then_rtds(ll::polymarket::BtcFiveMinuteBucketDiscovery& disc, bool try_web_first,
                                     const char* log_tag) {
  if (disc.gamma_has_price_to_beat && bucket_disc_has_strike(disc)) {
    return;
  }
  if (try_web_first) {
    std::string werr;
    if (ll::polymarket::fill_strike_from_polymarket_web_event_page(disc, &werr)) {
      ll::io::SyncCerrLock _;
      std::cerr << log_tag << "[web_ptb] strike=" << disc.price_to_beat << " slug=" << disc.confirmed_slug
                << "\n";
      return;
    }
    {
      ll::io::SyncCerrLock _;
      std::cerr << log_tag << "[web_ptb] unavailable: " << werr << "\n";
    }
  }
  maybe_fill_strike_rtds(disc, log_tag);
}

/// Best-effort .env loader for the C++ process.
/// Supports KEY=VALUE and optional `export KEY=VALUE`.
/// Only sets a variable if it is not already present in the environment.
void load_dotenv(const char* path = ".env") {
  std::ifstream f(path);
  if (!f.is_open()) return;
  std::string raw;
  while (std::getline(f, raw)) {
    // trim leading whitespace
    auto beg = raw.find_first_not_of(" \t\r\n");
    if (beg == std::string::npos) continue;
    std::string line = raw.substr(beg);
    if (line.empty() || line[0] == '#') continue;
    if (line.rfind("export ", 0) == 0) line = line.substr(7);
    auto eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    // strip trailing whitespace / CR
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
    // strip optional surrounding double quotes
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
      val = val.substr(1, val.size() - 2);
    }
    if (key.empty()) continue;
    // only set if not already present (0 = don't overwrite)
    ::setenv(key.c_str(), val.c_str(), 0);
  }
}

std::atomic<bool> g_stop{false};
void on_sig(int) { g_stop = true; }

/// `q` in [0,1]; `v` sorted ascending (copy).
static double percentile_sorted_copy(std::vector<std::int64_t> v, double q) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(v.begin(), v.end());
  q = std::max(0.0, std::min(1.0, q));
  const double pos = q * static_cast<double>(v.size() - 1);
  const std::size_t i = static_cast<std::size_t>(pos);
  const std::size_t j = (i + 1 < v.size()) ? i + 1 : i;
  const double t = pos - static_cast<double>(i);
  return (1.0 - t) * static_cast<double>(v[i]) + t * static_cast<double>(v[j]);
}

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* ud) {
  static_cast<std::string*>(ud)->append(ptr, size * nmemb);
  return size * nmemb;
}

/// REST base for `/api/v3/klines` — align with WebSocket host (`--host` / `LL_BINANCE_WS_HOST`).
/// Non‑US IPs often get HTTP 451 from api.binance.com; use US WS + matching REST (or set LL_BINANCE_REST_BASE).
std::string binance_klines_rest_base(const ll::binance::StreamClientConfig& bin_cfg) {
  if (const char* b = std::getenv("LL_BINANCE_REST_BASE")) {
    if (b[0] != '\0') {
      std::string s(b);
      while (!s.empty() && (s.back() == '/' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
      }
      return s;
    }
  }
  if (bin_cfg.ws_host.find("binance.us") != std::string::npos) {
    return "https://api.binance.us";
  }
  return "https://api.binance.com";
}

static bool parse_chainlink_jsonl_row(const nlohmann::json& j, std::int64_t* payload_ts_ms, double* px) {
  if (!payload_ts_ms || !px || !j.is_object()) {
    return false;
  }
  if (j.value("event_type", std::string()) != "chainlink") {
    return false;
  }
  if (!j.contains("payload_ts_ms")) {
    return false;
  }
  *payload_ts_ms = j["payload_ts_ms"].get<std::int64_t>();
  if (j.contains("payload") && j["payload"].is_object() && j["payload"].contains("price")) {
    *px = j["payload"]["price"].get<double>();
  } else {
    return false;
  }
  return *payload_ts_ms > 0 && std::isfinite(*px) && *px > 0.0;
}

static void ingest_chainlink_jsonl_lines(const std::string& blob, std::int64_t min_payload_ts_ms,
                                         std::map<std::int64_t, double>& by_ts) {
  std::istringstream ss(blob);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) {
      continue;
    }
    try {
      const auto j = nlohmann::json::parse(line);
      std::int64_t ts_ms = 0;
      double px = 0.0;
      if (!parse_chainlink_jsonl_row(j, &ts_ms, &px) || ts_ms < min_payload_ts_ms) {
        continue;
      }
      by_ts[ts_ms] = px;
    } catch (...) {
    }
  }
}

/// Tail-read large jsonl (e.g. `data/chainlink_price.jsonl`) for the last ~12MB of lines.
static void ingest_chainlink_jsonl_file(const std::string& path, std::int64_t min_payload_ts_ms,
                                        std::map<std::int64_t, double>& by_ts) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return;
  }
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return;
  }
  const std::streamoff sz = in.tellg();
  if (sz <= 0) {
    return;
  }
  constexpr std::streamoff kMaxTailBytes = 12 * 1024 * 1024;
  const std::streamoff start = (sz > kMaxTailBytes) ? (sz - kMaxTailBytes) : 0;
  in.seekg(start);
  std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (start > 0) {
    const auto nl = blob.find('\n');
    if (nl != std::string::npos) {
      blob.erase(0, nl + 1);
    }
  }
  ingest_chainlink_jsonl_lines(blob, min_payload_ts_ms, by_ts);
}

/// Prefill σ history from Chainlink jsonl (`payload_ts_ms` + `payload.price`, same as backtest.ipynb).
void prefill_bin_hist_from_chainlink_jsonl(std::deque<std::pair<std::int64_t, double>>& hist,
                                           const std::vector<std::string>& paths,
                                           const char* log_pfx) {
  const std::int64_t now_ms = ll::core::system_ms();
  const std::int64_t min_ts = now_ms - 3600 * 1000LL;
  std::map<std::int64_t, double> by_ts;
  for (const auto& path : paths) {
    if (path.empty()) {
      continue;
    }
    ingest_chainlink_jsonl_file(path, min_ts, by_ts);
  }
  for (const auto& kv : by_ts) {
    hist.emplace_back(kv.first, kv.second);
  }
  {
    ll::io::SyncCerrLock _;
    std::cerr << log_pfx << " [prefill] Chainlink jsonl → bin_hist: " << by_ts.size()
              << " points (payload_ts_ms >= now-1h)";
    if (!paths.empty()) {
      std::cerr << " paths=";
      for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i) {
          std::cerr << ",";
        }
        std::cerr << paths[i];
      }
    }
    std::cerr << "\n";
  }
}

/// Fetch past 1h of Binance 1s klines and populate bin_hist (used when --spot-feed binance).
void prefill_bin_hist(std::deque<std::pair<std::int64_t, double>>& hist,
                      const ll::binance::StreamClientConfig& bin_cfg) {
  const std::string url = binance_klines_rest_base(bin_cfg) +
                          "/api/v3/klines?symbol=BTCUSDT&interval=1s&limit=3600";
  CURL* curl = curl_easy_init();
  if (!curl) {
    { ll::io::SyncCerrLock _; std::cerr << "[prefill] curl_easy_init failed\n"; }
    return;
  }
  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "leadlag/1.0");
  CURLcode res = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);
  if (res != CURLE_OK || code < 200 || code >= 300) {
    {
      ll::io::SyncCerrLock _;
      std::cerr << "[prefill] Binance klines fetch failed: curl=" << static_cast<int>(res)
                << " http=" << code << "\n";
    }
    return;
  }
  try {
    const auto arr = nlohmann::json::parse(body);
    if (!arr.is_array()) return;
    std::size_t count = 0;
    for (const auto& k : arr) {
      if (!k.is_array() || k.size() < 5) continue;
      // kline: [open_time, open, high, low, close, ...]
      const std::int64_t open_ms = k[0].get<std::int64_t>();
      const double open = std::stod(k[1].get<std::string>());
      const double close = std::stod(k[4].get<std::string>());
      const double mid = 0.5 * (open + close);
      hist.emplace_back(open_ms, mid);
      ++count;
    }
    { ll::io::SyncCerrLock _; std::cerr << "[prefill] loaded " << count << " Binance 1s klines into bin_hist\n"; }
  } catch (const std::exception& e) {
    { ll::io::SyncCerrLock _; std::cerr << "[prefill] parse error: " << e.what() << "\n"; }
  }
}

class TradeJsonlWriter {
 public:
  explicit TradeJsonlWriter(std::string path) : path_(std::move(path)) {
    try {
      std::filesystem::path p(path_);
      if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
      }
    } catch (...) {
      // best-effort
    }
    out_.open(path_, std::ios::out | std::ios::trunc);
    ok_ = static_cast<bool>(out_);
    if (!ok_) {
      { ll::io::SyncCerrLock _; std::cerr << "[paper_trader] cannot open trades jsonl for write: " << path_ << "\n"; }
    }
  }

  void append(const nlohmann::json& row) {
    if (!ok_) return;
    std::lock_guard<std::mutex> lk(mu_);
    out_ << row.dump() << "\n";
    out_.flush();
  }

  const std::string& path() const { return path_; }
  bool ok() const { return ok_; }

 private:
  std::string path_;
  std::ofstream out_;
  bool ok_{false};
  std::mutex mu_;
};

/// 每次进程启动清空会话 jsonl（避免上次 open 失败等原因留下旧内容）。
void reset_session_jsonl_file(const std::string& path) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) {
    std::filesystem::create_directories(p.parent_path(), ec);
  }
  std::filesystem::remove(p, ec);
}

struct HotState {
  std::mutex mu;
  std::int64_t active_epoch{-1};
  std::string slug;

  bool have_bin{false};
  double bin_mid{0.0};
  std::int64_t bin_wall_ms{0};
  /// Local wall at last spot update (Chainlink: ingest `system_ms()`; Binance: tick `local_wall_ms`).
  /// Used for BUY entry window vs bucket start; `bin_wall_ms` may be Chainlink payload time for theo T.
  std::int64_t bin_entry_clock_wall_ms{0};

  std::deque<std::pair<std::int64_t, double>> bin_hist;

  bool have_K{false};
  double K{0.0};

  double sigma_bucket{0.0};
  bool have_sigma{false};
  /// True once bin_hist covers ≥1h before the current σ anchor (slug bucket start or rolling wall).
  bool sigma_vol_window_full{false};

  bool have_up{false};
  double up_bid{0.0};
  double up_ask{0.0};

  bool have_down{false};
  double down_bid{0.0};
  double down_ask{0.0};

  /// Mono time when `theo-ask >= entry` (Up) or `theo-ask >= entry` (Down) first became true; -1 if not armed.
  /// Aligns with `data/real_backtest.ipynb` EDGE_PERSIST_MS (continuous edge in steady time).
  std::int64_t edge_up_ok_since_mono_ns{-1};
  std::int64_t edge_dn_ok_since_mono_ns{-1};
};

static inline double clamp01(double x) { return (x < 0.0) ? 0.0 : (x > 1.0 ? 1.0 : x); }

static inline double clamp_sigma(double s, double lo, double hi) {
  if (!std::isfinite(s)) {
    return lo;
  }
  if (s < lo) {
    return lo;
  }
  if (s > hi) {
    return hi;
  }
  return s;
}

static inline double normal_cdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

static inline double digital_call_prob(double S, double K, double T_years, double sigma, double r) {
  if (T_years <= 0.0) {
    return (S > K) ? 1.0 : 0.0;
  }
  if (sigma <= 0.0) {
    return (S > K) ? 1.0 : 0.0;
  }
  const double vsqrt = sigma * std::sqrt(T_years);
  const double d2 = (std::log(S / K) + (r - 0.5 * sigma * sigma) * T_years) / vsqrt;
  const double disc = std::exp(-r * T_years);
  return clamp01(disc * normal_cdf(d2));
}

static inline double realized_vol_1h_resampled_from_hist(
    const std::deque<std::pair<std::int64_t, double>>& hist, std::int64_t now_wall_ms,
    std::int64_t step_ms) {
  if (hist.size() < 3) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const std::int64_t start_ms = now_wall_ms - 3600 * 1000;
  std::vector<double> mids;
  mids.reserve(static_cast<std::size_t>(3600'000 / std::max<std::int64_t>(1, step_ms)) + 4);

  // Resample via last observation carry-forward.
  std::size_t idx = 0;
  while (idx + 1 < hist.size() && hist[idx].first < start_ms) {
    ++idx;
  }
  double last_mid = hist[idx].second;
  for (std::int64_t t = start_ms; t <= now_wall_ms; t += step_ms) {
    while (idx + 1 < hist.size() && hist[idx + 1].first <= t) {
      ++idx;
      last_mid = hist[idx].second;
    }
    mids.push_back(last_mid);
  }
  if (mids.size() < 10) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  std::vector<double> rets;
  rets.reserve(mids.size() - 1);
  for (std::size_t i = 1; i < mids.size(); ++i) {
    const double r = std::log(mids[i] / mids[i - 1]);
    if (std::isfinite(r)) {
      rets.push_back(r);
    }
  }
  if (rets.size() < 10) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  double mean = 0.0;
  for (double x : rets) mean += x;
  mean /= static_cast<double>(rets.size());
  double var = 0.0;
  for (double x : rets) {
    const double d = x - mean;
    var += d * d;
  }
  var /= static_cast<double>(rets.size() - 1);
  const double steps_per_year =
      (365.0 * 24.0 * 3600.0 * 1000.0) / static_cast<double>(step_ms);
  return std::sqrt(var * steps_per_year);
}

static inline std::vector<double> resample_mids_from_hist(
    const std::deque<std::pair<std::int64_t, double>>& hist, std::int64_t now_wall_ms,
    std::int64_t step_ms) {
  std::vector<double> mids;
  if (hist.size() < 3) return mids;
  const std::int64_t start_ms = now_wall_ms - 3600 * 1000;
  mids.reserve(static_cast<std::size_t>(3600'000 / std::max<std::int64_t>(1, step_ms)) + 4);
  std::size_t idx = 0;
  while (idx + 1 < hist.size() && hist[idx].first < start_ms) ++idx;
  double last_mid = hist[idx].second;
  for (std::int64_t t = start_ms; t <= now_wall_ms; t += step_ms) {
    while (idx + 1 < hist.size() && hist[idx + 1].first <= t) {
      ++idx;
      last_mid = hist[idx].second;
    }
    mids.push_back(last_mid);
  }
  return mids;
}

/// Annualized realized vol from resampled mids (matches backtest / rolling path in this file).
static bool realized_vol_from_resampled_mids(const std::vector<double>& mids, std::int64_t step_ms,
                                             double sigma_min, double sigma_max, double* out_sigma) {
  if (!out_sigma || mids.size() < 10) {
    return false;
  }
  std::vector<double> rets;
  rets.reserve(mids.size() - 1);
  for (std::size_t ri = 1; ri < mids.size(); ++ri) {
    const double lr = std::log(mids[ri] / mids[ri - 1]);
    if (std::isfinite(lr)) {
      rets.push_back(lr);
    }
  }
  if (rets.size() < 10) {
    return false;
  }
  double mean = 0.0;
  for (double x : rets) {
    mean += x;
  }
  mean /= static_cast<double>(rets.size());
  double var = 0.0;
  for (double x : rets) {
    const double d = x - mean;
    var += d * d;
  }
  var /= static_cast<double>(rets.size() - 1);
  const double steps_per_year =
      (365.0 * 24.0 * 3600.0 * 1000.0) / static_cast<double>(std::max<std::int64_t>(1, step_ms));
  const double sig = std::sqrt(var * steps_per_year);
  if (!std::isfinite(sig) || sig < sigma_min || sig > sigma_max) {
    return false;
  }
  *out_sigma = sig;
  return true;
}

enum class SigmaVolMode { Rolling, SlugFixed, Constant };

static inline double poly_taker_fee(double notional_usdc, double price, double fee_rate) {
  // Polymarket (crypto taker) fee:
  // fee = C * feeRate * p * (1-p), rounded to 5 decimals, min 0.00001
  // Here C is notional in USDC (e.g., qty * price).
  double fee = notional_usdc * fee_rate * price * (1.0 - price);
  fee = std::round(fee * 1e5) / 1e5;
  return (fee >= 1e-5) ? fee : 0.0;
}

#ifdef LL_ENABLE_LIVE_TRADER
static void sleep_ms(std::int64_t ms) {
  if (ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }
}

static std::string clob_order_status_upper(const nlohmann::json& order) {
  std::string st;
  if (order.contains("status") && order["status"].is_string()) {
    st = order["status"].get<std::string>();
  } else if (order.contains("order_status") && order["order_status"].is_string()) {
    st = order["order_status"].get<std::string>();
  }
  for (char& c : st) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return st;
}

static bool clob_order_status_terminal(const std::string& st_upper) {
  return st_upper == "MATCHED" || st_upper == "CANCELED" || st_upper == "CANCELLED" ||
         st_upper == "FILLED" || st_upper == "EXECUTED";
}

static bool clob_json_to_double(const nlohmann::json& v, double* out) {
  if (!out) {
    return false;
  }
  if (v.is_number()) {
    *out = v.get<double>();
    return std::isfinite(*out);
  }
  if (v.is_string()) {
    try {
      *out = std::stod(v.get<std::string>());
      return std::isfinite(*out);
    } catch (...) {
      return false;
    }
  }
  return false;
}

/// CLOB fixed-point amounts (USDC / shares) are often scaled by 1e6.
static double clob_decode_amount(double raw) {
  if (!std::isfinite(raw)) {
    return raw;
  }
  if (raw >= 1.0e4 && std::fabs(raw - std::round(raw)) < 1e-6) {
    return raw / 1.0e6;
  }
  return raw;
}

static bool clob_json_amount_field(const nlohmann::json& obj, const char* key, double* out) {
  if (!obj.contains(key)) {
    return false;
  }
  double raw = 0.0;
  if (!clob_json_to_double(obj.at(key), &raw)) {
    return false;
  }
  *out = clob_decode_amount(raw);
  return std::isfinite(*out) && *out >= 0.0;
}

static bool clob_order_size_matched_shares(const nlohmann::json& order, double* out_shares) {
  if (!out_shares) {
    return false;
  }
  double sm = 0.0;
  if (!clob_json_amount_field(order, "size_matched", &sm) &&
      !clob_json_amount_field(order, "sizeMatched", &sm)) {
    return false;
  }
  if (sm <= 0.0) {
    return false;
  }
  *out_shares = sm;
  return true;
}

static bool try_market_buy_fill_from_post_resp(const nlohmann::json& submit_top, double* out_cost_usd,
                                               double* out_shares) {
  const nlohmann::json* body = nullptr;
  if (submit_top.contains("resp") && submit_top["resp"].is_object()) {
    body = &submit_top["resp"];
  } else if (submit_top.is_object()) {
    body = &submit_top;
  }
  if (!body) {
    return false;
  }
  double making = 0.0;
  double taking = 0.0;
  const bool have_making = clob_json_amount_field(*body, "makingAmount", &making) ||
                           clob_json_amount_field(*body, "making_amount", &making);
  const bool have_taking = clob_json_amount_field(*body, "takingAmount", &taking) ||
                           clob_json_amount_field(*body, "taking_amount", &taking);
  if (!have_making || !have_taking || making <= 0.0 || taking <= 0.0) {
    return false;
  }
  // BUY: pay USDC (making), receive outcome shares (taking). Swap if fields appear reversed.
  double cost = making;
  double shares = taking;
  if (cost <= 1.0 && shares > 1.0) {
    std::swap(cost, shares);
  }
  if (shares <= 0.0 || cost <= 0.0) {
    return false;
  }
  const double px = cost / shares;
  if (px < 0.001 || px > 0.999) {
    std::swap(cost, shares);
    if (shares <= 0.0 || cost <= 0.0) {
      return false;
    }
    const double px2 = cost / shares;
    if (px2 < 0.001 || px2 > 0.999) {
      return false;
    }
  }
  *out_cost_usd = cost;
  *out_shares = shares;
  return true;
}

struct LiveMarketBuyFill {
  bool ok{false};
  double fill_qty{0.0};
  double fill_avg_price{0.0};
  double cost_usd{0.0};
  double fee_usd{0.0};
  bool fill_price_estimated{false};
  std::string last_error;
};

static LiveMarketBuyFill live_resolve_market_buy_fill(ll::execution::LiveExecutor& live_ex,
                                                      const std::string& token_id,
                                                      const std::string& order_id,
                                                      double balance_before_shares,
                                                      double spend_target_usd, double signal_ask,
                                                      double fee_rate,
                                                      const nlohmann::json& submit_resp) {
  constexpr std::int64_t kOrderPollMs = 150;
  constexpr int kMaxOrderPolls = 20;
  constexpr double kShareEps = 1e-6;

  LiveMarketBuyFill out;
  double cost_usd = std::numeric_limits<double>::quiet_NaN();
  double fill_qty = std::numeric_limits<double>::quiet_NaN();

  if (try_market_buy_fill_from_post_resp(submit_resp, &cost_usd, &fill_qty)) {
    out.fill_price_estimated = false;
  }

  if (!order_id.empty()) {
    for (int pi = 0; pi < kMaxOrderPolls; ++pi) {
      nlohmann::json order;
      std::string oerr;
      if (live_ex.query_order(order_id, &order, &oerr, nullptr)) {
        const std::string st = clob_order_status_upper(order);
        double matched_shares = 0.0;
        const bool have_matched = clob_order_size_matched_shares(order, &matched_shares);
        if (have_matched && matched_shares > kShareEps) {
          if (!std::isfinite(fill_qty) || fill_qty <= 0.0) {
            fill_qty = matched_shares;
          }
          if (!std::isfinite(cost_usd) || cost_usd <= 0.0) {
            double order_px = 0.0;
            if (clob_json_amount_field(order, "price", &order_px) && order_px > 0.0 &&
                order_px < 1.0) {
              cost_usd = fill_qty * order_px;
              out.fill_price_estimated = true;
            }
          }
        }
        if (clob_order_status_terminal(st) && have_matched) {
          break;
        }
        if (clob_order_status_terminal(st) && !have_matched) {
          break;
        }
      } else if (!oerr.empty()) {
        out.last_error = oerr;
      }
      sleep_ms(kOrderPollMs);
    }
  } else {
    sleep_ms(kOrderPollMs);
  }

  double balance_after = balance_before_shares;
  std::string qerr;
  if (live_ex.query_conditional_balance(token_id, &balance_after, &qerr, nullptr)) {
    const double delta = balance_after - balance_before_shares;
    if (delta > kShareEps) {
      fill_qty = delta;
    }
  } else if (!qerr.empty()) {
    out.last_error = qerr;
  }

  if (!std::isfinite(fill_qty) || fill_qty <= kShareEps) {
    if (out.last_error.empty()) {
      out.last_error = "no shares filled (balance delta and order match empty)";
    }
    return out;
  }

  if (!std::isfinite(cost_usd) || cost_usd <= 0.0) {
    if (spend_target_usd > 0.0 && signal_ask > 0.0) {
      const double expected_qty = spend_target_usd / signal_ask;
      if (expected_qty > 0.0 && fill_qty >= expected_qty * 0.98) {
        cost_usd = spend_target_usd;
        out.fill_price_estimated = true;
      } else {
        cost_usd = fill_qty * signal_ask;
        out.fill_price_estimated = true;
      }
    } else {
      out.last_error = "cannot infer buy cost (missing CLOB amounts)";
      return out;
    }
  }

  out.fill_qty = fill_qty;
  out.cost_usd = cost_usd;
  out.fill_avg_price = cost_usd / fill_qty;
  out.fee_usd = poly_taker_fee(cost_usd, out.fill_avg_price, fee_rate);
  out.ok = true;
  return out;
}

struct StopSellUntilFlatResult {
  bool flat{false};
  double total_proceeds{0.0};
  double total_fee{0.0};
  double final_balance{0.0};
  double last_sell_qty{0.0};
  int attempts{0};
  std::string last_error;
};

static StopSellUntilFlatResult live_stop_sell_until_flat(ll::execution::LiveExecutor& live_ex,
                                                        const std::string& token_id,
                                                        double mark_bid, double fee_rate) {
  constexpr int kMaxAttempts = 30;
  constexpr std::int64_t kRetryMs = 300;
  constexpr std::int64_t kOrderPollMs = 150;
  constexpr int kMaxOrderPolls = 15;
  constexpr double kFlatEps = 1e-6;

  StopSellUntilFlatResult out;
  const double worst_px = std::max(0.01, mark_bid);

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    out.attempts = attempt + 1;
    double bal = 0.0;
    std::string qerr;
    if (!live_ex.query_conditional_balance(token_id, &bal, &qerr, nullptr)) {
      out.last_error = qerr;
      sleep_ms(kRetryMs);
      continue;
    }
    out.final_balance = bal;
    if (bal <= kFlatEps) {
      out.flat = true;
      return out;
    }

    const double bal_before = bal;
    ll::execution::OrderIntent oi;
    oi.market_token_id = token_id;
    oi.side = "SELL";
    oi.market_order = true;
    oi.market_order_type = "FAK";
    oi.market_worst_price = worst_px;
    oi.qty = bal_before;

    std::string errmsg;
    std::string oid;
    if (!live_ex.submit(oi, &errmsg, &oid, nullptr)) {
      out.last_error = errmsg;
      sleep_ms(kRetryMs);
      continue;
    }

    if (!oid.empty()) {
      for (int pi = 0; pi < kMaxOrderPolls; ++pi) {
        nlohmann::json order;
        std::string oerr;
        if (live_ex.query_order(oid, &order, &oerr, nullptr)) {
          if (clob_order_status_terminal(clob_order_status_upper(order))) {
            break;
          }
        }
        sleep_ms(kOrderPollMs);
      }
    } else {
      sleep_ms(kOrderPollMs);
    }

    double bal_after = bal_before;
    if (!live_ex.query_conditional_balance(token_id, &bal_after, &qerr, nullptr)) {
      out.last_error = qerr;
      sleep_ms(kRetryMs);
      continue;
    }
    out.final_balance = bal_after;
    const double sold = std::max(0.0, bal_before - bal_after);
    out.last_sell_qty = sold;
    if (sold > kFlatEps) {
      const double proceeds = sold * mark_bid;
      out.total_proceeds += proceeds;
      out.total_fee += poly_taker_fee(proceeds, mark_bid, fee_rate);
    }
    if (bal_after <= kFlatEps) {
      out.flat = true;
      return out;
    }
    sleep_ms(kRetryMs);
  }
  return out;
}
#endif

bool parse_epoch_from_confirmed_slug(const std::string& slug, std::int64_t* out_epoch) {
  static constexpr char kPrefix[] = "btc-updown-5m-";
  constexpr std::size_t plen = sizeof(kPrefix) - 1;
  if (slug.size() <= plen) return false;
  for (std::size_t i = 0; i < plen; ++i) {
    if (std::tolower(static_cast<unsigned char>(slug[i])) !=
        static_cast<unsigned char>(kPrefix[i])) {
      return false;
    }
  }
  try {
    *out_epoch = std::stoll(slug.substr(plen));
    return true;
  } catch (...) {
    return false;
  }
}

int run_live_impl(bool live_execution, int argc, char** argv) {
  load_dotenv();

  std::signal(SIGINT, on_sig);
  std::signal(SIGTERM, on_sig);

  const std::string src_tag = live_execution ? "live_trader" : "paper_trader";
  const char* log_pfx = live_execution ? "[live]" : "[paper]";

  if (live_execution) {
    const char* rc = std::getenv("POLY_REQUIRE_CONFIRM");
    if (rc && rc[0]) {
      ll::io::SyncCerrLock _;
      std::cerr << "[live_trader] warning: POLY_REQUIRE_CONFIRM is set; each submit may be refused. "
                   "Unset it for automated --strategy mode.\n";
    }
  }

  std::string out_prefix = live_execution ? "live" : "paper";
  bool disable_rollover_force_sell = false;
  if (const char* v = std::getenv("POLY_DISABLE_ROLLOVER_FORCE_SELL")) {
    if (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y') {
      disable_rollover_force_sell = true;
    }
  }

  // Defaults: align with data/backtest.ipynb cell 5–7 (override via CLI).
  const double r = 0.035;
  const double sigma_rv_min = 0.05;  // realized/GARCH estimate must be in range to accept
  const double sigma_rv_max = 5.0;
  double sigma_clamp_min = 0.15;  // theo pricing: clamp final σ to [min, max]
  double sigma_clamp_max = 0.2;
  double sigma_fallback = 0.15;
  std::int64_t sigma_step_ms = 300;
  /// `slug_fixed` (default): σ at bucket anchor, constant for the 5m slug (backtest SIGMA_MODE=slug_fixed).
  /// `rolling` (`realized`): re-estimate σ on every spot tick (1h window ending at current wall).
  SigmaVolMode sigma_vol_mode = SigmaVolMode::SlugFixed;
  /// GARCH(1,1) at bucket σ init only (`--sigma-model garch`); fallback to realized on failure.
  bool sigma_try_garch = false;

  double initial_cash = 100.0;
  double risk_frac = 0.01;
  double entry = 0.15;  // theo - ask; align with data/backtest.ipynb ENTRY_DELTA_EXEC
  /// SELL when bid >= theo - close_eps - close_early_threshold (default 0 matches notebook: bid >= theo).
  double close_eps = 0.0;
  /// Exit earlier: extra slack below theo; 0 matches backtest CLOSE_EARLY_THRESHOLD.
  double close_early_threshold = 0.0;
  /// BUY only when local-wall seconds since bucket start in [min, max] (backtest ENTRY_ELAPSED_*).
  bool use_entry_elapsed_window = true;
  double entry_elapsed_min_sec = 260.0;
  double entry_elapsed_max_sec = 300.0;
  /// BUY: `theo-ask >= entry` must hold continuously for this many ms (steady clock); 0 = off.
  int edge_persist_ms = 500;  // EDGE_PERSIST_MS in backtest.ipynb
  int lat_ms = 0;             // LAT_MS in backtest.ipynb
  double fee_rate = 0.0;      // POLY_TAKER_FEE_RATE in backtest.ipynb
  /// Unrealized loss vs pos_cost_basis (incl. buy fee); <=0 disables (backtest STOP_LOSS_FRAC).
  double stop_loss_frac = 0.4;
  double max_loss_usd = std::numeric_limits<double>::infinity();
  /// Each entry BUY uses this USDC notional (backtest TRADE_DOLLARS=1); 0 falls back to --risk-frac.
  double fixed_spend_usd = 1.0;
  /// BUY only when chosen outcome's best ask is in [buy_ask_min, buy_ask_max] (ask must be > 0).
  bool use_buy_ask_range = true;
  double buy_ask_min = 0.6;
  double buy_ask_max = 0.9;

  bool poly_discover = true;
  bool poly_rollover_web_ptb = false;
  std::string poly_manual_token;
  std::string poly_event_slug;
  std::size_t poly_parse_workers = 0;
  /// When true, spot `S` + realized-vol history come from Polymarket RTDS Chainlink (`btc/usd`), not Binance.
  bool spot_feed_chainlink = true;
  /// Consumer-thread spot path timing + queue depth (stderr + periodic summary).
  bool spot_latency_monitor = false;
  double spot_latency_report_sec = 5.0;
  if (const char* slm = std::getenv("LL_SPOT_LATENCY_MONITOR")) {
    if (slm[0] == '1' || slm[0] == 't' || slm[0] == 'T' || slm[0] == 'y' || slm[0] == 'Y') {
      spot_latency_monitor = true;
    }
  }

  ll::binance::StreamClientConfig bin_cfg;
  ll::binance::apply_stream_env_overrides(bin_cfg);

  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--stream" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "trade") {
        bin_cfg.stream_path = "/ws/btcusdt@trade";
      } else if (m == "bookTicker") {
        bin_cfg.stream_path = "/ws/btcusdt@bookTicker";
      } else {
        { ll::io::SyncCerrLock _; std::cerr << "--stream must be trade or bookTicker\n"; }
        return 2;
      }
    } else if (a == "--binance-stream" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "trade") {
        bin_cfg.stream_path = "/ws/btcusdt@trade";
      } else if (m == "bookTicker") {
        bin_cfg.stream_path = "/ws/btcusdt@bookTicker";
      } else {
        { ll::io::SyncCerrLock _; std::cerr << "--binance-stream must be trade or bookTicker\n"; }
        return 2;
      }
    } else if (a == "--host" && i + 1 < argc) {
      bin_cfg.ws_host = argv[++i];
    } else if (a == "--port" && i + 1 < argc) {
      bin_cfg.ws_port = std::stoi(argv[++i]);
    } else if (a == "--parse-workers" && i + 1 < argc) {
      bin_cfg.parse_workers = std::stoi(argv[++i]);
    } else if (a == "--poly-parse-workers" && i + 1 < argc) {
      poly_parse_workers = static_cast<std::size_t>(std::stoul(argv[++i]));
    } else if (a == "--spot-feed" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "chainlink" || m == "cl" || m == "rtds") {
        spot_feed_chainlink = true;
      } else if (m == "binance" || m == "bin") {
        spot_feed_chainlink = false;
      } else {
        { ll::io::SyncCerrLock _; std::cerr << "--spot-feed must be chainlink or binance\n"; }
        return 2;
      }
    } else if (a == "--out-prefix" && i + 1 < argc) {
      out_prefix = argv[++i];
    } else if (a == "--sigma" && i + 1 < argc) {
      sigma_fallback = std::stod(argv[++i]);
    } else if (a == "--sigma-clamp-min" && i + 1 < argc) {
      sigma_clamp_min = std::stod(argv[++i]);
    } else if (a == "--sigma-clamp-max" && i + 1 < argc) {
      sigma_clamp_max = std::stod(argv[++i]);
    } else if (a == "--sigma-step-ms" && i + 1 < argc) {
      sigma_step_ms = std::stoll(argv[++i]);
    } else if (a == "--sigma-model" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "garch" || m == "GARCH") {
        sigma_try_garch = true;
        sigma_vol_mode = SigmaVolMode::SlugFixed;
      } else if (m == "realized" || m == "rv" || m == "rolling") {
        sigma_try_garch = false;
        sigma_vol_mode = SigmaVolMode::Rolling;
      } else if (m == "slug_fixed" || m == "fixed") {
        sigma_try_garch = false;
        sigma_vol_mode = SigmaVolMode::SlugFixed;
      } else if (m == "constant") {
        sigma_try_garch = false;
        sigma_vol_mode = SigmaVolMode::Constant;
      } else {
        { ll::io::SyncCerrLock _;
          std::cerr << "--sigma-model must be slug_fixed|fixed|rolling|realized|rv|constant|garch\n"; }
        return 2;
      }
    } else if (a == "--initial-cash" && i + 1 < argc) {
      initial_cash = std::stod(argv[++i]);
    } else if (a == "--risk-frac" && i + 1 < argc) {
      risk_frac = std::stod(argv[++i]);
    } else if (a == "--entry" && i + 1 < argc) {
      entry = std::stod(argv[++i]);
    } else if (a == "--close" && i + 1 < argc) {
      close_eps = std::stod(argv[++i]);
    } else if (a == "--close-early-threshold" && i + 1 < argc) {
      close_early_threshold = std::stod(argv[++i]);
      if (close_early_threshold < 0.0) {
        { ll::io::SyncCerrLock _; std::cerr << "--close-early-threshold must be >= 0\n"; }
        return 2;
      }
    } else if (a == "--lat-ms" && i + 1 < argc) {
      lat_ms = std::stoi(argv[++i]);
    } else if (a == "--fee-rate" && i + 1 < argc) {
      fee_rate = std::stod(argv[++i]);
    } else if (a == "--stop-loss-frac" && i + 1 < argc) {
      stop_loss_frac = std::stod(argv[++i]);
      if (stop_loss_frac < 0.0) {
        { ll::io::SyncCerrLock _; std::cerr << "--stop-loss-frac must be >= 0 (0 disables)\n"; }
        return 2;
      }
    } else if (a == "--max-loss-usd" && i + 1 < argc) {
      max_loss_usd = std::stod(argv[++i]);
    } else if (a == "--fixed-spend-usd" && i + 1 < argc) {
      fixed_spend_usd = std::stod(argv[++i]);
      if (fixed_spend_usd < 0.0) {
        { ll::io::SyncCerrLock _; std::cerr << "--fixed-spend-usd must be >= 0\n"; }
        return 2;
      }
    } else if (a == "--no-entry-elapsed-window") {
      use_entry_elapsed_window = false;
    } else if (a == "--entry-elapsed-min-sec" && i + 1 < argc) {
      entry_elapsed_min_sec = std::stod(argv[++i]);
    } else if (a == "--entry-elapsed-max-sec" && i + 1 < argc) {
      entry_elapsed_max_sec = std::stod(argv[++i]);
    } else if (a == "--edge-persist-ms" && i + 1 < argc) {
      edge_persist_ms = std::stoi(argv[++i]);
      if (edge_persist_ms < 0) {
        { ll::io::SyncCerrLock _; std::cerr << "--edge-persist-ms must be >= 0 (0 disables)\n"; }
        return 2;
      }
    } else if (a == "--buy-ask-min" && i + 1 < argc) {
      buy_ask_min = std::stod(argv[++i]);
    } else if (a == "--buy-ask-max" && i + 1 < argc) {
      buy_ask_max = std::stod(argv[++i]);
    } else if (a == "--no-buy-ask-range") {
      use_buy_ask_range = false;
    } else if (a == "--poly-token" && i + 1 < argc) {
      poly_discover = false;
      poly_manual_token = argv[++i];
    } else if (a == "--poly-event-slug" && i + 1 < argc) {
      poly_event_slug = argv[++i];
    } else if (a == "--poly-discover") {
      poly_discover = true;
    } else if (a == "--poly-rollover-web-ptb") {
      poly_rollover_web_ptb = true;
    } else if (a == "--disable-rollover-force-sell") {
      disable_rollover_force_sell = true;
    } else if (a == "--spot-latency-monitor") {
      spot_latency_monitor = true;
    } else if (a == "--spot-latency-report-sec" && i + 1 < argc) {
      spot_latency_report_sec = std::stod(argv[++i]);
      if (spot_latency_report_sec < 0.25) {
        { ll::io::SyncCerrLock _; std::cerr << "--spot-latency-report-sec must be >= 0.25\n"; }
        return 2;
      }
    } else if (a == "--help") {
      ll::io::SyncCerrLock _;
      std::cerr << "usage: paper_trader --live [options]   |   live_trader --strategy [options]\n"
                   "  --initial-cash X\n"
                   "  --risk-frac F              (default 0.01)\n"
                   "  --entry X                  (theo-ask; default 0.15 per backtest.ipynb)\n"
                   "  --close X                  (SELL when bid >= theo - X - close_early_threshold; default X=0)\n"
                   "  --close-early-threshold Y  (extra slack vs theo; default 0 per backtest.ipynb)\n"
                   "  --lat-ms N                 (BUY/SELL delay; default 0 per backtest.ipynb)\n"
                   "  --fee-rate R               (default 0 per backtest.ipynb POLY_TAKER_FEE_RATE)\n"
                   "  --stop-loss-frac F         (exit when (bid proceeds - cost_basis)/cost_basis <= -F; default 0.4)\n"
                   "  --max-loss-usd L           stop opening new BUY when mark-to-market loss >= L\n"
                   "                             (vs --initial-cash baseline; default: no cap)\n"
                   "  --fixed-spend-usd X        each BUY uses X USDC notional (default 1; 0 uses --risk-frac)\n"
                   "  --entry-elapsed-min-sec S  (with max; default 260; local wall sec since bucket start)\n"
                   "  --entry-elapsed-max-sec S  (default 300; inclusive)\n"
                   "  --edge-persist-ms N        BUY: theo-ask>=entry must hold N ms steady time (default 500;\n"
                   "                             0 off). Matches backtest.ipynb EDGE_PERSIST_MS.\n"
                   "  --no-entry-elapsed-window  allow BUY any time in bucket (disables the above)\n"
                   "  --buy-ask-min P            (with max; default 0.6 per backtest.ipynb)\n"
                   "  --buy-ask-max P            (default 0.9)\n"
                   "  --no-buy-ask-range         allow BUY at any ask (restores min-ask 0.02 guard only)\n"
                   "  --sigma S                  (fallback when bucket sigma missing)\n"
                   "  --sigma-clamp-min S        (clamp σ used in theo; default 0.15)\n"
                   "  --sigma-clamp-max S        (default 0.2)\n"
                   "  --sigma-step-ms N          (resample step for GARCH input + realized fallback)\n"
                   "  --sigma-model slug_fixed|fixed|rolling|realized|rv|constant|garch\n"
                   "                             (default slug_fixed: σ at bucket start, constant per slug;\n"
                   "                              constant: fixed --sigma for entire run (backtest SIGMA_MODE=constant);\n"
                   "                              rolling/realized/rv: 1h window updates each spot tick;\n"
                   "                              garch: slug_fixed + GARCH at bucket open via poly_daemon)\n"
                   "  --spot-feed chainlink|binance   spot price for theo S + vol history (default: chainlink RTDS)\n"
                   "  env CHAINLINK_PREFILL_JSONL     extra jsonl for σ prefill (payload_ts_ms/price)\n"
                   "  --host/--port/--parse-workers/--stream ... (binance; only if --spot-feed binance)\n"
                   "  --poly-parse-workers N     (polymarket json parse workers; default 0)\n"
                   "  --out-prefix NAME          (under data/: *_trades.jsonl, *_series.jsonl; each process start\n"
                   "                             deletes+recreates those files for a fresh session;\n"
                   "                             with --spot-feed chainlink also *_chainlink.jsonl ticks)\n"
                   "  --poly-token TOKEN         (manual fixed token; pair with --poly-event-slug; dual WS)\n"
                   "  --poly-event-slug SLUG     required with --poly-token (Gamma btc-updown-5m-<epoch> slug)\n"
                   "  --poly-rollover-web-ptb    warmup + each rollover: strike from polymarket.com event page\n"
                   "                             first; fallback RTDS Chainlink if fetch fails\n"
                   "  --disable-rollover-force-sell  do not auto FORCE_SELL on rollover\n"
                   "                               (also env POLY_DISABLE_ROLLOVER_FORCE_SELL=1)\n"
                   "  --spot-latency-monitor       log spot handler latency + queue depth (stderr)\n"
                   "                               (also env LL_SPOT_LATENCY_MONITOR=1)\n"
                   "  --spot-latency-report-sec S  summary interval (default 5)\n";
      return 0;
    } else {
      { ll::io::SyncCerrLock _; std::cerr << "unknown arg: " << a << "\n"; }
      return 2;
    }
  }

  if (use_entry_elapsed_window && entry_elapsed_min_sec > entry_elapsed_max_sec) {
    { ll::io::SyncCerrLock _; std::cerr << "--entry-elapsed-min-sec must be <= --entry-elapsed-max-sec\n"; }
    return 2;
  }

  if (use_buy_ask_range) {
    if (!std::isfinite(buy_ask_min) || !std::isfinite(buy_ask_max) || buy_ask_min < 0.0 || buy_ask_max > 1.0 ||
        buy_ask_min > buy_ask_max) {
      { ll::io::SyncCerrLock _; std::cerr << "--buy-ask-min/--buy-ask-max invalid (expect 0<=min<=max<=1)\n"; }
      return 2;
    }
  }

  const std::int64_t edge_persist_ns =
      edge_persist_ms > 0 ? static_cast<std::int64_t>(edge_persist_ms) * 1'000'000LL : 0LL;

#ifndef LL_ENABLE_GARCH_DAEMON
  if (sigma_try_garch) {
    ll::io::SyncCerrLock _;
    std::cerr << "[" << src_tag << "] GARCH disabled in this build; using realized vol only "
                 "(configure with -DBUILD_DAEMON_GARCH=ON or -DBUILD_LIVE_TRADER=ON)\n";
  }
#endif

  if (!poly_discover) {
    if (poly_event_slug.empty()) {
      { ll::io::SyncCerrLock _; std::cerr << "--poly-token requires --poly-event-slug for Gamma priceToBeat\n"; }
      return 2;
    }
  }

  const std::string trades_path = "data/" + out_prefix + "_trades.jsonl";
  const std::string series_path = "data/" + out_prefix + "_series.jsonl";
  const std::string cl_path = "data/" + out_prefix + "_chainlink.jsonl";

  HotState hot;
  if (spot_feed_chainlink) {
    std::vector<std::string> cl_prefill_paths;
    if (const char* envp = std::getenv("CHAINLINK_PREFILL_JSONL")) {
      if (envp[0]) {
        cl_prefill_paths.emplace_back(envp);
      }
    }
    cl_prefill_paths.emplace_back("data/chainlink_price.jsonl");
    cl_prefill_paths.emplace_back(cl_path);
    prefill_bin_hist_from_chainlink_jsonl(hot.bin_hist, cl_prefill_paths, log_pfx);
  } else {
    prefill_bin_hist(hot.bin_hist, bin_cfg);
  }

  // 每次启动只保留本会话：先删旧文件再 trunc 打开（trades / series / chainlink ticks）。
  reset_session_jsonl_file(trades_path);
  reset_session_jsonl_file(series_path);
  TradeJsonlWriter trades_writer(trades_path);
  if (trades_writer.ok()) {
    { ll::io::SyncCerrLock _; std::cerr << "[" << src_tag << "] recording trades to " << trades_writer.path()
                                        << " (fresh session)\n"; }
  }
  TradeJsonlWriter series_writer(series_path);
  if (series_writer.ok()) {
    { ll::io::SyncCerrLock _; std::cerr << "[" << src_tag << "] recording series to " << series_writer.path()
                                        << " (fresh session)\n"; }
  }
  {
    ll::io::SyncCerrLock _;
    if (use_entry_elapsed_window) {
      std::cerr << "[" << src_tag << "] BUY entry window (local wall vs bucket): [" << entry_elapsed_min_sec << ", "
                << entry_elapsed_max_sec << "] s (backtest.ipynb cell 10)\n";
    } else {
      std::cerr << "[" << src_tag << "] BUY entry window: disabled\n";
    }
    if (use_buy_ask_range) {
      std::cerr << "[" << src_tag << "] BUY ask range: [" << buy_ask_min << ", " << buy_ask_max << "] (ask>0)\n";
    } else {
      std::cerr << "[" << src_tag << "] BUY ask range: disabled (min ask 0.02 for entry)\n";
    }
    if (edge_persist_ns > 0) {
      std::cerr << "[" << src_tag << "] BUY edge persist: " << edge_persist_ms
                << " ms steady (backtest.ipynb EDGE_PERSIST_MS)\n";
    } else {
      std::cerr << "[" << src_tag << "] BUY edge persist: off\n";
    }
    if (fixed_spend_usd > 0.0) {
      std::cerr << "[" << src_tag << "] position size: fixed $" << fixed_spend_usd << " per BUY\n";
    } else {
      std::cerr << "[" << src_tag << "] position size: risk_frac=" << risk_frac << "\n";
    }
  {
    const char* sigma_mode_str = "rolling (1h realized each tick)";
    if (sigma_vol_mode == SigmaVolMode::SlugFixed) {
      sigma_mode_str = "slug_fixed (per slug, bucket anchor)";
    } else if (sigma_vol_mode == SigmaVolMode::Constant) {
      sigma_mode_str = "constant (--sigma)";
    }
    std::cerr << "[" << src_tag << "] sigma vol mode: " << sigma_mode_str
              << (sigma_try_garch ? " + GARCH at bucket init" : "") << "\n";
    std::cerr << "[" << src_tag << "] sigma theo clamp: [" << sigma_clamp_min << ", "
              << sigma_clamp_max << "] (fallback=" << sigma_fallback << ")\n";
  }
  if (sigma_clamp_min <= 0.0 || sigma_clamp_max <= 0.0 || sigma_clamp_min > sigma_clamp_max) {
    { ll::io::SyncCerrLock _;
      std::cerr << "sigma clamp range invalid: need 0 < sigma-clamp-min <= sigma-clamp-max\n"; }
    return 2;
  }
  sigma_fallback = clamp_sigma(sigma_fallback, sigma_clamp_min, sigma_clamp_max);
    if (stop_loss_frac > 0.0) {
      std::cerr << "[" << src_tag << "] stop loss: unrealized PnL / cost_basis <= -" << stop_loss_frac
                << "; exit uses FAK + retry until CLOB balance=0\n";
    }
    std::cerr << "[" << src_tag << "] SELL when bid >= theo - close_eps - close_early_threshold"
              << " (close_eps=" << close_eps << " close_early_threshold=" << close_early_threshold << ")\n";
  }
  std::unique_ptr<TradeJsonlWriter> chainlink_ticks_writer;
  std::atomic<std::uint64_t> chainlink_tick_seq{0};
  if (spot_feed_chainlink) {
    reset_session_jsonl_file(cl_path);
    chainlink_ticks_writer = std::make_unique<TradeJsonlWriter>(cl_path);
    if (chainlink_ticks_writer->ok()) {
      { ll::io::SyncCerrLock _; std::cerr << "[" << src_tag << "] recording Chainlink ticks to "
                                          << chainlink_ticks_writer->path() << " (fresh session)\n"; }
    }
  }
  std::int64_t last_series_mono_ns = 0;
  // Series jsonl for dashboards: at most once per this interval (steady clock).
  constexpr std::int64_t kSeriesEveryNs = 100 * 1000 * 1000;  // 100ms
  constexpr int kSeriesIdlePollMs = 100;  // wake consumer to sample series when event queue is quiet

  ll::telemetry::Pipeline tel;
  std::unique_ptr<ll::binance::StreamClient> bin_client;
  if (!spot_feed_chainlink) {
    bin_client = std::make_unique<ll::binance::StreamClient>(&tel);
  }
  ll::polymarket::WsFixedTokenQuoteFeed feed_up(&tel);
  ll::polymarket::WsFixedTokenQuoteFeed feed_down(&tel);
  feed_up.set_parse_workers(poly_parse_workers);
  feed_down.set_parse_workers(poly_parse_workers);

  ll::execution::LiveExecutor live_ex;

  std::unique_ptr<TradeJsonlWriter> poly_daemon_traffic_writer;
  if (live_execution) {
    bool disable_poly_daemon_log = false;
    if (const char* d = std::getenv("POLY_DISABLE_DAEMON_TRAFFIC_LOG")) {
      if (d[0] == '1' || d[0] == 't' || d[0] == 'T' || d[0] == 'y' || d[0] == 'Y') {
        disable_poly_daemon_log = true;
      }
    }
    if (!disable_poly_daemon_log) {
      std::string poly_daemon_path = "data/" + out_prefix + "_poly_daemon.jsonl";
      if (const char* e = std::getenv("POLY_DAEMON_TRAFFIC_JSONL")) {
        if (e[0] != '\0') {
          poly_daemon_path = e;
        }
      }
      poly_daemon_traffic_writer = std::make_unique<TradeJsonlWriter>(poly_daemon_path);
      if (poly_daemon_traffic_writer->ok()) {
        live_ex.set_poly_daemon_traffic_log(
            [src_tag, w = poly_daemon_traffic_writer.get()](const nlohmann::json& row) {
              nlohmann::json r = row;
              r["runner_source"] = src_tag;
              w->append(r);
            });
        { ll::io::SyncCerrLock _;
          std::cerr << "[" << src_tag << "] recording poly_daemon I/O to " << poly_daemon_traffic_writer->path()
                    << " (truncated)\n";
        }
      }
    }
  }

  {
    ll::io::SyncCerrLock _;
    if (!spot_feed_chainlink) {
      std::cerr << "[" << src_tag << "][binance_ws] connecting to wss://" << bin_cfg.ws_host << ':'
                << bin_cfg.ws_port << bin_cfg.stream_path << "\n";
    } else {
      std::cerr << "[" << src_tag << "][spot] Polymarket RTDS Chainlink btc/usd for S + vol (no Binance WS)\n";
    }
    if (spot_latency_monitor) {
      std::cerr << "[" << src_tag << "] spot_latency_monitor=1 report_sec=" << spot_latency_report_sec
                << " (LL_SPOT_LATENCY_MONITOR / --spot-latency-monitor)\n";
    }
  }

  std::mutex print_mu;
  std::mutex paper_mu;

  // Single-thread consumer queue: callbacks only enqueue, strategy runs in one thread.
  struct Event {
    enum class Type { BinBook, BinTrade, ChainlinkSpot, PolyQuote, Rollover, Stop } type;
    ll::core::BookTickerTick book{};
    ll::core::TradeTick trade{};
    std::int64_t chainlink_ts_ms{0};
    double chainlink_px{0.0};
    ll::polymarket::PolymarketWsQuote quote{};
    std::int64_t rollover_epoch{-1};
    std::string rollover_slug;
    // Snapshot of OLD slug quotes at rollover time (for FORCE_SELL pricing).
    double rollover_up_bid{0.0};
    double rollover_up_ask{0.0};
    double rollover_down_bid{0.0};
    double rollover_down_ask{0.0};
    double rollover_price_to_beat{0.0};
  };
  std::mutex ev_mu;
  std::condition_variable ev_cv;
  std::queue<Event> ev_q;
  std::atomic<std::uint64_t> ev_q_push_depth_max_window{0};

  auto push_event = [&](Event&& e) {
    std::uint64_t depth_after = 0;
    {
      std::lock_guard<std::mutex> lk(ev_mu);
      ev_q.push(std::move(e));
      depth_after = static_cast<std::uint64_t>(ev_q.size());
    }
    std::uint64_t prev = ev_q_push_depth_max_window.load(std::memory_order_relaxed);
    while (depth_after > prev &&
           !ev_q_push_depth_max_window.compare_exchange_weak(prev, depth_after, std::memory_order_relaxed)) {
    }
    ev_cv.notify_one();
  };

  struct PaperState {
    bool have_pos{false};
    std::string side; // "Up"/"Down"
    std::string token_id;
    std::string slug;
    double qty{0.0};  // fractional shares allowed
    double pos_cost_basis{0.0};  // buy cost + fee (backtest pos_cost_basis)
    /// Actual average fill price per share (live: from CLOB; paper: signal ask).
    double entry_fill_price{0.0};
    double cash{0.0};
    bool pending{false};
    std::string pending_action; // BUY/SELL
    std::string pending_side;
    std::string pending_exit_reason;  // take_profit | stop_loss
    std::int64_t due_ns{0};
    bool risk_stop{false};
    std::int64_t cooldown_until_ns{0};
    /// Stop-loss FAK sell did not flatten; keep scheduling SELL until CLOB balance is 0.
    bool stop_sell_retry{false};
  } paper;
  paper.cash = initial_cash;
  constexpr std::int64_t kCooldownNs = 10'000'000'000LL;  // 10 seconds
  bool trading_enabled = false;  // skip the very first slug; enable after first rollover

  auto append_sigma_trade_fields = [&](nlohmann::json& row, std::int64_t active_epoch_sec,
                                     std::int64_t ref_wall_ms) {
    bool window_full = false;
    bool slug_sigma_ready = false;
    std::int64_t anchor_ms = ref_wall_ms;
    if (sigma_vol_mode == SigmaVolMode::SlugFixed && active_epoch_sec >= 0) {
      anchor_ms = active_epoch_sec * 1000LL;
    }
    {
      std::lock_guard<std::mutex> lk(hot.mu);
      slug_sigma_ready = hot.have_sigma;
      if (!hot.bin_hist.empty()) {
        window_full = hot.bin_hist.front().first <= anchor_ms - 3600 * 1000LL;
      }
    }
    row["sigma_vol_window_full"] = window_full;
    row["sigma_slug_ready"] = slug_sigma_ready;
    row["sigma_fallback"] = !slug_sigma_ready;
  };

  auto stop_loss_signal = [&](double bid, double qty) -> bool {
    if (stop_loss_frac <= 0.0 || paper.pos_cost_basis <= 0.0 || qty <= 0.0) {
      return false;
    }
    if (!std::isfinite(bid) || bid <= 0.0) {
      return false;
    }
    const double proceeds = qty * bid;
    const double fee = poly_taker_fee(proceeds, bid, fee_rate);
    const double pnl_frac = (proceeds - fee - paper.pos_cost_basis) / paper.pos_cost_basis;
    return pnl_frac <= -stop_loss_frac;
  };

  auto entry_elapsed_ok = [&](std::int64_t active_epoch_sec, std::int64_t entry_clock_wall_ms) -> bool {
    if (!use_entry_elapsed_window) {
      return true;
    }
    if (active_epoch_sec < 0) {
      return false;
    }
    const std::int64_t bucket_start_ms = active_epoch_sec * 1000;
    const double elapsed_sec =
        (static_cast<double>(entry_clock_wall_ms - bucket_start_ms)) / 1000.0;
    return elapsed_sec >= entry_elapsed_min_sec && elapsed_sec <= entry_elapsed_max_sec;
  };

  auto schedule = [&](double p_up, double p_dn, double up_ask, double up_bid, double dn_ask, double dn_bid) {
    std::lock_guard<std::mutex> lk(paper_mu);
    const auto now_ns = ll::core::steady_ns();
    const std::int64_t lat_ns = static_cast<std::int64_t>(lat_ms) * 1'000'000LL;
    if (paper.pending) return;
    if (paper.risk_stop) return;

    if (paper.have_pos) {
      std::lock_guard<std::mutex> hk(hot.mu);
      hot.edge_up_ok_since_mono_ns = -1;
      hot.edge_dn_ok_since_mono_ns = -1;
    }

    if (!paper.have_pos) {
      if (!trading_enabled) {
        std::lock_guard<std::mutex> hk(hot.mu);
        hot.edge_up_ok_since_mono_ns = -1;
        hot.edge_dn_ok_since_mono_ns = -1;
        return;
      }
      const double e_up = p_up - up_ask;
      const double e_dn = p_dn - dn_ask;
      auto buy_ask_ok = [&](double ask) -> bool {
        if (!std::isfinite(ask) || ask <= 0.0) return false;
        if (use_buy_ask_range) return ask >= buy_ask_min && ask <= buy_ask_max;
        return ask >= 0.02;
      };
      const bool edge_up_raw = (e_up >= entry) && buy_ask_ok(up_ask);
      const bool edge_dn_raw = (e_dn >= entry) && buy_ask_ok(dn_ask);

      bool up_ok = false;
      bool dn_ok = false;
      {
        std::lock_guard<std::mutex> hk(hot.mu);
        if (edge_up_raw) {
          if (hot.edge_up_ok_since_mono_ns < 0) hot.edge_up_ok_since_mono_ns = now_ns;
        } else {
          hot.edge_up_ok_since_mono_ns = -1;
        }
        if (edge_dn_raw) {
          if (hot.edge_dn_ok_since_mono_ns < 0) hot.edge_dn_ok_since_mono_ns = now_ns;
        } else {
          hot.edge_dn_ok_since_mono_ns = -1;
        }
        if (edge_up_raw && (edge_persist_ns <= 0LL || (now_ns - hot.edge_up_ok_since_mono_ns) >= edge_persist_ns)) {
          up_ok = true;
        }
        if (edge_dn_raw && (edge_persist_ns <= 0LL || (now_ns - hot.edge_dn_ok_since_mono_ns) >= edge_persist_ns)) {
          dn_ok = true;
        }
      }
      if (!up_ok && !dn_ok) return;
      std::int64_t ae = -1;
      std::int64_t ecw = 0;
      {
        std::lock_guard<std::mutex> hk(hot.mu);
        ae = hot.active_epoch;
        ecw = hot.bin_entry_clock_wall_ms;
      }
      if (!entry_elapsed_ok(ae, ecw)) return;
      paper.pending = true;
      paper.pending_action = "BUY";
      if (up_ok && dn_ok) {
        paper.pending_side = (e_up >= e_dn) ? "Up" : "Down";
      } else if (up_ok) {
        paper.pending_side = "Up";
      } else {
        paper.pending_side = "Down";
      }
      paper.due_ns = now_ns + lat_ns;
      return;
    }

    // backtest.ipynb: take_profit on bid >= theo - ...; stop_loss on unrealized loss vs cost_basis.
    const double close_floor_up = p_up - close_eps - close_early_threshold;
    const double close_floor_dn = p_dn - close_eps - close_early_threshold;
    bool stop = false;
    bool take_profit = false;
    if (paper.side == "Up") {
      stop = stop_loss_signal(up_bid, paper.qty);
      take_profit = up_bid >= close_floor_up;
    } else {
      stop = stop_loss_signal(dn_bid, paper.qty);
      take_profit = dn_bid >= close_floor_dn;
    }
    if (paper.stop_sell_retry) {
      stop = true;
    }
    if (!stop && !take_profit) {
      return;
    }
    paper.pending = true;
    paper.pending_action = "SELL";
    paper.pending_side = paper.side;
    paper.pending_exit_reason = (stop || paper.stop_sell_retry) ? "stop_loss" : "take_profit";
    paper.due_ns = now_ns + lat_ns;
  };

  auto execute_due = [&]() {
    std::lock_guard<std::mutex> lk(paper_mu);
    if (!paper.pending) return;
    const auto now_ns = ll::core::steady_ns();
    if (now_ns < paper.due_ns) return;

    std::string slug;
    std::int64_t active_epoch = -1;
    std::int64_t bin_wall_ms = 0;
    std::int64_t bin_entry_clock_wall_ms = 0;
    double S = 0.0, K = 0.0;
    bool have_sigma = false;
    double sigma_bucket = 0.0;
    double up_bid = 0, up_ask = 0, dn_bid = 0, dn_ask = 0;
    {
      std::lock_guard<std::mutex> lk(hot.mu);
      if (!hot.have_bin || !hot.have_K || !hot.have_up || !hot.have_down || hot.active_epoch < 0) {
        return;
      }
      slug = hot.slug;
      active_epoch = hot.active_epoch;
      bin_wall_ms = hot.bin_wall_ms;
      bin_entry_clock_wall_ms = hot.bin_entry_clock_wall_ms;
      S = hot.bin_mid;
      K = hot.K;
      have_sigma = hot.have_sigma;
      sigma_bucket = hot.sigma_bucket;
      up_bid = hot.up_bid;
      up_ask = hot.up_ask;
      dn_bid = hot.down_bid;
      dn_ask = hot.down_ask;
    }
    const std::int64_t exp_wall_ms = (active_epoch + 300) * 1000;
    const double rem_s = std::max(0.001, (exp_wall_ms - bin_wall_ms) / 1000.0);
    const double T_years = rem_s / (365.0 * 24.0 * 3600.0);
    const double sigma_raw = have_sigma ? sigma_bucket : sigma_fallback;
    const double sigma_use = clamp_sigma(sigma_raw, sigma_clamp_min, sigma_clamp_max);
    const bool sigma_is_fallback = !have_sigma;
    const bool sigma_was_clamped =
        std::isfinite(sigma_raw) && (sigma_raw < sigma_clamp_min - 1e-12 || sigma_raw > sigma_clamp_max + 1e-12);
    const double p_up = digital_call_prob(S, K, T_years, sigma_use, r);
    const double p_dn = digital_call_prob(K, S, T_years, sigma_use, r);  // placeholder; overwritten below
    const double d2 = (std::log(S / K) + (r - 0.5 * sigma_use * sigma_use) * T_years) /
                      (sigma_use * std::sqrt(T_years));
    const double disc = std::exp(-r * T_years);
    const double theo_up = clamp01(disc * normal_cdf(d2));
    const double theo_dn = clamp01(disc * normal_cdf(-d2));

    if (paper.pending_action == "BUY") {
      if (paper.have_pos) {
        paper.pending = false;
        return;
      }
      const bool want_up = (paper.pending_side == "Up");
      const double ask = want_up ? up_ask : dn_ask;
      const double mid = want_up ? 0.5 * (up_bid + up_ask) : 0.5 * (dn_bid + dn_ask);
      const double theo = want_up ? theo_up : theo_dn;
      const double edge = theo - ask;
      if (edge < entry) {
        paper.pending = false;
        return;
      }
      if (!entry_elapsed_ok(active_epoch, bin_entry_clock_wall_ms)) {
        paper.pending = false;
        return;
      }
      if (use_buy_ask_range) {
        if (!std::isfinite(ask) || ask <= 0.0 || ask < buy_ask_min || ask > buy_ask_max) {
          paper.pending = false;
          return;
        }
      } else if (ask < 0.02) {
        paper.pending = false;
        return;
      }
      // Polymarket has a practical minimum order size; enforce min $1 notional per entry when using risk_frac.
      const double spend_target =
          (fixed_spend_usd > 0.0) ? fixed_spend_usd : std::max(paper.cash * risk_frac, 1.0);
      // With fractional shares allowed, target notional spend is the key constraint.
      // Also ensure we can pay fee: spend + fee(spend) <= cash.
      const double fee_mult = fee_rate * ask * (1.0 - ask);
      const double max_spend_affordable = paper.cash / (1.0 + fee_mult);
      const double spend = std::min(spend_target, max_spend_affordable);
      if (spend < 1.0) {
        paper.pending = false;
        return;
      }
      const double qty = spend / ask;
      const double cost = spend;
      const double fee = poly_taker_fee(cost, ask, fee_rate);
      const double proj_eq = paper.cash - cost - fee + qty * mid;
      if (std::isfinite(max_loss_usd) && initial_cash - proj_eq > max_loss_usd + 1e-9) {
        paper.risk_stop = true;
        paper.pending = false;
        {
          ll::io::SyncCerrLock _;
          std::cerr << log_pfx << " risk: max-loss cap (" << max_loss_usd << " USD) would be exceeded; "
                       "blocking BUY\n";
        }
        return;
      }
      std::int64_t daemon_submit_latency_ns = -1;
      double fill_qty = qty;
      double fill_cost = cost;
      double fill_fee = fee;
      double fill_avg_price = ask;
      bool fill_price_estimated = false;
      const double signal_ask = ask;
#ifdef LL_ENABLE_LIVE_TRADER
      if (live_execution) {
        const std::string token_id = want_up ? feed_up.token_id() : feed_down.token_id();
        double bal_before = 0.0;
        std::string bal_err;
        if (!live_ex.query_conditional_balance(token_id, &bal_before, &bal_err, nullptr)) {
          {
            ll::io::SyncCerrLock _;
            std::cerr << log_pfx << " BUY balance query failed: " << bal_err << "\n";
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + kCooldownNs;
          paper.pending = false;
          return;
        }
        ll::execution::OrderIntent oi;
        oi.market_token_id = token_id;
        oi.side = "BUY";
        oi.market_order = true;
        oi.qty = spend;
        oi.market_worst_price = ask;
        oi.mono_ns = now_ns;
        std::string errmsg;
        std::string oid;
        nlohmann::json submit_resp;
        if (!live_ex.submit(oi, &errmsg, &oid, &daemon_submit_latency_ns, &submit_resp)) {
          {
            ll::io::SyncCerrLock _;
            std::cerr << "[live] BUY submit failed: " << errmsg;
            if (daemon_submit_latency_ns >= 0) {
              std::cerr << " daemon_submit_latency_ms="
                        << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
            }
            std::cerr << "\n";
          }
          if (trades_writer.ok()) {
            nlohmann::json row;
            row["schema_version"] = 1;
            row["source"] = src_tag;
            row["event_type"] = "error";
            row["action"] = "BUY";
            row["slug"] = slug;
            row["side"] = paper.pending_side;
            row["token_id"] = oi.market_token_id;
            row["error"] = errmsg;
            row["local_ts_wall_ms"] = ll::core::system_ms();
            row["local_ts_mono_ns"] = now_ns;
            row["theo"] = theo;
            row["signal_ask"] = signal_ask;
            row["ask"] = signal_ask;
            row["edge"] = edge;
            row["qty"] = qty;
            row["spend"] = spend;
            if (daemon_submit_latency_ns >= 0) {
              row["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
              row["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
            }
            trades_writer.append(row);
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + kCooldownNs;
          paper.pending = false;
          return;
        }
        const LiveMarketBuyFill bf = live_resolve_market_buy_fill(
            live_ex, token_id, oid, bal_before, spend, signal_ask, fee_rate, submit_resp);
        if (!bf.ok) {
          {
            ll::io::SyncCerrLock _;
            std::cerr << log_pfx << " BUY fill unresolved";
            if (!bf.last_error.empty()) {
              std::cerr << ": " << bf.last_error;
            }
            std::cerr << "\n";
          }
          if (trades_writer.ok()) {
            nlohmann::json row;
            row["schema_version"] = 1;
            row["source"] = src_tag;
            row["event_type"] = "error";
            row["action"] = "BUY_NO_FILL";
            row["slug"] = slug;
            row["side"] = paper.pending_side;
            row["token_id"] = token_id;
            row["order_id"] = oid;
            row["signal_ask"] = signal_ask;
            row["ask"] = signal_ask;
            row["spend"] = spend;
            if (!bf.last_error.empty()) {
              row["error"] = bf.last_error;
            }
            row["local_ts_wall_ms"] = ll::core::system_ms();
            row["local_ts_mono_ns"] = now_ns;
            trades_writer.append(row);
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + kCooldownNs;
          paper.pending = false;
          return;
        }
        fill_qty = bf.fill_qty;
        fill_cost = bf.cost_usd;
        fill_fee = bf.fee_usd;
        fill_avg_price = bf.fill_avg_price;
        fill_price_estimated = bf.fill_price_estimated;
      }
#endif
      paper.cash -= (fill_cost + fill_fee);
      paper.have_pos = true;
      paper.side = paper.pending_side;
      paper.slug = slug;
      paper.token_id = want_up ? feed_up.token_id() : feed_down.token_id();
      paper.qty = fill_qty;
      paper.entry_fill_price = fill_avg_price;
      paper.pos_cost_basis = fill_cost + fill_fee;
      {
        double eq = paper.cash;
        if (paper.have_pos) {
          eq += paper.qty * mid;
        }
        if (std::isfinite(max_loss_usd) && initial_cash - eq >= max_loss_usd - 1e-9) {
          paper.risk_stop = true;
        }
      }
      {
        nlohmann::json row;
        row["schema_version"] = 1;
        row["source"] = src_tag;
        row["event_type"] = "trade";
        row["action"] = "BUY";
        row["slug"] = paper.slug;
        row["side"] = paper.side;
        row["token_id"] = paper.token_id;
        row["local_ts_mono_ns"] = now_ns;
        row["local_ts_wall_ms"] = ll::core::system_ms();
        row["S"] = S;
        row["K"] = K;
        row["T_s"] = rem_s;
        row["sigma"] = sigma_use;
        if (sigma_was_clamped) {
          row["sigma_raw"] = sigma_raw;
          row["sigma_clamped"] = true;
        }
        append_sigma_trade_fields(row, active_epoch, bin_wall_ms);
        row["theo"] = theo;
        row["bid"] = (want_up ? up_bid : dn_bid);
        row["signal_ask"] = signal_ask;
        row["ask"] = signal_ask;
        row["fill_price"] = fill_avg_price;
        row["entry_fill_price"] = fill_avg_price;
        row["mid"] = mid;
        row["edge"] = edge;
        row["qty"] = fill_qty;
        row["cost"] = fill_cost;
        row["fee"] = fill_fee;
        row["spend_target"] = spend_target;
        row["spend"] = fill_cost;
        row["fill_price_estimated"] = fill_price_estimated;
        row["pos_cost_basis"] = paper.pos_cost_basis;
        row["cash_before"] = (paper.cash + fill_cost + fill_fee);
        row["cash_after"] = paper.cash;
        if (live_execution && daemon_submit_latency_ns >= 0) {
          row["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
          row["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
        }
        trades_writer.append(row);
      }
      {
        std::lock_guard<std::mutex> pk(print_mu);
        std::cout << log_pfx << " BUY side=" << paper.side << " slug=" << paper.slug
                  << " S=" << S << " K=" << K << " T_s=" << rem_s << " sigma=" << sigma_use
                  << (sigma_is_fallback ? " (fallback)" : "")
                  << " theo=" << theo << " bid=" << (want_up ? up_bid : dn_bid)
                  << " signal_ask=" << signal_ask << " fill_price=" << fill_avg_price
                  << (fill_price_estimated ? " (est)" : "")
                  << " mid=" << mid << " edge=" << edge << " qty=" << fill_qty
                  << " cost=" << fill_cost << " fee=" << fill_fee
                  << " spend_target=" << spend_target << " spend=" << fill_cost
                  << " pos_cost_basis=" << paper.pos_cost_basis
                  << " cash_before=" << (paper.cash + fill_cost + fill_fee) << " cash_after=" << paper.cash;
        if (live_execution && daemon_submit_latency_ns >= 0) {
          std::cout << " daemon_submit_ms=" << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
        }
        std::cout << "\n";
      }
      paper.pending = false;
      return;
    }

    if (paper.pending_action == "SELL" && paper.have_pos) {
      const bool is_up = (paper.side == "Up");
      const double bid = is_up ? up_bid : dn_bid;
      const double ask = is_up ? up_ask : dn_ask;
      const double mid = 0.5 * (bid + ask);
      const double up_mid_ex = 0.5 * (up_bid + up_ask);
      const double dn_mid_ex = 0.5 * (dn_bid + dn_ask);
      const double theo = is_up ? theo_up : theo_dn;
      const double edge = theo - ask;
      const bool exit_is_stop = (paper.pending_exit_reason == "stop_loss");
      if (!exit_is_stop) {
        if (is_up) {
          if (up_bid < theo_up - close_eps - close_early_threshold) {
            paper.pending = false;
            return;
          }
        } else {
          if (dn_bid < theo_dn - close_eps - close_early_threshold) {
            paper.pending = false;
            return;
          }
        }
      }
      const double strategy_qty = paper.qty;
      double sell_qty = strategy_qty;
      double clob_balance_shares = std::numeric_limits<double>::quiet_NaN();
      std::int64_t daemon_query_latency_ns = -1;
#ifdef LL_ENABLE_LIVE_TRADER
      if (live_execution) {
        std::string qerr;
        if (!live_ex.query_conditional_balance(paper.token_id, &clob_balance_shares, &qerr,
                                               &daemon_query_latency_ns)) {
          {
            ll::io::SyncCerrLock _;
            std::cerr << "[live] SELL conditional balance query failed: " << qerr;
            if (daemon_query_latency_ns >= 0) {
              std::cerr << " daemon_query_latency_ms="
                        << (static_cast<double>(daemon_query_latency_ns) / 1e6);
            }
            std::cerr << "\n";
          }
          if (trades_writer.ok()) {
            nlohmann::json row;
            row["schema_version"] = 1;
            row["source"] = src_tag;
            row["event_type"] = "error";
            row["action"] = "SELL_BALANCE_QUERY";
            row["slug"] = paper.slug;
            row["side"] = paper.side;
            row["token_id"] = paper.token_id;
            row["error"] = qerr;
            row["local_ts_wall_ms"] = ll::core::system_ms();
            row["local_ts_mono_ns"] = now_ns;
            row["strategy_qty"] = strategy_qty;
            if (daemon_query_latency_ns >= 0) {
              row["daemon_query_latency_ns"] = daemon_query_latency_ns;
              row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_ns) / 1e6;
            }
            trades_writer.append(row);
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + kCooldownNs;
          paper.pending = false;
          return;
        }
        // Live SELL size: use CLOB conditional balance only (do not cap by strategy_qty).
        sell_qty = std::max(0.0, clob_balance_shares);
      }
#endif
      if (sell_qty <= 1e-12) {
        if (live_execution && exit_is_stop && std::isfinite(clob_balance_shares) &&
            clob_balance_shares <= 1e-12) {
          paper.have_pos = false;
          paper.qty = 0.0;
          paper.pos_cost_basis = 0.0;
          paper.entry_fill_price = 0.0;
          paper.side.clear();
          paper.slug.clear();
          paper.token_id.clear();
          paper.stop_sell_retry = false;
          paper.pending = false;
          return;
        }
        if (live_execution) {
          {
            ll::io::SyncCerrLock _;
            std::cerr << "[live] SELL skipped: clob_balance=" << clob_balance_shares
                      << " (API reported 0 or negative sellable shares; keeping position for retry)\n";
          }
          if (trades_writer.ok()) {
            nlohmann::json row;
            row["schema_version"] = 1;
            row["source"] = src_tag;
            row["event_type"] = "error";
            row["action"] = "SELL_SKIPPED_ZERO";
            row["slug"] = paper.slug;
            row["side"] = paper.side;
            row["token_id"] = paper.token_id;
            row["error"] = "sell_qty from API balance is 0";
            row["local_ts_wall_ms"] = ll::core::system_ms();
            row["local_ts_mono_ns"] = now_ns;
            row["strategy_qty"] = strategy_qty;
            if (std::isfinite(clob_balance_shares)) {
              row["clob_balance"] = clob_balance_shares;
            }
            if (daemon_query_latency_ns >= 0) {
              row["daemon_query_latency_ns"] = daemon_query_latency_ns;
              row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_ns) / 1e6;
            }
            trades_writer.append(row);
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + kCooldownNs;
        }
        paper.pending = false;
        return;
      }
      double proceeds = sell_qty * bid;
      double fee = poly_taker_fee(proceeds, bid, fee_rate);
      const double cash_before = paper.cash;
      std::int64_t daemon_submit_latency_ns = -1;
      int stop_sell_attempts = 0;
#ifdef LL_ENABLE_LIVE_TRADER
      if (live_execution && exit_is_stop) {
        const StopSellUntilFlatResult sfr =
            live_stop_sell_until_flat(live_ex, paper.token_id, bid, fee_rate);
        stop_sell_attempts = sfr.attempts;
        clob_balance_shares = sfr.final_balance;
        if (!sfr.flat) {
          paper.stop_sell_retry = true;
          {
            ll::io::SyncCerrLock _;
            std::cerr << log_pfx << " stop-loss FAK incomplete after " << sfr.attempts
                      << " attempts; clob_balance=" << sfr.final_balance << " — retrying\n";
            if (!sfr.last_error.empty()) {
              std::cerr << log_pfx << " last_error: " << sfr.last_error << "\n";
            }
          }
          if (trades_writer.ok()) {
            nlohmann::json row;
            row["schema_version"] = 1;
            row["source"] = src_tag;
            row["event_type"] = "error";
            row["action"] = "SELL_STOP_INCOMPLETE";
            row["exit_reason"] = "stop_loss";
            row["order_type"] = "FAK";
            row["slug"] = paper.slug;
            row["side"] = paper.side;
            row["token_id"] = paper.token_id;
            row["stop_sell_attempts"] = sfr.attempts;
            row["clob_balance"] = sfr.final_balance;
            row["strategy_qty"] = strategy_qty;
            if (!sfr.last_error.empty()) {
              row["error"] = sfr.last_error;
            }
            row["local_ts_wall_ms"] = ll::core::system_ms();
            row["local_ts_mono_ns"] = now_ns;
            trades_writer.append(row);
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + 200'000'000LL;
          paper.pending = false;
          return;
        }
        paper.stop_sell_retry = false;
        proceeds = sfr.total_proceeds;
        fee = sfr.total_fee;
      } else if (live_execution) {
        ll::execution::OrderIntent oi;
        oi.market_token_id = paper.token_id;
        oi.side = "SELL";
        oi.market_order = true;
        oi.qty = sell_qty;
        oi.mono_ns = now_ns;
        if (bid > 0.0 && std::isfinite(bid)) {
          oi.market_worst_price = std::max(0.01, bid);
        }
        std::string errmsg;
        std::string oid;
        if (!live_ex.submit(oi, &errmsg, &oid, &daemon_submit_latency_ns)) {
          {
            ll::io::SyncCerrLock _;
            std::cerr << "[live] SELL submit failed: " << errmsg;
            if (daemon_submit_latency_ns >= 0) {
              std::cerr << " daemon_submit_latency_ms="
                        << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
            }
            std::cerr << "\n";
          }
          if (trades_writer.ok()) {
            nlohmann::json row;
            row["schema_version"] = 1;
            row["source"] = src_tag;
            row["event_type"] = "error";
            row["action"] = "SELL";
            row["slug"] = paper.slug;
            row["side"] = paper.side;
            row["token_id"] = paper.token_id;
            row["error"] = errmsg;
            row["local_ts_wall_ms"] = ll::core::system_ms();
            row["local_ts_mono_ns"] = now_ns;
            row["theo"] = theo;
            row["bid"] = bid;
            row["edge"] = edge;
            row["strategy_qty"] = strategy_qty;
            row["sell_qty"] = sell_qty;
            if (std::isfinite(clob_balance_shares)) {
              row["clob_balance"] = clob_balance_shares;
            }
            if (daemon_query_latency_ns >= 0) {
              row["daemon_query_latency_ns"] = daemon_query_latency_ns;
              row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_ns) / 1e6;
            }
            if (daemon_submit_latency_ns >= 0) {
              row["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
              row["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
            }
            trades_writer.append(row);
          }
          paper.cooldown_until_ns = ll::core::steady_ns() + kCooldownNs;
          paper.pending = false;
          return;
        }
      }
#endif
      paper.cash += (proceeds - fee);
      {
        nlohmann::json row;
        row["schema_version"] = 1;
        row["source"] = src_tag;
        row["event_type"] = "trade";
        row["action"] = "SELL";
        row["exit_reason"] = paper.pending_exit_reason.empty() ? "take_profit" : paper.pending_exit_reason;
        if (exit_is_stop) {
          row["order_type"] = "FAK";
          if (stop_sell_attempts > 0) {
            row["stop_sell_attempts"] = stop_sell_attempts;
          }
        }
        row["slug"] = paper.slug;
        row["side"] = paper.side;
        row["token_id"] = paper.token_id;
        row["local_ts_mono_ns"] = now_ns;
        row["local_ts_wall_ms"] = ll::core::system_ms();
        row["S"] = S;
        row["K"] = K;
        row["T_s"] = rem_s;
        row["sigma"] = sigma_use;
        append_sigma_trade_fields(row, active_epoch, bin_wall_ms);
        row["theo"] = theo;
        row["bid"] = bid;
        row["ask"] = ask;
        row["mid"] = mid;
        row["edge"] = edge;
        row["qty"] = sell_qty;
        row["strategy_qty"] = strategy_qty;
        if (live_execution && std::isfinite(clob_balance_shares)) {
          row["clob_balance_shares"] = clob_balance_shares;
          row["qty_clamped"] = (std::abs(sell_qty - strategy_qty) > 1e-12);
        }
        row["proceeds"] = proceeds;
        row["fee"] = fee;
        row["cash_before"] = cash_before;
        row["cash_after"] = paper.cash;
        if (live_execution && daemon_query_latency_ns >= 0) {
          row["daemon_query_latency_ns"] = daemon_query_latency_ns;
          row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_ns) / 1e6;
        }
        if (live_execution && daemon_submit_latency_ns >= 0) {
          row["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
          row["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
        }
        trades_writer.append(row);
      }
      {
        std::lock_guard<std::mutex> pk(print_mu);
        std::cout << log_pfx << " SELL side=" << paper.side << " slug=" << paper.slug
                  << " S=" << S << " K=" << K << " T_s=" << rem_s << " sigma=" << sigma_use
                  << (sigma_is_fallback ? " (fallback)" : "")
                  << " theo=" << theo << " bid=" << bid << " ask=" << ask << " mid=" << mid
                  << " edge=" << edge << " qty=" << sell_qty << " proceeds=" << proceeds
                  << " fee=" << fee << " cash_before=" << cash_before << " cash_after=" << paper.cash;
        if (live_execution && std::isfinite(clob_balance_shares)) {
          std::cout << " strategy_qty=" << strategy_qty << " clob_balance=" << clob_balance_shares;
        }
        if (live_execution && daemon_query_latency_ns >= 0) {
          std::cout << " daemon_query_ms=" << (static_cast<double>(daemon_query_latency_ns) / 1e6);
        }
        if (live_execution && daemon_submit_latency_ns >= 0) {
          std::cout << " daemon_submit_ms=" << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
        }
        std::cout << "\n";
      }
      {
        const double eq_flat = paper.cash;
        if (std::isfinite(max_loss_usd) && initial_cash - eq_flat >= max_loss_usd - 1e-9) {
          paper.risk_stop = true;
        }
      }
      paper.have_pos = false;
      paper.qty = 0.0;
      paper.pos_cost_basis = 0.0;
      paper.entry_fill_price = 0.0;
      paper.side.clear();
      paper.slug.clear();
      paper.token_id.clear();
      paper.pending_exit_reason.clear();
      paper.stop_sell_retry = false;
      paper.pending = false;
      return;
    }
    // Unknown state; clear pending.
    paper.pending = false;
    paper.pending_exit_reason.clear();
  };

  auto on_hot_update = [&](bool run_strategy) {
    // Called after spot/poly updates, or on idle timer for series-only sampling.
    // When run_strategy is false, only rate-limited series rows are written (no schedule/execute).
    std::int64_t active_epoch = -1;
    std::int64_t bin_wall_ms = 0;
    double S = 0.0, K = 0.0;
    bool have_sigma = false;
    double sigma_bucket = 0.0;
    double up_bid = 0, up_ask = 0, dn_bid = 0, dn_ask = 0;
    {
      std::lock_guard<std::mutex> lk(hot.mu);
      if (!hot.have_bin || !hot.have_K || !hot.have_up || !hot.have_down || hot.active_epoch < 0) return;
      active_epoch = hot.active_epoch;
      bin_wall_ms = hot.bin_wall_ms;
      S = hot.bin_mid;
      K = hot.K;
      have_sigma = hot.have_sigma;
      sigma_bucket = hot.sigma_bucket;
      up_bid = hot.up_bid;
      up_ask = hot.up_ask;
      dn_bid = hot.down_bid;
      dn_ask = hot.down_ask;
    }
    const std::int64_t exp_wall_ms = (active_epoch + 300) * 1000;
    const double rem_s = std::max(0.001, (exp_wall_ms - bin_wall_ms) / 1000.0);
    const double T_years = rem_s / (365.0 * 24.0 * 3600.0);
    const double sigma_raw = have_sigma ? sigma_bucket : sigma_fallback;
    const double sigma_use = clamp_sigma(sigma_raw, sigma_clamp_min, sigma_clamp_max);
    const double d2 = (std::log(S / K) + (r - 0.5 * sigma_use * sigma_use) * T_years) /
                      (sigma_use * std::sqrt(T_years));
    const double disc = std::exp(-r * T_years);
    const double p_up = clamp01(disc * normal_cdf(d2));
    const double p_dn = clamp01(disc * normal_cdf(-d2));
    const double up_mid = 0.5 * (up_bid + up_ask);
    const double dn_mid = 0.5 * (dn_bid + dn_ask);

    // Record theo vs market series (rate-limited).
    if (series_writer.ok()) {
      const auto now_ns = ll::core::steady_ns();
      if (last_series_mono_ns == 0 || now_ns - last_series_mono_ns >= kSeriesEveryNs) {
        last_series_mono_ns = now_ns;
        nlohmann::json row;
        row["schema_version"] = 1;
        row["source"] = src_tag;
        row["event_type"] = "series";
        row["local_ts_mono_ns"] = now_ns;
        row["local_ts_wall_ms"] = ll::core::system_ms();
        row["active_epoch"] = active_epoch;
        row["T_s"] = rem_s;
        row["S"] = S;
        row["K"] = K;
        row["sigma"] = sigma_use;
        if (std::isfinite(sigma_raw) &&
            (sigma_raw < sigma_clamp_min - 1e-12 || sigma_raw > sigma_clamp_max + 1e-12)) {
          row["sigma_raw"] = sigma_raw;
          row["sigma_clamped"] = true;
        }
        {
          bool window_full = false;
          bool slug_sigma_ready = have_sigma;
          const std::int64_t anchor_ms = active_epoch * 1000LL;
          std::lock_guard<std::mutex> lk(hot.mu);
          slug_sigma_ready = hot.have_sigma;
          if (!hot.bin_hist.empty()) {
            window_full = hot.bin_hist.front().first <= anchor_ms - 3600 * 1000LL;
          }
          row["sigma_fallback"] = !slug_sigma_ready;
          row["sigma_vol_window_full"] = window_full;
          row["sigma_slug_ready"] = slug_sigma_ready;
        }
        row["theo_up"] = p_up;
        row["theo_down"] = p_dn;
        row["up"] = {{"bid", up_bid}, {"ask", up_ask}, {"mid", up_mid}};
        row["down"] = {{"bid", dn_bid}, {"ask", dn_ask}, {"mid", dn_mid}};
        {
          std::lock_guard<std::mutex> lk(paper_mu);
          row["cash"] = paper.cash;
          row["have_pos"] = paper.have_pos;
          row["pos_side"] = paper.side;
          row["pos_qty"] = paper.qty;
          if (paper.have_pos && paper.entry_fill_price > 0.0) {
            row["entry_fill_price"] = paper.entry_fill_price;
            row["pos_cost_basis"] = paper.pos_cost_basis;
          }
          row["trading_enabled"] = trading_enabled;
        }
        series_writer.append(row);
      }
    }

    if (!run_strategy) {
      return;
    }
    bool enabled = false;
    {
      std::lock_guard<std::mutex> lk(paper_mu);
      enabled = trading_enabled;
    }
    if (!enabled) return;
    schedule(p_up, p_dn, up_ask, up_bid, dn_ask, dn_bid);
    execute_due();
  };

  std::thread consumer_thr([&] {
    std::int64_t spot_lat_report_last_mono_ns = 0;
    std::vector<std::int64_t> spot_lat_total_ns;
    std::vector<std::int64_t> spot_lat_sigma_ns;
    std::vector<std::int64_t> spot_lat_q_rem;
    const std::int64_t spot_lat_report_period_ns =
        static_cast<std::int64_t>(spot_latency_report_sec * 1e9);
    spot_lat_total_ns.reserve(4096);
    spot_lat_sigma_ns.reserve(4096);
    spot_lat_q_rem.reserve(4096);

    auto maybe_report_spot_latency = [&]() {
      if (!spot_latency_monitor) return;
      const auto now = ll::core::steady_ns();
      if (spot_lat_report_last_mono_ns == 0) {
        spot_lat_report_last_mono_ns = now;
        return;
      }
      if (now - spot_lat_report_last_mono_ns < spot_lat_report_period_ns) return;
      spot_lat_report_last_mono_ns = now;

      const std::uint64_t push_q_max =
          ev_q_push_depth_max_window.exchange(0, std::memory_order_relaxed);

      const std::size_t n = spot_lat_total_ns.size();
      if (n == 0) {
        ll::io::SyncCerrLock _;
        std::cerr << log_pfx << " spot_latency summary window_sec=" << spot_latency_report_sec
                  << " samples=0 push_q_max_window=" << push_q_max << "\n";
        return;
      }

      std::vector<std::int64_t> tot = spot_lat_total_ns;
      std::vector<std::int64_t> sig = spot_lat_sigma_ns;
      std::vector<std::int64_t> qrem = spot_lat_q_rem;
      spot_lat_total_ns.clear();
      spot_lat_sigma_ns.clear();
      spot_lat_q_rem.clear();

      const double tot_p50 = percentile_sorted_copy(tot, 0.50) / 1000.0;
      const double tot_p95 = percentile_sorted_copy(tot, 0.95) / 1000.0;
      const double tot_p99 = percentile_sorted_copy(tot, 0.99) / 1000.0;
      const double tot_max =
          static_cast<double>(*std::max_element(tot.begin(), tot.end())) / 1000.0;

      const double sig_p50 = percentile_sorted_copy(sig, 0.50) / 1000.0;
      const double sig_p95 = percentile_sorted_copy(sig, 0.95) / 1000.0;
      const double sig_p99 = percentile_sorted_copy(sig, 0.99) / 1000.0;
      const double sig_max =
          static_cast<double>(*std::max_element(sig.begin(), sig.end())) / 1000.0;

      const double qrem_p50 = percentile_sorted_copy(qrem, 0.50);
      const double qrem_p99 = percentile_sorted_copy(qrem, 0.99);
      const double qrem_max =
          static_cast<double>(*std::max_element(qrem.begin(), qrem.end()));

      ll::io::SyncCerrLock _;
      std::cerr << log_pfx << " spot_latency summary window_sec=" << spot_latency_report_sec
                << " samples=" << n << " push_q_max_window=" << push_q_max
                << " q_rem_p50=" << qrem_p50 << " q_rem_p99=" << qrem_p99 << " q_rem_max=" << qrem_max
                << " spot_total_us p50=" << tot_p50 << " p95=" << tot_p95 << " p99=" << tot_p99
                << " max=" << tot_max << " | sigma_path_us p50=" << sig_p50 << " p95=" << sig_p95
                << " p99=" << sig_p99 << " max=" << sig_max << "\n";
    };

    for (;;) {
      Event ev;
      std::size_t q_rem = 0;
      {
        std::unique_lock<std::mutex> lk(ev_mu);
        ev_cv.wait_for(lk, std::chrono::milliseconds(kSeriesIdlePollMs), [&] {
          return g_stop.load(std::memory_order_relaxed) || !ev_q.empty();
        });
        if (ev_q.empty()) {
          lk.unlock();
          if (g_stop.load(std::memory_order_relaxed)) {
            return;
          }
          on_hot_update(false);
          continue;
        }
        ev = std::move(ev_q.front());
        ev_q.pop();
        q_rem = ev_q.size();
      }

      maybe_report_spot_latency();

      if (ev.type == Event::Type::Stop) {
        return;
      }

      if (ev.type == Event::Type::Rollover) {
        // Optional: FORCE_SELL any open position at rollover using current best bid.
        {
          std::lock_guard<std::mutex> lk(paper_mu);
          if (paper.have_pos && disable_rollover_force_sell) {
            {
              ll::io::SyncCerrLock _;
              std::cerr << log_pfx << " rollover: FORCE_SELL disabled; carrying position for manual settlement"
                        << " side=" << paper.side << " qty=" << paper.qty << " token_id=" << paper.token_id
                        << " slug=" << paper.slug << "\n";
            }
            paper.have_pos = false;
            paper.pending = false;
            paper.qty = 0.0;
            paper.pos_cost_basis = 0.0;
          paper.entry_fill_price = 0.0;
            paper.side.clear();
            paper.slug.clear();
            paper.token_id.clear();
          } else if (paper.have_pos) {
            const double bid = (paper.side == "Up") ? ev.rollover_up_bid : ev.rollover_down_bid;
            const double strategy_qty_fs = paper.qty;
            double sell_qty_fs = strategy_qty_fs;
            double clob_balance_fs = std::numeric_limits<double>::quiet_NaN();
            std::int64_t daemon_query_latency_fs_ns = -1;
            bool balance_query_failed = false;
#ifdef LL_ENABLE_LIVE_TRADER
            if (live_execution) {
              std::string qerr;
              if (!live_ex.query_conditional_balance(paper.token_id, &clob_balance_fs, &qerr,
                                                     &daemon_query_latency_fs_ns)) {
                {
                  ll::io::SyncCerrLock _;
                  std::cerr << "[live] FORCE_SELL conditional balance query failed: " << qerr;
                  if (daemon_query_latency_fs_ns >= 0) {
                    std::cerr << " daemon_query_latency_ms="
                              << (static_cast<double>(daemon_query_latency_fs_ns) / 1e6);
                  }
                  std::cerr << "\n";
                }
                balance_query_failed = true;
                if (trades_writer.ok()) {
                  const auto wall_ms = ll::core::system_ms();
                  const auto mono_ns = ll::core::steady_ns();
                  nlohmann::json row;
                  row["schema_version"] = 1;
                  row["source"] = src_tag;
                  row["event_type"] = "error";
                  row["action"] = "FORCE_SELL_BALANCE_QUERY";
                  row["slug"] = paper.slug;
                  row["side"] = paper.side;
                  row["token_id"] = paper.token_id;
                  row["error"] = qerr;
                  row["local_ts_wall_ms"] = wall_ms;
                  row["local_ts_mono_ns"] = mono_ns;
                  row["strategy_qty"] = strategy_qty_fs;
                  if (daemon_query_latency_fs_ns >= 0) {
                    row["daemon_query_latency_ns"] = daemon_query_latency_fs_ns;
                    row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_fs_ns) / 1e6;
                  }
                  trades_writer.append(row);
                }
              } else {
                // FORCE_SELL size: CLOB balance only (do not cap by strategy_qty).
                sell_qty_fs = std::max(0.0, clob_balance_fs);
              }
            }
#endif
            if (balance_query_failed) {
              // Keep open position; rollover slug/K reset still happens below.
            } else if (sell_qty_fs <= 1e-12) {
              if (live_execution) {
                {
                  ll::io::SyncCerrLock _;
                  std::cerr << "[live] FORCE_SELL skipped: clob_balance=" << clob_balance_fs
                            << " (API reported 0 sellable shares)\n";
                }
                if (trades_writer.ok()) {
                  nlohmann::json row;
                  row["schema_version"] = 1;
                  row["source"] = src_tag;
                  row["event_type"] = "error";
                  row["action"] = "FORCE_SELL_SKIPPED_ZERO";
                  row["slug"] = paper.slug;
                  row["side"] = paper.side;
                  row["token_id"] = paper.token_id;
                  row["error"] = "sell_qty from API balance is 0 at rollover FORCE_SELL";
                  row["local_ts_wall_ms"] = ll::core::system_ms();
                  row["local_ts_mono_ns"] = ll::core::steady_ns();
                  row["strategy_qty"] = strategy_qty_fs;
                  if (std::isfinite(clob_balance_fs)) {
                    row["clob_balance"] = clob_balance_fs;
                  }
                  if (daemon_query_latency_fs_ns >= 0) {
                    row["daemon_query_latency_ns"] = daemon_query_latency_fs_ns;
                    row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_fs_ns) / 1e6;
                  }
                  trades_writer.append(row);
                }
              }
              paper.have_pos = false;
              paper.pending = false;
              paper.qty = 0.0;
              paper.pos_cost_basis = 0.0;
          paper.entry_fill_price = 0.0;
              paper.side.clear();
              paper.slug.clear();
              paper.token_id.clear();
            } else {
              const double proceeds = sell_qty_fs * bid;
              const double fee = poly_taker_fee(proceeds, bid, fee_rate);
              const double cash_before = paper.cash;
              bool live_fs_ok = true;
              std::int64_t daemon_submit_latency_ns = -1;
#ifdef LL_ENABLE_LIVE_TRADER
              if (live_execution) {
                ll::execution::OrderIntent oi;
                oi.market_token_id = paper.token_id;
                oi.side = "SELL";
                oi.market_order = true;
                oi.qty = sell_qty_fs;
                oi.mono_ns = ll::core::steady_ns();
                std::string errmsg;
                std::string oid;
                if (!live_ex.submit(oi, &errmsg, &oid, &daemon_submit_latency_ns)) {
                  {
                    ll::io::SyncCerrLock _;
                    std::cerr << "[live] FORCE_SELL submit failed: " << errmsg;
                    if (daemon_submit_latency_ns >= 0) {
                      std::cerr << " daemon_submit_latency_ms="
                                << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
                    }
                    std::cerr << "\n";
                  }
                  live_fs_ok = false;
                  if (trades_writer.ok()) {
                    nlohmann::json erow;
                    erow["schema_version"] = 1;
                    erow["source"] = src_tag;
                    erow["event_type"] = "error";
                    erow["action"] = "FORCE_SELL";
                    erow["slug"] = paper.slug;
                    erow["side"] = paper.side;
                    erow["token_id"] = paper.token_id;
                    erow["error"] = errmsg;
                    erow["local_ts_wall_ms"] = ll::core::system_ms();
                    erow["local_ts_mono_ns"] = ll::core::steady_ns();
                    erow["bid"] = bid;
                    erow["strategy_qty"] = strategy_qty_fs;
                    erow["sell_qty"] = sell_qty_fs;
                    if (std::isfinite(clob_balance_fs)) {
                      erow["clob_balance"] = clob_balance_fs;
                    }
                    if (daemon_query_latency_fs_ns >= 0) {
                      erow["daemon_query_latency_ns"] = daemon_query_latency_fs_ns;
                      erow["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_fs_ns) / 1e6;
                    }
                    if (daemon_submit_latency_ns >= 0) {
                      erow["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
                      erow["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
                    }
                    trades_writer.append(erow);
                  }
                }
              }
#endif
              if (!live_execution || live_fs_ok) {
                paper.cash += (proceeds - fee);
                nlohmann::json row;
                row["schema_version"] = 1;
                row["source"] = src_tag;
                row["event_type"] = "trade";
                row["action"] = "FORCE_SELL";
                row["slug"] = paper.slug;
                row["side"] = paper.side;
                row["token_id"] = paper.token_id;
                row["local_ts_mono_ns"] = ll::core::steady_ns();
                row["local_ts_wall_ms"] = ll::core::system_ms();
                row["bid"] = bid;
                row["qty"] = sell_qty_fs;
                row["strategy_qty"] = strategy_qty_fs;
                if (live_execution && std::isfinite(clob_balance_fs)) {
                  row["clob_balance_shares"] = clob_balance_fs;
                  row["qty_clamped"] = (std::abs(sell_qty_fs - strategy_qty_fs) > 1e-12);
                }
                row["proceeds"] = proceeds;
                row["fee"] = fee;
                row["cash_before"] = cash_before;
                row["cash_after"] = paper.cash;
                row["note"] = "rollover";
                if (live_execution && daemon_query_latency_fs_ns >= 0) {
                  row["daemon_query_latency_ns"] = daemon_query_latency_fs_ns;
                  row["daemon_query_latency_ms"] = static_cast<double>(daemon_query_latency_fs_ns) / 1e6;
                }
                if (live_execution && daemon_submit_latency_ns >= 0) {
                  row["daemon_submit_latency_ns"] = daemon_submit_latency_ns;
                  row["daemon_submit_latency_ms"] = static_cast<double>(daemon_submit_latency_ns) / 1e6;
                }
                trades_writer.append(row);
                std::lock_guard<std::mutex> pk(print_mu);
                std::cout << log_pfx << " FORCE_SELL side=" << paper.side << " slug=" << paper.slug << " px=" << bid
                          << " qty=" << sell_qty_fs << " proceeds=" << proceeds << " fee=" << fee
                          << " cash_before=" << cash_before << " cash_after=" << paper.cash;
                if (live_execution && std::isfinite(clob_balance_fs)) {
                  std::cout << " strategy_qty=" << strategy_qty_fs << " clob_balance=" << clob_balance_fs;
                }
                if (live_execution && daemon_query_latency_fs_ns >= 0) {
                  std::cout << " daemon_query_ms=" << (static_cast<double>(daemon_query_latency_fs_ns) / 1e6);
                }
                if (live_execution && daemon_submit_latency_ns >= 0) {
                  std::cout << " daemon_submit_ms=" << (static_cast<double>(daemon_submit_latency_ns) / 1e6);
                }
                std::cout << "\n";
                paper.have_pos = false;
                paper.pending = false;
                paper.qty = 0.0;
                paper.pos_cost_basis = 0.0;
          paper.entry_fill_price = 0.0;
                paper.side.clear();
                paper.slug.clear();
                paper.token_id.clear();
              }
            }
          }
        }
        {
          nlohmann::json row;
          row["schema_version"] = 1;
          row["source"] = src_tag;
          row["event_type"] = "rollover";
          row["new_slug"] = ev.rollover_slug;
          row["new_epoch"] = ev.rollover_epoch;
          if (std::isfinite(ev.rollover_price_to_beat)) {
            row["price_to_beat"] = ev.rollover_price_to_beat;
          } else {
            row["price_to_beat"] = nullptr;
          }
          row["local_ts_wall_ms"] = ll::core::system_ms();
          row["disable_force_sell"] = disable_rollover_force_sell;
          {
            std::lock_guard<std::mutex> lk(paper_mu);
            row["cash"] = paper.cash;
            row["have_pos"] = paper.have_pos;
            if (paper.have_pos) {
              row["pos_side"] = paper.side;
              row["pos_qty"] = paper.qty;
              row["pos_slug"] = paper.slug;
              row["pos_token_id"] = paper.token_id;
            }
          }
          trades_writer.append(row);
        }
        {
          std::lock_guard<std::mutex> hk(hot.mu);
          hot.slug = ev.rollover_slug;
          hot.active_epoch = ev.rollover_epoch;
          if (std::isfinite(ev.rollover_price_to_beat) && ev.rollover_price_to_beat > 0.0) {
            hot.have_K = true;
            hot.K = ev.rollover_price_to_beat;
          } else {
            hot.have_K = false;
          }
          hot.have_sigma = false;
          hot.sigma_bucket = 0.0;
          hot.edge_up_ok_since_mono_ns = -1;
          hot.edge_dn_ok_since_mono_ns = -1;
        }
        {
          std::lock_guard<std::mutex> lk(paper_mu);
          trading_enabled = true;
        }
        continue;
      }

      if (ev.type == Event::Type::ChainlinkSpot || ev.type == Event::Type::BinBook ||
          ev.type == Event::Type::BinTrade) {
        const std::int64_t t_spot_begin =
            spot_latency_monitor ? ll::core::steady_ns() : std::int64_t{0};
        if (ev.type == Event::Type::ChainlinkSpot && chainlink_ticks_writer &&
            chainlink_ticks_writer->ok()) {
          const auto n = chainlink_tick_seq.fetch_add(1, std::memory_order_relaxed) + 1;
          nlohmann::json row;
          row["schema_version"] = 1;
          row["source"] = "polymarket_rtds_chainlink";
          row["symbol"] = "BTC/USD";
          row["payload_ts_ms"] = ev.chainlink_ts_ms;
          row["local_ts_mono_ns"] = ll::core::steady_ns();
          row["local_ts_wall_ms"] = ll::core::system_ms();
          row["seq"] = n;
          row["event_type"] = "chainlink";
          row["payload"] = {{"price", ev.chainlink_px}};
          chainlink_ticks_writer->append(row);
        }
        const double mid = (ev.type == Event::Type::ChainlinkSpot)
                               ? ev.chainlink_px
                               : (ev.type == Event::Type::BinBook)
                                     ? 0.5 * (ev.book.bid_price + ev.book.ask_price)
                                     : ev.trade.price;
        const std::int64_t wall_ms = (ev.type == Event::Type::ChainlinkSpot)
                                         ? ev.chainlink_ts_ms
                                         : (ev.type == Event::Type::BinBook)
                                               ? ev.book.local_wall_ms
                                               : ev.trade.local_wall_ms;
        const std::int64_t entry_clock_wall_ms =
            (ev.type == Event::Type::ChainlinkSpot) ? ll::core::system_ms() : wall_ms;
        const std::int64_t wall_s = wall_ms / 1000;

        const std::int64_t t_sigma_begin =
            spot_latency_monitor ? ll::core::steady_ns() : std::int64_t{0};
        bool need_sigma = false;
        bool have_full_hour = false;
        std::int64_t sigma_anchor_wall_ms = wall_ms;
        std::vector<double> resampled_mids;

        {
          std::lock_guard<std::mutex> lk(hot.mu);
          hot.have_bin = true;
          hot.bin_mid = mid;
          hot.bin_wall_ms = wall_ms;
          hot.bin_entry_clock_wall_ms = entry_clock_wall_ms;
          hot.bin_hist.emplace_back(wall_ms, mid);
          // Rolling window for σ; slug_fixed also keeps [bucket_anchor-1h, …] through the 5m slug.
          std::int64_t hist_trim_floor_ms = wall_ms - 3600 * 1000;
          if (sigma_vol_mode == SigmaVolMode::SlugFixed && hot.active_epoch >= 0) {
            const std::int64_t slug_anchor_ms = hot.active_epoch * 1000LL;
            const std::int64_t slug_vol_start_ms = slug_anchor_ms - 3600 * 1000LL;
            if (slug_vol_start_ms < hist_trim_floor_ms) {
              hist_trim_floor_ms = slug_vol_start_ms;
            }
          }
          while (!hot.bin_hist.empty() && hot.bin_hist.front().first < hist_trim_floor_ms) {
            hot.bin_hist.pop_front();
          }
          if (sigma_vol_mode == SigmaVolMode::Constant && hot.active_epoch >= 0 &&
              wall_s >= hot.active_epoch) {
            hot.have_sigma = true;
            hot.sigma_bucket = clamp_sigma(sigma_fallback, sigma_clamp_min, sigma_clamp_max);
          } else if (hot.active_epoch >= 0 && !hot.have_sigma && wall_s >= hot.active_epoch) {
            need_sigma = true;
            if (sigma_vol_mode == SigmaVolMode::SlugFixed) {
              sigma_anchor_wall_ms = hot.active_epoch * 1000LL;
            } else {
              sigma_anchor_wall_ms = wall_ms;
            }
            // slug_fixed anchors at bucket open: need 1h of mids ending at anchor (see resample_mids_from_hist).
            have_full_hour =
                !hot.bin_hist.empty() &&
                hot.bin_hist.front().first <= sigma_anchor_wall_ms - 3600 * 1000LL;
            if (have_full_hour) {
              hot.sigma_vol_window_full = true;
              resampled_mids =
                  resample_mids_from_hist(hot.bin_hist, sigma_anchor_wall_ms, sigma_step_ms);
            }
          }
        }

        if (need_sigma && have_full_hour && !resampled_mids.empty()) {
          double final_sig = std::numeric_limits<double>::quiet_NaN();
#ifdef LL_ENABLE_GARCH_DAEMON
          if (sigma_try_garch) {
            double garch_sig = 0.0;
            std::string gerr;
            std::int64_t glat = -1;
            if (live_ex.query_garch_sigma(resampled_mids, sigma_step_ms, &garch_sig, &gerr, &glat)) {
              if (std::isfinite(garch_sig) && garch_sig >= sigma_rv_min && garch_sig <= sigma_rv_max) {
                final_sig = garch_sig;
                {
                  ll::io::SyncCerrLock _;
                  std::cerr << log_pfx << " GARCH sigma=" << garch_sig
                            << " n_mids=" << resampled_mids.size()
                            << " anchor_wall_ms=" << sigma_anchor_wall_ms;
                  if (glat >= 0) std::cerr << " latency_ms=" << (static_cast<double>(glat) / 1e6);
                  std::cerr << "\n";
                }
              } else {
                {
                  ll::io::SyncCerrLock _;
                  std::cerr << log_pfx << " GARCH sigma out of range: " << garch_sig
                            << ", falling back to realized vol\n";
                }
              }
            } else {
              {
                ll::io::SyncCerrLock _;
                std::cerr << log_pfx << " GARCH failed: " << gerr << ", falling back to realized vol\n";
              }
            }
          }
#endif
          if (!std::isfinite(final_sig)) {
            realized_vol_from_resampled_mids(resampled_mids, sigma_step_ms, sigma_rv_min, sigma_rv_max,
                                               &final_sig);
          }

          {
            std::lock_guard<std::mutex> lk(hot.mu);
            if (std::isfinite(final_sig) && final_sig >= sigma_rv_min && final_sig <= sigma_rv_max) {
              hot.have_sigma = true;
              const double sig_clamped = clamp_sigma(final_sig, sigma_clamp_min, sigma_clamp_max);
              hot.sigma_bucket = sig_clamped;
              if (sigma_vol_mode == SigmaVolMode::SlugFixed) {
                ll::io::SyncCerrLock _;
                std::cerr << log_pfx << " slug_fixed sigma=" << sig_clamped;
                if (std::fabs(sig_clamped - final_sig) > 1e-12) {
                  std::cerr << " raw=" << final_sig;
                }
                std::cerr << " slug=" << hot.slug << " anchor_wall_ms=" << sigma_anchor_wall_ms
                          << "\n";
              }
            } else {
              hot.have_sigma = false;
              hot.sigma_bucket = 0.0;
            }
          }
        } else if (need_sigma) {
          std::lock_guard<std::mutex> lk(hot.mu);
          hot.have_sigma = false;
          hot.sigma_bucket = 0.0;
        }

        // Rolling realized σ every spot tick (only when --sigma-model rolling|realized|rv).
        if (sigma_vol_mode == SigmaVolMode::Rolling) {
          bool in_bucket = false;
          bool rv_full_hour = false;
          std::vector<double> rv_mids;
          {
            std::lock_guard<std::mutex> lk(hot.mu);
            in_bucket = hot.active_epoch >= 0 && wall_s >= hot.active_epoch;
            if (in_bucket) {
              rv_full_hour =
                  !hot.bin_hist.empty() && (wall_ms - hot.bin_hist.front().first >= 3600 * 1000);
              if (rv_full_hour) {
                hot.sigma_vol_window_full = true;
                rv_mids = resample_mids_from_hist(hot.bin_hist, wall_ms, sigma_step_ms);
              }
            }
          }
          if (in_bucket && rv_full_hour && rv_mids.size() >= 10) {
            double rv_sig = std::numeric_limits<double>::quiet_NaN();
            if (realized_vol_from_resampled_mids(rv_mids, sigma_step_ms, sigma_rv_min, sigma_rv_max,
                                                 &rv_sig)) {
              std::lock_guard<std::mutex> lk(hot.mu);
              hot.have_sigma = true;
              hot.sigma_bucket = clamp_sigma(rv_sig, sigma_clamp_min, sigma_clamp_max);
            }
          }
        }

        const std::int64_t t_sigma_end =
            spot_latency_monitor ? ll::core::steady_ns() : std::int64_t{0};
        on_hot_update(true);
        const std::int64_t t_spot_end =
            spot_latency_monitor ? ll::core::steady_ns() : std::int64_t{0};
        if (spot_latency_monitor) {
          spot_lat_total_ns.push_back(t_spot_end - t_spot_begin);
          spot_lat_sigma_ns.push_back(t_sigma_end - t_sigma_begin);
          spot_lat_q_rem.push_back(static_cast<std::int64_t>(q_rem));
          constexpr std::size_t kSpotLatCap = 16384;
          if (spot_lat_total_ns.size() > kSpotLatCap) {
            const auto drop = static_cast<std::ptrdiff_t>(kSpotLatCap / 2);
            spot_lat_total_ns.erase(spot_lat_total_ns.begin(), spot_lat_total_ns.begin() + drop);
            spot_lat_sigma_ns.erase(spot_lat_sigma_ns.begin(), spot_lat_sigma_ns.begin() + drop);
            spot_lat_q_rem.erase(spot_lat_q_rem.begin(), spot_lat_q_rem.begin() + drop);
          }
        }
        continue;
      }

      if (ev.type == Event::Type::PolyQuote) {
        {
          std::lock_guard<std::mutex> lk(hot.mu);
          if (ev.quote.outcome == "Up") {
            hot.have_up = true;
            hot.up_bid = ev.quote.best_bid;
            hot.up_ask = ev.quote.best_ask;
          } else if (ev.quote.outcome == "Down") {
            hot.have_down = true;
            hot.down_bid = ev.quote.best_bid;
            hot.down_ask = ev.quote.best_ask;
          }
          if (ev.quote.market_bucket_epoch >= 0 && hot.active_epoch < 0) {
            hot.active_epoch = ev.quote.market_bucket_epoch;
            hot.slug = ev.quote.event_slug;
            hot.edge_up_ok_since_mono_ns = -1;
            hot.edge_dn_ok_since_mono_ns = -1;
          }
        }
        on_hot_update(true);
        continue;
      }
    }
  });

  if (spot_feed_chainlink) {
    ll::polymarket::RtdsChainlinkFeed::instance().set_on_tick(
        [&](std::int64_t ts_ms, double px) {
          Event e;
          e.type = Event::Type::ChainlinkSpot;
          e.chainlink_ts_ms = ts_ms;
          e.chainlink_px = px;
          push_event(std::move(e));
        });
    ll::polymarket::RtdsChainlinkFeed::instance().ensure_started();
  } else {
    // Binance callback: update mid + hist; snapshot sigma once per bucket after open (strike K from Gamma).
    bin_client->set_on_bookticker([&](const ll::core::BookTickerTick& b) {
      Event e;
      e.type = Event::Type::BinBook;
      e.book = b;
      push_event(std::move(e));
    });

    bin_client->set_on_trade([&](const ll::core::TradeTick& t) {
      Event e;
      e.type = Event::Type::BinTrade;
      e.trade = t;
      push_event(std::move(e));
    });
  }

  // Polymarket quote callbacks: update best bid/ask.
  feed_up.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
    Event e;
    e.type = Event::Type::PolyQuote;
    e.quote = q;
    push_event(std::move(e));
  });
  feed_down.set_on_quote([&](const ll::polymarket::PolymarketWsQuote& q) {
    Event e;
    e.type = Event::Type::PolyQuote;
    e.quote = q;
    push_event(std::move(e));
  });

  // Consumer thread now owns hot updates; callbacks only enqueue. Disable direct hot updates.

  std::thread bin_thread;
  if (!spot_feed_chainlink) {
    bin_thread = std::thread([&] {
      if (!bin_client->start(bin_cfg)) {
        { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][binance] start failed\n"; }
        g_stop = true;
      }
      while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      bin_client->stop();
    });
  }

  // Polymarket discovery + rollover.
  std::int64_t active_epoch = -1;
  std::int64_t next_epoch = -1;
  std::string current_slug;
  std::string token_up;
  std::string token_dn;

  ll::polymarket::BtcFiveMinuteBucketDiscovery initial_disc;
  if (!poly_discover) {
    std::string derr;
    if (!ll::polymarket::discover_btc_updown_5m_for_exact_slug(poly_event_slug, initial_disc, &derr)) {
      { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] gamma slug fetch failed: " << derr << "\n"; }
      g_stop = true;
    } else if (initial_disc.up_token_id.empty() || initial_disc.down_token_id.empty()) {
      { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] discovery missing up/down token id\n"; }
      g_stop = true;
    } else if (poly_manual_token != initial_disc.up_token_id) {
      { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] --poly-token must match Gamma Up token id for slug\n"; }
      g_stop = true;
    } else {
      token_up = initial_disc.up_token_id;
      token_dn = initial_disc.down_token_id;
      current_slug = initial_disc.confirmed_slug;
      active_epoch = initial_disc.bucket_epoch_seconds;
      if (active_epoch < 0 && !parse_epoch_from_confirmed_slug(current_slug, &active_epoch)) {
        { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] bucket epoch unavailable from slug\n"; }
        g_stop = true;
      } else {
        maybe_fill_strike_web_then_rtds(initial_disc, poly_rollover_web_ptb, "[paper_trader]");
        feed_up.set_market_context(current_slug, active_epoch, "Up");
        feed_down.set_market_context(current_slug, active_epoch, "Down");
        std::string e1, e2;
        if (!feed_up.start(token_up, &e1) || !feed_down.start(token_dn, &e2)) {
          { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] ws start failed: " << e1 << " " << e2 << "\n"; }
          g_stop = true;
        } else {
          ll::polymarket::RtdsChainlinkFeed::instance().ensure_started();
          {
            std::lock_guard<std::mutex> hk(hot.mu);
            hot.slug = current_slug;
            hot.active_epoch = active_epoch;
            if (bucket_disc_has_strike(initial_disc)) {
              hot.have_K = true;
              hot.K = initial_disc.price_to_beat;
            } else {
              hot.have_K = false;
            }
          }
          trading_enabled = true;
        }
      }
    }
  } else {
    std::string derr;
    std::string err_exact;
    const std::int64_t wall_now_s = ll::core::system_ms() / 1000;
    const std::int64_t utc_floor = (wall_now_s / 300) * 300;
    const std::string slug_exact = "btc-updown-5m-" + std::to_string(utc_floor);
    bool discovered = false;
    // Same order as live_pipeline rollover: exact slug for the expected epoch, then ± bucket scan.
    if (ll::polymarket::discover_btc_updown_5m_for_exact_slug(slug_exact, initial_disc, &err_exact)) {
      std::int64_t ep = initial_disc.bucket_epoch_seconds;
      if (ep < 0 && !parse_epoch_from_confirmed_slug(initial_disc.confirmed_slug, &ep)) {
        ep = -1;
      }
      if (ep >= 0 && wall_now_s >= ep && wall_now_s < ep + 300) {
        discovered = true;
      }
    }
    if (!discovered) {
      if (!ll::polymarket::discover_active_btc_updown_5m_via_bucket(initial_disc, &derr)) {
        { ll::io::SyncCerrLock _;
          std::cerr << "[paper_trader][poly] bucket discovery failed: exact_slug_err=" << err_exact
                    << " scan_err=" << derr << "\n"; }
        g_stop = true;
      }
    }
    if (!g_stop.load(std::memory_order_relaxed) &&
        (initial_disc.up_token_id.empty() || initial_disc.down_token_id.empty())) {
      { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] discovery missing up/down token id\n"; }
      g_stop = true;
    }
    if (!g_stop.load(std::memory_order_relaxed)) {
      token_up = initial_disc.up_token_id;
      token_dn = initial_disc.down_token_id;
      current_slug = initial_disc.confirmed_slug;
      active_epoch = initial_disc.bucket_epoch_seconds;
      if (active_epoch < 0 && !parse_epoch_from_confirmed_slug(current_slug, &active_epoch)) {
        { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] bucket epoch unavailable from slug\n"; }
        g_stop = true;
      } else {
        next_epoch = active_epoch + 300;
        maybe_fill_strike_web_then_rtds(initial_disc, poly_rollover_web_ptb, "[paper_trader]");
        feed_up.set_market_context(current_slug, active_epoch, "Up");
        feed_down.set_market_context(current_slug, active_epoch, "Down");
        std::string e1, e2;
        if (!feed_up.start(token_up, &e1) || !feed_down.start(token_dn, &e2)) {
          { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][poly] ws start failed: " << e1 << " " << e2 << "\n"; }
          g_stop = true;
        } else {
          ll::polymarket::RtdsChainlinkFeed::instance().ensure_started();
          {
            std::lock_guard<std::mutex> hk(hot.mu);
            hot.slug = current_slug;
            hot.active_epoch = active_epoch;
            if (bucket_disc_has_strike(initial_disc)) {
              hot.have_K = true;
              hot.K = initial_disc.price_to_beat;
            } else {
              hot.have_K = false;
            }
          }
          // hot state will be reset/updated by the single consumer thread via events.
          {
            ll::io::SyncCerrLock _;
            std::cerr << '[' << src_tag << "] warmup: first slug will NOT trade; will enable on rollover slug="
                      << current_slug << " active_epoch=" << active_epoch << " price_to_beat=";
            if (initial_disc.gamma_has_price_to_beat && bucket_disc_has_strike(initial_disc)) {
              std::cerr << initial_disc.price_to_beat << " (gamma_metadata)";
            } else if (initial_disc.strike_from_polymarket_web_event_page) {
              std::cerr << initial_disc.price_to_beat << " (polymarket_web_event_page)";
            } else if (initial_disc.strike_from_polymarket_rtds_chainlink) {
              std::cerr << initial_disc.price_to_beat << " (polymarket_rtds_chainlink)";
            } else {
              std::cerr << "(strike_unavailable)";
            }
            std::cerr << '\n';
          }
        }
      }
    }
  }

  // Rollover monitor: when next bucket is ready, switch tokens; FORCE_SELL at rollover.
  while (!g_stop.load(std::memory_order_relaxed) && poly_discover) {
    const auto now_s = ll::core::system_ms() / 1000;
    if (active_epoch >= 0 && now_s >= next_epoch + 1) {
      ll::polymarket::BtcFiveMinuteBucketDiscovery d2;
      std::string err_exact;
      std::string err_scan;
      const std::string slug_next = "btc-updown-5m-" + std::to_string(next_epoch);
      bool got = false;
      if (ll::polymarket::discover_btc_updown_5m_for_exact_slug(slug_next, d2, &err_exact)) {
        std::int64_t ep = d2.bucket_epoch_seconds;
        if (ep < 0 && !parse_epoch_from_confirmed_slug(d2.confirmed_slug, &ep)) {
          ep = -1;
        }
        if (ep >= next_epoch) {
          got = true;
        }
      }
      if (!got && ll::polymarket::discover_active_btc_updown_5m_via_bucket(d2, &err_scan) &&
          d2.bucket_epoch_seconds >= next_epoch) {
        got = true;
      }
      if (got) {
        maybe_fill_strike_web_then_rtds(d2, poly_rollover_web_ptb, "[paper_trader]");
        // FORCE_SELL is handled by the single consumer thread (on rollover event).
        // Snapshot OLD-slug quotes BEFORE switch_token: after switch, hot may be empty/stale for the new tokens.
        Event rollover_ev;
        rollover_ev.type = Event::Type::Rollover;
        rollover_ev.rollover_epoch = d2.bucket_epoch_seconds;
        rollover_ev.rollover_slug = d2.confirmed_slug;
        rollover_ev.rollover_price_to_beat =
            bucket_disc_has_strike(d2) ? d2.price_to_beat : std::numeric_limits<double>::quiet_NaN();
        {
          std::lock_guard<std::mutex> hk(hot.mu);
          rollover_ev.rollover_up_bid = hot.up_bid;
          rollover_ev.rollover_up_ask = hot.up_ask;
          rollover_ev.rollover_down_bid = hot.down_bid;
          rollover_ev.rollover_down_ask = hot.down_ask;
        }

        std::string err_up, err_dn;
        if (feed_up.switch_token(d2.up_token_id, &err_up) &&
            feed_down.switch_token(d2.down_token_id, &err_dn)) {
          current_slug = d2.confirmed_slug;
          active_epoch = d2.bucket_epoch_seconds;
          if (active_epoch < 0 && !parse_epoch_from_confirmed_slug(current_slug, &active_epoch)) {
            { ll::io::SyncCerrLock _; std::cerr << "[paper_trader][rollover] epoch missing from slug\n"; }
          }
          next_epoch = active_epoch + 300;
          feed_up.set_market_context(current_slug, active_epoch, "Up");
          feed_down.set_market_context(current_slug, active_epoch, "Down");
          push_event(std::move(rollover_ev));
          {
            ll::io::SyncCerrLock _;
            std::cerr << '[' << src_tag << "][rollover] active_epoch=" << active_epoch << " slug=" << current_slug
                      << " price_to_beat=";
            if (d2.gamma_has_price_to_beat && bucket_disc_has_strike(d2)) {
              std::cerr << d2.price_to_beat << " (gamma_metadata)";
            } else if (d2.strike_from_polymarket_web_event_page) {
              std::cerr << d2.price_to_beat << " (polymarket_web_event_page)";
            } else if (d2.strike_from_polymarket_rtds_chainlink) {
              std::cerr << d2.price_to_beat << " (polymarket_rtds_chainlink)";
            } else {
              std::cerr << "(strike_unavailable)";
            }
            std::cerr << '\n';
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  feed_up.stop();
  if (poly_discover) feed_down.stop();
  if (bin_thread.joinable()) bin_thread.join();
  // Stop consumer thread after feeds stop.
  {
    Event e;
    e.type = Event::Type::Stop;
    push_event(std::move(e));
  }
  if (consumer_thr.joinable()) consumer_thr.join();
  { ll::io::SyncCerrLock _; std::cerr << "telemetry: " << tel.summary() << "\n"; }
  return 0;
}

}  // namespace

namespace ll::btc_poly {

int run_strategy_main(int argc, char** argv, bool live_execution) {
#ifndef LL_ENABLE_LIVE_TRADER
  if (live_execution) {
    { ll::io::SyncCerrLock _; std::cerr << "[btc_poly] live execution requires CMake -DBUILD_LIVE_TRADER=ON\n"; }
    return 2;
  }
#endif
  return run_live_impl(live_execution, argc, argv);
}

}  // namespace ll::btc_poly
