#include <iostream>
#include <string>

#include "execution/executor.hpp"

int main() {
  ll::execution::LiveExecutor ex;
  ll::execution::OrderIntent o;
  o.mono_ns = 0;
  o.side = "BUY";
  o.limit_price = 0.0;
  o.qty = 0.0;
  o.market_token_id = "demo";

  std::string err;
  const bool ok = ex.submit(o, &err);
  std::cout << "submit_ok=" << (ok ? "true" : "false") << "\n";
  std::cout << err << "\n";
  return ok ? 0 : 2;
}
