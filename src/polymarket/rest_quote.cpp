#include "polymarket/rest_quote.hpp"

#include <curl/curl.h>

#include <mutex>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/clock.hpp"

namespace ll::polymarket {

namespace {

std::once_flag curl_once;

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* body = static_cast<std::string*>(userdata);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

double parse_price_field(const nlohmann::json& j) {
  if (j.is_string()) {
    return std::stod(j.get<std::string>());
  }
  if (j.is_number()) {
    return j.get<double>();
  }
  return 0.0;
}

}  // namespace

bool fetch_order_book(const RestQuoteConfig& cfg, core::BookQuote& out, std::string* error_message) {
  std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

  std::string body;
  CURL* curl = curl_easy_init();
  if (!curl) {
    if (error_message) {
      *error_message = "curl_easy_init failed";
    }
    return false;
  }

  char* esc = curl_easy_escape(curl, cfg.token_id.c_str(), static_cast<int>(cfg.token_id.size()));
  const std::string url =
      std::string("https://clob.polymarket.com/book?token_id=") + (esc ? esc : "");
  if (esc) {
    curl_free(esc);
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "leadlag-course/1.0");

  const CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    if (error_message) {
      *error_message = curl_easy_strerror(res);
    }
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    if (error_message) {
      *error_message = "HTTP " + std::to_string(http_code) + " body=" + body;
    }
    return false;
  }

  try {
    const auto j = nlohmann::json::parse(body);
    const auto& bids = j.at("bids");
    const auto& asks = j.at("asks");
    if (bids.empty() || asks.empty()) {
      if (error_message) {
        *error_message = "empty bids/asks";
      }
      return false;
    }
    out.token_id = cfg.token_id;
    out.local_mono_ns = core::steady_ns();
    out.local_wall_ms = core::system_ms();
    // CLOB returns bids low->high and asks high->low; top-of-book is last element.
    out.best_bid = parse_price_field(bids.back().at("price"));
    out.best_ask = parse_price_field(asks.back().at("price"));
    return true;
  } catch (const std::exception& ex) {
    if (error_message) {
      *error_message = ex.what();
    }
    return false;
  }
}

}  // namespace ll::polymarket
