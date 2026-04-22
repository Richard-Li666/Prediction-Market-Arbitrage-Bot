#include "polymarket/gamma_bootstrap.hpp"

#include <algorithm>
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

/// Gamma slug must begin with `prefix` (ASCII), compared case-insensitively — targets one market family only.
bool slug_has_prefix_ci(const std::string& slug, const std::string& prefix) {
  if (prefix.empty()) {
    return true;
  }
  const std::string ls = lower(slug);
  const std::string lp = lower(prefix);
  if (ls.size() < lp.size()) {
    return false;
  }
  return ls.compare(0, lp.size(), lp) == 0;
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

std::size_t index_up_outcome(const std::vector<std::string>& outcomes) {
  for (std::size_t i = 0; i < outcomes.size(); ++i) {
    if (lower(outcomes[i]) == "up") {
      return i;
    }
  }
  return 0;
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
  if (v.is_number_float()) {
    std::ostringstream oss;
    oss.precision(20);
    oss << v.get<double>();
    return oss.str();
  }
  return {};
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

}  // namespace

bool gamma_discover_active_market(const GammaBootstrapParams& params,
                                  PolymarketMarketRef& out,
                                  std::string* error_message) {
  std::string body;
  const std::string url =
      "https://gamma-api.polymarket.com/markets?active=true&closed=false&limit=500";
  if (!http_get(url, &body, error_message)) {
    return false;
  }

  try {
    const auto arr = nlohmann::json::parse(body);
    if (!arr.is_array()) {
      if (error_message) {
        *error_message = "gamma response not array";
      }
      return false;
    }

    std::vector<nlohmann::json> candidates;
    for (const auto& m : arr) {
      if (!m.is_object()) {
        continue;
      }
      const std::string slug = m.value("slug", std::string{});
      if (!slug_has_prefix_ci(slug, params.slug_prefix)) {
        continue;
      }
      const auto outs = outcomes_from_market(m);
      if (params.require_up_down_outcomes) {
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
        if (!(has_up && has_down)) {
          continue;
        }
      }
      candidates.push_back(m);
    }

    if (candidates.empty()) {
      if (error_message) {
        *error_message = "no matching active market in gamma response";
      }
      return false;
    }

    // Pick the candidate with the lexicographically largest endDate (ISO) if present, else first.
    std::sort(candidates.begin(), candidates.end(),
              [](const nlohmann::json& a, const nlohmann::json& b) {
                return a.value("endDate", std::string{}) < b.value("endDate", std::string{});
              });
    const auto& m = candidates.back();

    const auto outs = outcomes_from_market(m);
    const auto toks = tokens_from_market(m);
    if (toks.empty() || outs.size() != toks.size()) {
      if (error_message) {
        *error_message = "cannot align outcomes with clob token ids";
      }
      return false;
    }

    const std::size_t up_i = index_up_outcome(outs);
    std::size_t down_i = 0;
    for (std::size_t i = 0; i < outs.size(); ++i) {
      if (lower(outs[i]) == "down") {
        down_i = i;
        break;
      }
    }

    out.market_id = json_scalar_id_string(m, "id");
    out.condition_id = m.value("conditionId", std::string{});
    out.slug = m.value("slug", std::string{});
    out.active = m.value("active", true);
    out.start_time = m.value("startDate", std::string{});
    out.end_time = m.value("endDate", std::string{});
    out.up_token_id = toks.at(up_i);
    out.down_token_id = toks.at(down_i);
    return true;
  } catch (const std::exception& ex) {
    if (error_message) {
      *error_message = ex.what();
    }
    return false;
  }
}

}  // namespace ll::polymarket
