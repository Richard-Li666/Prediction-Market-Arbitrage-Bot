#include "polymarket/polymarket_web_ptb.hpp"

#include "polymarket/bucket_market_discovery.hpp"

#include <cmath>
#include <curl/curl.h>
#include <mutex>
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

bool http_get_binary(const std::string& url, std::string* body, std::string* err) {
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
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl,
                   CURLOPT_USERAGENT,
                   "Mozilla/5.0 (compatible; leadlag-course/1.0; +polymarket-strike-fetch)");
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

bool extract_next_data_json(const std::string& html, std::string* out_json, std::string* err) {
  static constexpr char kTag[] = "<script id=\"__NEXT_DATA__\"";
  auto pos = html.find(kTag);
  if (pos == std::string::npos) {
    if (err) {
      *err = "__NEXT_DATA__ script tag not found";
    }
    return false;
  }
  pos = html.find('>', pos);
  if (pos == std::string::npos) {
    if (err) {
      *err = "__NEXT_DATA__ malformed";
    }
    return false;
  }
  ++pos;
  const auto end = html.find("</script>", pos);
  if (end == std::string::npos) {
    if (err) {
      *err = "__NEXT_DATA__ closing </script> not found";
    }
    return false;
  }
  *out_json = html.substr(pos, end - pos);
  return true;
}

bool json_to_positive_double(const nlohmann::json& j, double* out) {
  if (j.is_number()) {
    *out = j.get<double>();
    return std::isfinite(*out) && *out > 0.0;
  }
  if (j.is_string()) {
    try {
      *out = std::stod(j.get<std::string>());
      return std::isfinite(*out) && *out > 0.0;
    } catch (...) {
      return false;
    }
  }
  return false;
}

bool ptb_from_event_obj(const nlohmann::json& ev, const std::string& want_slug, double* out_ptb) {
  if (!ev.is_object()) {
    return false;
  }
  if (!ev.contains("slug") || !ev["slug"].is_string()) {
    return false;
  }
  if (ev["slug"].get<std::string>() != want_slug) {
    return false;
  }
  if (!ev.contains("eventMetadata") || !ev["eventMetadata"].is_object()) {
    return false;
  }
  const auto& em = ev["eventMetadata"];
  if (!em.contains("priceToBeat")) {
    return false;
  }
  return json_to_positive_double(em["priceToBeat"], out_ptb);
}

bool scan_next_data_for_slug_ptb(const nlohmann::json& root, const std::string& want_slug,
                                 double* out_ptb) {
  try {
    const auto& qs = root.at("props").at("pageProps").at("dehydratedState").at("queries");
    if (!qs.is_array()) {
      return false;
    }
    for (const auto& q : qs) {
      if (!q.contains("state") || !q["state"].contains("data")) {
        continue;
      }
      const auto& data = q["state"]["data"];
      if (!data.is_object()) {
        continue;
      }
      if (ptb_from_event_obj(data, want_slug, out_ptb)) {
        return true;
      }
      if (data.contains("events") && data["events"].is_array()) {
        for (const auto& ev : data["events"]) {
          if (ptb_from_event_obj(ev, want_slug, out_ptb)) {
            return true;
          }
        }
      }
    }
  } catch (...) {
    return false;
  }
  return false;
}

bool fetch_price_to_beat_from_event_html(const std::string& slug, double* out_ptb, std::string* err) {
  const std::string url = "https://polymarket.com/event/" + slug;
  std::string html;
  if (!http_get_binary(url, &html, err)) {
    return false;
  }
  std::string raw_json;
  if (!extract_next_data_json(html, &raw_json, err)) {
    return false;
  }
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(raw_json);
  } catch (const std::exception& ex) {
    if (err) {
      *err = std::string("__NEXT_DATA__ JSON parse failed: ") + ex.what();
    }
    return false;
  }
  if (!scan_next_data_for_slug_ptb(root, slug, out_ptb)) {
    if (err) {
      *err = "priceToBeat not found for slug " + slug + " in __NEXT_DATA__";
    }
    return false;
  }
  return true;
}

}  // namespace

bool fill_strike_from_polymarket_web_event_page(BtcFiveMinuteBucketDiscovery& disc,
                                                std::string* error_message) {
  if (disc.gamma_has_price_to_beat && std::isfinite(disc.price_to_beat) && disc.price_to_beat > 0.0) {
    return true;
  }
  if (disc.confirmed_slug.empty()) {
    if (error_message) {
      *error_message = "confirmed_slug empty";
    }
    return false;
  }
  double ptb = 0.0;
  std::string local_err;
  if (!fetch_price_to_beat_from_event_html(disc.confirmed_slug, &ptb,
                                           error_message ? error_message : &local_err)) {
    return false;
  }
  disc.price_to_beat = ptb;
  disc.strike_from_polymarket_web_event_page = true;
  disc.strike_from_polymarket_rtds_chainlink = false;
  return true;
}

}  // namespace ll::polymarket
