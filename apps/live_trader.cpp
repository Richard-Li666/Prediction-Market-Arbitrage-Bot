#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <cstring>
#include <string>

#include "btc_poly_runner.hpp"
#include "execution/executor.hpp"

namespace {

void trim_inplace(std::string& s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.erase(s.begin());
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }
}

// Load ./.env from cwd if present; only sets keys that are not already in the environment.
void load_dotenv_if_present() {
  std::ifstream in(".env");
  if (!in) {
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    trim_inplace(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    if (line.rfind("export ", 0) == 0) {
      line = line.substr(7);
      trim_inplace(line);
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    trim_inplace(key);
    trim_inplace(val);
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
      val = val.substr(1, val.size() - 2);
    }
    if (key.empty() || std::getenv(key.c_str()) != nullptr) {
      continue;
    }
    setenv(key.c_str(), val.c_str(), 1);
  }
}

bool env_flag_true(const char* key) {
  const char* v = std::getenv(key);
  if (!v || !v[0]) {
    return false;
  }
  if (std::strcmp(v, "1") == 0) {
    return true;
  }
  std::string s(v);
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s == "true" || s == "yes";
}

}  // namespace

static void usage() {
  std::cerr << "usage: live_trader [--token-id TOKEN] --side BUY|SELL [--market | --price P] (--qty Q | --notional USD)\n"
               "  If --token-id omitted, reads env POLY_TOKEN_ID (set in .env).\n"
               "  --market         FOK market order (no --price). BUY: use --notional USD; SELL: use --qty shares.\n"
               "  --notional USD   BUY limit: qty = notional / price; BUY market: spend ~USD (with --market).\n"
               "  --spend USD      alias for --notional\n"
               "  --dry-run        Validate only; print intent; do not start daemon or submit.\n"
               "  --confirm        Required for real submit when POLY_REQUIRE_CONFIRM=1 (also -y / --yes).\n"
               "env:\n"
               "  POLY_DAEMON_CMD: command to start python daemon, e.g.\n"
               "    POLY_DAEMON_CMD='python3 -u poly_daemon.py'\n"
               "  POLY_TOKEN_ID: default outcome token id when --token-id not passed\n"
               "  POLY_REQUIRE_CONFIRM=1: refuse live submit unless --confirm / -y / --yes (ignored with --dry-run).\n"
               "  Also loads ./.env from current directory when variables are unset.\n"
               "strategy mode:\n"
               "  live_trader --strategy [same options as paper_trader --live]\n"
               "    Runs the BTC 5m bucket strategy with real market orders (needs BUILD_LIVE_TRADER=ON).\n"
               "note:\n"
               "  build with -DBUILD_LIVE_TRADER=ON\n";
}

int main(int argc, char** argv) {
  load_dotenv_if_present();

  if (argc >= 2 && std::string(argv[1]) == "--strategy") {
    return ll::btc_poly::run_strategy_main(argc, argv, true);
  }

  ll::execution::LiveExecutor ex;
  ll::execution::OrderIntent o;
  o.mono_ns = 0;
  double notional_usd = 0.0;
  bool have_qty = false;
  bool dry_run = false;
  bool confirm_flag = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--token-id" && i + 1 < argc) {
      o.market_token_id = argv[++i];
    } else if (a == "--side" && i + 1 < argc) {
      o.side = argv[++i];
    } else if (a == "--price" && i + 1 < argc) {
      o.limit_price = std::stod(argv[++i]);
    } else if (a == "--qty" && i + 1 < argc) {
      o.qty = std::stod(argv[++i]);
      have_qty = true;
    } else if ((a == "--notional" || a == "--spend") && i + 1 < argc) {
      notional_usd = std::stod(argv[++i]);
    } else if (a == "--market") {
      o.market_order = true;
    } else if (a == "--dry-run") {
      dry_run = true;
    } else if (a == "--confirm" || a == "-y" || a == "--yes") {
      confirm_flag = true;
    } else if (a == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "unknown arg: " << a << "\n";
      usage();
      return 2;
    }
  }

  if (o.market_token_id.empty()) {
    if (const char* tid = std::getenv("POLY_TOKEN_ID")) {
      o.market_token_id = tid;
    }
  }

  if (o.market_order) {
    if (o.side == "BUY") {
      if (notional_usd <= 0.0) {
        std::cerr << "market BUY requires --notional/--spend USD\n";
        return 2;
      }
      o.qty = notional_usd;
      std::cerr << "[live_trader] market BUY spend_usd=" << notional_usd << "\n";
    } else if (o.side == "SELL") {
      if (!have_qty || o.qty <= 0.0) {
        std::cerr << "market SELL requires --qty shares\n";
        return 2;
      }
      std::cerr << "[live_trader] market SELL shares=" << o.qty << "\n";
    } else {
      std::cerr << "--market requires side BUY or SELL\n";
      return 2;
    }
  } else if (notional_usd > 0.0) {
    if (o.side != "BUY") {
      std::cerr << "--notional/--spend only supported for side BUY\n";
      return 2;
    }
    if (o.limit_price <= 0.0) {
      usage();
      return 2;
    }
    o.qty = notional_usd / o.limit_price;
    std::cerr << "[live_trader] notional=" << notional_usd << " price=" << o.limit_price
              << " -> qty=" << o.qty << "\n";
  } else if (!have_qty || o.qty <= 0.0) {
    usage();
    return 2;
  }

  if (o.market_order) {
    if (o.market_token_id.empty()) {
      std::cerr << "missing outcome token: pass --token-id or set POLY_TOKEN_ID (e.g. in .env in cwd)\n";
      return 2;
    }
    if (o.side.empty() || o.qty <= 0.0) {
      usage();
      return 2;
    }
  } else {
    if (o.market_token_id.empty()) {
      std::cerr << "missing outcome token: pass --token-id or set POLY_TOKEN_ID (e.g. in .env in cwd)\n";
      return 2;
    }
    if (o.side.empty() || o.limit_price <= 0 || o.qty <= 0) {
      usage();
      return 2;
    }
  }

  if (env_flag_true("POLY_REQUIRE_CONFIRM") && !dry_run && !confirm_flag) {
    std::cerr << "live submit refused: POLY_REQUIRE_CONFIRM=1 requires --confirm (or -y / --yes). "
                 "Use --dry-run to preview without contacting the daemon.\n";
    return 2;
  }

  if (dry_run) {
    std::cout << "[dry-run] validated only; daemon not started; no order sent\n";
    std::cout << "  token_id=" << o.market_token_id << "\n";
    std::cout << "  side=" << o.side << "\n";
    std::cout << "  market_order=" << (o.market_order ? "true" : "false") << "\n";
    if (o.market_order) {
      std::cout << "  amount=" << o.qty;
      if (o.side == "BUY") {
        std::cout << " (USD spend)\n";
      } else {
        std::cout << " (shares)\n";
      }
    } else {
      std::cout << "  limit_price=" << o.limit_price << " size=" << o.qty << "\n";
    }
    return 0;
  }

  std::string err;
  std::string order_id;
  std::int64_t daemon_submit_latency_ns = -1;
  const bool ok = ex.submit(o, &err, &order_id, &daemon_submit_latency_ns);
  std::cout << "submit_ok=" << (ok ? "true" : "false") << "\n";
  if (!order_id.empty()) {
    std::cout << "order_id=" << order_id << "\n";
  }
  if (daemon_submit_latency_ns >= 0) {
    std::cout << "daemon_submit_latency_ms=" << (static_cast<double>(daemon_submit_latency_ns) / 1e6) << "\n";
  }
  if (!err.empty()) {
    std::cout << err << "\n";
  }
  return ok ? 0 : 2;
}
