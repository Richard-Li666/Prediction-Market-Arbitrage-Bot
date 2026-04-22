cmake_minimum_required(VERSION 3.16)
project(leadlag LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_LIVE_TRADER "Build live execution binary (requires API keys; off by default)" OFF)

include(FetchContent)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

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
)
target_include_directories(ll_execution PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ll_execution PUBLIC ll_core)

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

ll_app(paper_trader)
target_link_libraries(paper_trader PRIVATE ll_execution ll_sim ll_signals ll_replay ll_core nlohmann_json::nlohmann_json)

ll_app(live_trader)
target_link_libraries(live_trader PRIVATE ll_execution ll_core)
if(BUILD_LIVE_TRADER)
  target_compile_definitions(live_trader PRIVATE LL_ENABLE_LIVE_TRADER=1)
endif()
