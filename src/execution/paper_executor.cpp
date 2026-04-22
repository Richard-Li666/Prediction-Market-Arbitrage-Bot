#include "execution/executor.hpp"

#include <iostream>

namespace ll::execution {

void PaperExecutor::record_intent(const OrderIntent& o) {
  std::cout << "paper_order mono_ns=" << o.mono_ns << " side=" << o.side << " px=" << o.limit_price
            << " qty=" << o.qty << " token=" << o.market_token_id << "\n";
}

}  // namespace ll::execution
