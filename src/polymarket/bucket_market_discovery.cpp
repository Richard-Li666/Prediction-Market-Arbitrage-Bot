#include "polymarket/bucket_market_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <curl/curl.h>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>

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
  const std::string url = "https://gamma-api.polymarket.com/events?slug=" + slug;
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
  const std::int64_t base = current_bucket_epoch_seconds();
  const std::int64_t offsets[3] = {0, 300, -300};
  std::string last_err;
  for (std::int64_t off : offsets) {
    const std::int64_t bucket = base + off;
    const std::string slug = slug_for_bucket(bucket);
    std::string err;
    if (try_slug(slug, out, &err)) {
      return true;
    }
    last_err = std::move(err);
  }
  if (error_message) {
    *error_message = "no confirmed btc-updown-5m market in buckets [base, base+300, base-300]: " + last_err;
  }
  return false;
}

}  // namespace ll::polymarket
