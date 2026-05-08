#include "polymarket/bucket_market_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <curl/curl.h>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

#include <cmath>

#include <nlohmann/json.hpp>

namespace ll::polymarket {

namespace {

std::once_flag curl_once;

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* body = static_cast<std::string*>(userdata);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool http_get(const std::string& url, std::string* body, std::string* err) {
  std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CURL* curl = curl_easy_init();
  if (!curl) {
    if (err) {
      *err = "curl_easy_init failed";
    }
    return false;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "leadlag-course/1.0");
  const CURLcode res = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);
  if (res != CURLE_OK) {
    if (err) {
      *err = curl_easy_strerror(res);
    }
    return false;
  }
  if (code < 200 || code >= 300) {
    if (err) {
      *err = "HTTP " + std::to_string(code);
    }
    return false;
  }
  return true;
}

nlohmann::json parse_maybe_json_array_field(const std::string& raw) {
  try {
    return nlohmann::json::parse(raw);
  } catch (...) {
    return nlohmann::json::array();
  }
}

std::vector<std::string> outcomes_from_market(const nlohmann::json& m) {
  if (!m.contains("outcomes")) {
    return {};
  }
  const auto& o = m.at("outcomes");
  if (o.is_array()) {
    std::vector<std::string> r;
    for (const auto& x : o) {
      if (x.is_string()) {
        r.push_back(x.get<std::string>());
      }
    }
    return r;
  }
  if (o.is_string()) {
    const auto arr = parse_maybe_json_array_field(o.get<std::string>());
    std::vector<std::string> r;
    for (const auto& x : arr) {
      if (x.is_string()) {
        r.push_back(x.get<std::string>());
      }
    }
    return r;
  }
  return {};
}

std::vector<std::string> tokens_from_market(const nlohmann::json& m) {
  std::vector<std::string> ids;
  if (m.contains("clobTokenIds")) {
    const auto& t = m.at("clobTokenIds");
    if (t.is_array()) {
      for (const auto& x : t) {
        if (x.is_string()) {
          ids.push_back(x.get<std::string>());
        }
      }
      return ids;
    }
    if (t.is_string()) {
      const auto arr = parse_maybe_json_array_field(t.get<std::string>());
      for (const auto& x : arr) {
        if (x.is_string()) {
          ids.push_back(x.get<std::string>());
        }
      }
      return ids;
    }
  }
  return ids;
}

std::string json_scalar_id_string(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) {
    return {};
  }
  const auto& v = j.at(key);
  if (v.is_string()) {
    return v.get<std::string>();
  }
  if (v.is_number_unsigned()) {
    return std::to_string(v.get<std::uint64_t>());
  }
  if (v.is_number_integer()) {
    return std::to_string(v.get<std::int64_t>());
  }
  return {};
}

bool index_up_outcome(const std::vector<std::string>& outcomes, std::size_t* out_i) {
  for (std::size_t i = 0; i < outcomes.size(); ++i) {
    if (lower(outcomes[i]) == "up") {
      *out_i = i;
      return true;
    }
  }
  return false;
}

bool index_down_outcome(const std::vector<std::string>& outcomes, std::size_t* out_i) {
  for (std::size_t i = 0; i < outcomes.size(); ++i) {
    if (lower(outcomes[i]) == "down") {
      *out_i = i;
      return true;
    }
  }
  return false;
}

/// `current_ts = floor(now_sec / 300) * 300` (deterministic 5-minute UTC wall bucket).
std::int64_t current_bucket_epoch_seconds() {
  const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return (static_cast<std::int64_t>(now_s) / 300) * 300;
}

std::string slug_for_bucket(std::int64_t bucket_ts) {
  return "btc-updown-5m-" + std::to_string(bucket_ts);
}

bool fetch_events_for_slug(const std::string& slug, nlohmann::json* out_events_array, std::string* err) {
  const std::string url =
      "https://gamma-api.polymarket.com/events?slug=" + slug + "&includeMetadata=true";
  std::string body;
  if (!http_get(url, &body, err)) {
    return false;
  }
  try {
    const auto j = nlohmann::json::parse(body);
    if (!j.is_array()) {
      if (err) {
        *err = "gamma events response is not an array";
      }
      return false;
    }
    *out_events_array = std::move(j);
    return true;
  } catch (const std::exception& ex) {
    if (err) {
      *err = ex.what();
    }
    return false;
  }
}

const nlohmann::json* find_market_for_slug(const nlohmann::json& event, const std::string& want_slug) {
  if (!event.contains("markets") || !event["markets"].is_array()) {
    return nullptr;
  }
  for (const auto& m : event["markets"]) {
    if (!m.is_object()) {
      continue;
    }
    if (m.value("slug", std::string{}) == want_slug) {
      return &m;
    }
  }
  if (event["markets"].empty()) {
    return nullptr;
  }
  const auto& only = event["markets"][0];
  return only.is_object() ? &only : nullptr;
}

bool fetch_events_by_id_for_metadata(const std::string& event_id, nlohmann::json* out_events_array,
                                    std::string* err) {
  if (event_id.empty()) {
    if (err) {
      *err = "gamma event id empty";
    }
    return false;
  }
  const std::string url =
      "https://gamma-api.polymarket.com/events?id=" + event_id + "&includeMetadata=true";
  std::string body;
  if (!http_get(url, &body, err)) {
    return false;
  }
  try {
    const auto j = nlohmann::json::parse(body);
    if (!j.is_array()) {
      if (err) {
        *err = "gamma events?id response is not an array";
      }
      return false;
    }
    *out_events_array = std::move(j);
    return true;
  } catch (const std::exception& ex) {
    if (err) {
      *err = ex.what();
    }
    return false;
  }
}

bool event_metadata_object(const nlohmann::json& event, nlohmann::json* out_em, std::string* err) {
  if (!event.contains("eventMetadata")) {
    if (err) {
      *err = "gamma event missing eventMetadata";
    }
    return false;
  }
  const auto& meta = event.at("eventMetadata");
  if (meta.is_null()) {
    if (err) {
      *err = "gamma eventMetadata is null";
    }
    return false;
  }
  if (meta.is_object()) {
    *out_em = meta;
    return true;
  }
  if (meta.is_string()) {
    try {
      const auto parsed = nlohmann::json::parse(meta.get<std::string>());
      if (!parsed.is_object()) {
        if (err) {
          *err = "gamma eventMetadata string is not a JSON object";
        }
        return false;
      }
      *out_em = std::move(parsed);
      return true;
    } catch (const std::exception& ex) {
      if (err) {
        *err = std::string("gamma eventMetadata string parse failed: ") + ex.what();
      }
      return false;
    }
  }
  if (err) {
    *err = "gamma eventMetadata has unsupported type";
  }
  return false;
}

bool extract_price_to_beat_from_gamma_event(const nlohmann::json& event, double* out_ptb,
                                            std::string* err) {
  nlohmann::json em;
  if (!event_metadata_object(event, &em, err)) {
    return false;
  }
  if (!em.contains("priceToBeat")) {
    if (err) {
      *err = "gamma eventMetadata missing priceToBeat";
    }
    return false;
  }
  const auto& v = em.at("priceToBeat");
  double ptb = 0.0;
  try {
    if (v.is_number()) {
      ptb = v.get<double>();
    } else if (v.is_string()) {
      ptb = std::stod(v.get<std::string>());
    } else {
      if (err) {
        *err = "gamma priceToBeat has unsupported JSON type";
      }
      return false;
    }
  } catch (...) {
    if (err) {
      *err = "failed to parse priceToBeat as number";
    }
    return false;
  }
  if (!std::isfinite(ptb) || ptb <= 0.0) {
    if (err) {
      *err = "gamma priceToBeat invalid (non-finite or <= 0)";
    }
    return false;
  }
  *out_ptb = ptb;
  return true;
}

/// Parses Gamma ISO timestamps like `2026-05-06T23:05:00Z` / fractional seconds into unix milliseconds (UTC).
bool gamma_iso8601_utc_to_unix_ms(const std::string& in, std::int64_t* out_ms) {
  std::string s = in;
  while (!s.empty() && (s.back() == 'Z' || s.back() == 'z')) {
    s.pop_back();
  }
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  double sec_f = 0.0;
  const int n = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &year, &month, &day, &hour, &minute, &sec_f);
  if (n != 6 || year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) {
    return false;
  }
  const int sec_whole = static_cast<int>(sec_f);
  const double frac = sec_f - static_cast<double>(sec_whole);
  int frac_ms = static_cast<int>(std::lround(frac * 1000.0));
  if (frac_ms >= 1000) {
    frac_ms = 999;
  }
  if (frac_ms < 0) {
    frac_ms = 0;
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = sec_whole;
  tm.tm_isdst = 0;
#if defined(_WIN32)
  const std::time_t tt = _mkgmtime(&tm);
#else
  const std::time_t tt = timegm(&tm);
#endif
  if (tt == static_cast<std::time_t>(-1)) {
    return false;
  }
  *out_ms = static_cast<std::int64_t>(tt) * 1000LL + static_cast<std::int64_t>(frac_ms);
  return true;
}

bool parse_bucket_epoch_from_slug(const std::string& slug, std::int64_t* out_epoch) {
  static constexpr char kPrefix[] = "btc-updown-5m-";
  constexpr std::size_t plen = sizeof(kPrefix) - 1;
  const std::string ls = lower(slug);
  const std::string lp = lower(std::string(kPrefix));
  if (ls.size() <= plen || ls.compare(0, plen, lp) != 0) {
    return false;
  }
  try {
    *out_epoch = std::stoll(slug.substr(plen));
    return true;
  } catch (...) {
    return false;
  }
}

bool validate_and_extract(const nlohmann::json& event, const std::string& want_slug,
                          BtcFiveMinuteBucketDiscovery& out, std::string* err) {
  out.event_start_wall_ms = -1;
  out.strike_from_polymarket_rtds_chainlink = false;
  if (event.value("slug", std::string{}) != want_slug) {
    if (err) {
      *err = "event slug mismatch after fetch";
    }
    return false;
  }
  if (!event.value("active", false) || event.value("closed", true)) {
    if (err) {
      *err = "event not active or already closed";
    }
    return false;
  }

  const nlohmann::json* mp = find_market_for_slug(event, want_slug);
  if (mp == nullptr) {
    if (err) {
      *err = "no market object in gamma event";
    }
    return false;
  }
  const nlohmann::json& m = *mp;
  if (!m.value("active", false) || m.value("closed", true)) {
    if (err) {
      *err = "market not active or closed";
    }
    return false;
  }

  const auto outs = outcomes_from_market(m);
  const auto toks = tokens_from_market(m);
  bool has_up = false;
  bool has_down = false;
  for (const auto& o : outs) {
    const auto lo = lower(o);
    if (lo == "up") {
      has_up = true;
    }
    if (lo == "down") {
      has_down = true;
    }
  }
  if (!has_up || !has_down || outs.size() != toks.size() || toks.empty()) {
    if (err) {
      *err = "outcomes/tokens missing or not Up/Down aligned";
    }
    return false;
  }

  std::size_t up_i = 0;
  std::size_t down_i = 0;
  if (!index_up_outcome(outs, &up_i) || !index_down_outcome(outs, &down_i) || up_i >= toks.size() ||
      down_i >= toks.size()) {
    if (err) {
      *err = "cannot resolve Up/Down token indices";
    }
    return false;
  }

  out.confirmed_slug = want_slug;
  out.up_token_id = toks[up_i];
  out.down_token_id = toks[down_i];
  out.condition_id = m.value("conditionId", std::string{});
  out.market_numeric_id = json_scalar_id_string(m, "id");
  std::int64_t be = -1;
  if (!parse_bucket_epoch_from_slug(want_slug, &be)) {
    out.bucket_epoch_seconds = -1;
  } else {
    out.bucket_epoch_seconds = be;
  }

  std::string start_iso = m.value("eventStartTime", std::string{});
  if (start_iso.empty()) {
    start_iso = event.value("startTime", std::string{});
  }
  if (!start_iso.empty()) {
    std::int64_t pms = -1;
    if (gamma_iso8601_utc_to_unix_ms(start_iso, &pms)) {
      out.event_start_wall_ms = pms;
    }
  }
  if (out.event_start_wall_ms < 0 && out.bucket_epoch_seconds >= 0) {
    out.event_start_wall_ms = out.bucket_epoch_seconds * 1000;
  }

  // Prefer Hydration-aligned metadata: slug list responses can omit or lag vs `events?id=`.
  const std::string event_id = json_scalar_id_string(event, "id");
  nlohmann::json refreshed_by_id;
  const nlohmann::json* strike_source = &event;
  if (!event_id.empty() &&
      fetch_events_by_id_for_metadata(event_id, &refreshed_by_id, nullptr)) {
    if (refreshed_by_id.is_array() && !refreshed_by_id.empty() &&
        refreshed_by_id[0].is_object()) {
      const auto& ev2 = refreshed_by_id[0];
      if (ev2.value("slug", std::string{}) == want_slug) {
        strike_source = &ev2;
      }
    }
  }
  double ptb = 0.0;
  std::string ptb_err;
  if (extract_price_to_beat_from_gamma_event(*strike_source, &ptb, &ptb_err)) {
    out.price_to_beat = ptb;
    out.gamma_has_price_to_beat = true;
    out.strike_from_polymarket_rtds_chainlink = false;
  } else {
    // Gamma often omits `eventMetadata` on `/events?slug=` and `/events?id=`; tokens still validate.
    out.price_to_beat = 0.0;
    out.gamma_has_price_to_beat = false;
    out.strike_from_polymarket_rtds_chainlink = false;
    if (err) {
      *err = std::move(ptb_err);
    }
  }
  return true;
}

bool try_slug(const std::string& slug, BtcFiveMinuteBucketDiscovery& out, std::string* err) {
  nlohmann::json arr;
  if (!fetch_events_for_slug(slug, &arr, err)) {
    return false;
  }
  if (arr.empty()) {
    if (err) {
      *err = "empty events list for slug";
    }
    return false;
  }
  const auto& ev = arr[0];
  if (!ev.is_object()) {
    if (err) {
      *err = "first event is not an object";
    }
    return false;
  }
  return validate_and_extract(ev, slug, out, err);
}

}  // namespace

bool discover_btc_updown_5m_for_exact_slug(const std::string& slug,
                                         BtcFiveMinuteBucketDiscovery& out,
                                         std::string* error_message) {
  std::string err;
  if (!try_slug(slug, out, &err)) {
    if (error_message) {
      *error_message = std::move(err);
    }
    return false;
  }
  return true;
}

bool discover_active_btc_updown_5m_via_bucket(BtcFiveMinuteBucketDiscovery& out, std::string* error_message) {
  const std::int64_t now_s = static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const std::int64_t base = (now_s / 300) * 300;
  // Gamma may briefly lag on the new bucket; avoid accepting base-300 when it is still marked active
  // after wall clock has entered [base, base+300) — that breaks warmup (shows ended slug) and rollover
  // (next_epoch already past; rollover guard never sees d2.bucket_epoch >= next_epoch).
  const std::int64_t offsets[3] = {0, 300, -300};
  std::string last_err;
  for (std::int64_t off : offsets) {
    const std::int64_t bucket = base + off;
    const std::string slug = slug_for_bucket(bucket);
    std::string err;
    if (!try_slug(slug, out, &err)) {
      last_err = std::move(err);
      continue;
    }
    std::int64_t epoch = out.bucket_epoch_seconds;
    if (epoch < 0 && !parse_bucket_epoch_from_slug(out.confirmed_slug, &epoch)) {
      last_err = "confirmed slug missing bucket epoch";
      continue;
    }
    if (now_s >= epoch && now_s < epoch + 300) {
      out.bucket_epoch_seconds = epoch;
      return true;
    }
    last_err = "gamma returned active market for " + slug + " but wall clock is outside [epoch, epoch+300)";
  }
  if (error_message) {
    *error_message =
        "no btc-updown-5m market matching wall-clock bucket window [epoch, epoch+300): " + last_err;
  }
  return false;
}

}  // namespace ll::polymarket
