cmake_minimum_required(VERSION 3.16)
project(leadlag LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_LIVE_TRADER "Build live execution binary (requires API keys; off by default)" OFF)
option(BUILD_DAEMON_GARCH "Compile poly_daemon GARCH(1,1) sigma client for paper/live strategy (needs arch in venv)" ON)

include(FetchContent)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++20")
set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
check_cxx_source_compiles("
#include <thread>
int main() {
  std::jthread j([]{});
  return 0;
}
" LEADLAG_HAVE_STD_JTHREAD)
if(LEADLAG_HAVE_STD_JTHREAD)
  set(LEADLAG_POOL_USE_JTHREAD 1)
else()
  set(LEADLAG_POOL_USE_JTHREAD 0)
endif()
if(LEADLAG_HAVE_STD_JTHREAD)
  message(STATUS "std::jthread available: YES — ThreadPool uses std::jthread (assignment-aligned)")
else()
  message(STATUS "std::jthread available: NO — ThreadPool falls back to std::thread (same dispatch/Task API); use GCC 11+ / recent Clang for jthread")
endif()

set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v11.4.6
)
FetchContent_MakeAvailable(ixwebsocket)

find_package(CURL REQUIRED)

add_library(ll_core STATIC
  src/core/thread_pool.cpp
)
target_include_directories(ll_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ll_core PUBLIC Threads::Threads)
target_compile_definitions(ll_core PUBLIC LEADLAG_POOL_USE_JTHREAD=${LEADLAG_POOL_USE_JTHREAD})

add_library(ll_telemetry STATIC
  src/telemetry/pipeline.cpp
)
target_include_directories(ll_telemetry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ll_telemetry PUBLIC ll_core)

add_library(ll_logging STATIC
  src/logging/jsonl_writer.cpp
)
target_include_directories(ll_logging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ll_logging PUBLIC ll_core nlohmann_json::nlohmann_json)

add_library(ll_binance STATIC
  src/binance/stream_client.cpp
  src/binance/stream_env.cpp
)
target_include_directories(ll_binance PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ll_binance PUBLIC ll_core ll_telemetry ll_logging nlohmann_json::nlohmann_json ixwebsocket::ixwebsocket)

add_library(ll_polymarket STATIC
  src/polymarket/rest_quote.cpp
  src/polymarket/ws_market_client.cpp
  src/polymarket/gamma_bootstrap.cpp
  src/polymarket/bucket_market_discovery.cpp
  src/polymarket/rtds_chainlink_strike.cpp
  src/polymarket/rtds_chainlink_feed.cpp
  src/polymarket/polymarket_web_ptb.cpp
  src/polymarket/poly_ws_quote_feed.cpp
  src/polymarket/ws_fixed_token_feed.cpp
)
target_include_directories(ll_polymarket PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ll_polymarket PUBLIC ll_core ll_telemetry ll_logging nlohmann_json::nlohmann_json CURL::libcurl ixwebsocket::ixwebsocket)

add_library(ll_replay STATIC
  src/replay/timeline_merge.cpp
)
target_include_directories(ll_replay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ll_replay PUBLIC ll_core nlohmann_json::nlohmann_json)

add_library(ll_signals STATIC
  src/signals/engine.cpp
)
target_include_directories(ll_signals PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ll_signals PUBLIC ll_core nlohmann_json::nlohmann_json)

add_library(ll_sim STATIC
  src/sim/latency_backtest.cpp
)
target_include_directories(ll_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ll_sim PUBLIC ll_core ll_signals nlohmann_json::nlohmann_json)

add_library(ll_execution STATIC
  src/execution/paper_executor.cpp
  src/execution/live_executor.cpp
  src/execution/poly_daemon_client.cpp
)
target_include_directories(ll_execution PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ll_execution PUBLIC ll_core nlohmann_json::nlohmann_json)
if(BUILD_LIVE_TRADER)
  target_compile_definitions(ll_execution PUBLIC LL_ENABLE_LIVE_TRADER=1)
endif()
if(BUILD_LIVE_TRADER OR BUILD_DAEMON_GARCH)
  target_compile_definitions(ll_execution PUBLIC LL_ENABLE_GARCH_DAEMON=1)
endif()

function(ll_app name)
  add_executable(${name} apps/${name}.cpp)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
endfunction()

ll_app(binance_ws_probe)
target_link_libraries(binance_ws_probe PRIVATE ll_binance ll_core ll_telemetry)

ll_app(binance_logger)
target_link_libraries(binance_logger PRIVATE ll_binance ll_logging ll_core ll_telemetry)

ll_app(polymarket_quote_probe)
target_link_libraries(polymarket_quote_probe PRIVATE ll_polymarket ll_core ll_telemetry)

ll_app(rtds_chainlink_probe)
target_link_libraries(rtds_chainlink_probe PRIVATE ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json)

ll_app(polymarket_logger)
target_link_libraries(polymarket_logger PRIVATE ll_polymarket ll_logging ll_core ll_telemetry)

ll_app(replay_align)
target_link_libraries(replay_align PRIVATE ll_replay ll_core)

ll_app(signal_smoke)
target_link_libraries(signal_smoke PRIVATE ll_signals ll_replay ll_core)

ll_app(backtest_latency_sweep)
target_link_libraries(backtest_latency_sweep PRIVATE ll_sim ll_signals ll_replay ll_core nlohmann_json::nlohmann_json)

ll_app(live_pipeline)
target_link_libraries(live_pipeline PRIVATE ll_binance ll_polymarket ll_logging ll_telemetry ll_core)

add_executable(paper_trader apps/paper_trader.cpp apps/btc_poly_runner.cpp)
target_include_directories(paper_trader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(paper_trader PRIVATE ll_execution ll_sim ll_signals ll_replay ll_binance ll_polymarket ll_telemetry ll_core nlohmann_json::nlohmann_json)

add_executable(live_trader apps/live_trader.cpp apps/btc_poly_runner.cpp)
target_include_directories(live_trader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(live_trader PRIVATE ll_execution ll_sim ll_signals ll_replay ll_binance ll_polymarket ll_telemetry ll_core nlohmann_json::nlohmann_json)
if(BUILD_LIVE_TRADER)
  target_compile_definitions(live_trader PRIVATE LL_ENABLE_LIVE_TRADER=1)
endif()

enable_testing()

add_executable(thread_pool_smoke tests/thread_pool_smoke.cpp)
target_include_directories(thread_pool_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(thread_pool_smoke PRIVATE ll_core Threads::Threads)
add_test(NAME thread_pool_smoke COMMAND thread_pool_smoke)
