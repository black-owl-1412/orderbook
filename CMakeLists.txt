cmake_minimum_required(VERSION 3.28)
project(orderbook VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / IDEs

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ORDERBOOK_BUILD_TESTS "Build the unit tests" ON)
option(ORDERBOOK_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ORDERBOOK_FLAT_LEVELS "Store price levels in std::flat_map (ON) or std::map (OFF)" ON)
option(ORDERBOOK_BUILD_BENCHMARKS "Build benchmarks (use a Release build for real numbers)" ON)

if(ORDERBOOK_SANITIZE)
  # Applied globally so the library, tests and any fetched deps agree.
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

# Warnings live on an interface target so they apply to our code only.
add_library(orderbook_warnings INTERFACE)
target_compile_options(orderbook_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)

# ---- core library ----
add_library(orderbook_core STATIC src/OrderBook.cpp)
target_include_directories(orderbook_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(orderbook_core PRIVATE orderbook_warnings)
if(ORDERBOOK_FLAT_LEVELS)
  # PUBLIC: the definition changes the layout of OrderBook, so every user must agree.
  target_compile_definitions(orderbook_core PUBLIC ORDERBOOK_FLAT_LEVELS)
endif()

# ---- demo ----
add_executable(orderbook_demo apps/main.cpp)
target_link_libraries(orderbook_demo PRIVATE orderbook_core orderbook_warnings)

# ---- tests ----
if(ORDERBOOK_BUILD_TESTS)
  enable_testing()

  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(orderbook_tests tests/OrderBookTests.cpp tests/DifferentialTests.cpp
                                 tests/MarketSimTests.cpp)
  target_include_directories(orderbook_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(orderbook_tests PRIVATE orderbook_core orderbook_warnings GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(orderbook_tests)
endif()

# ---- benchmarks ----
if(ORDERBOOK_BUILD_BENCHMARKS)
  find_package(benchmark QUIET)
  if(NOT benchmark_FOUND)
    include(FetchContent)
    FetchContent_Declare(
      googlebenchmark
      URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googlebenchmark)
  endif()

  add_executable(orderbook_bench bench/OrderBookBench.cpp)
  target_link_libraries(orderbook_bench PRIVATE orderbook_core orderbook_warnings benchmark::benchmark)

  add_executable(orderbook_latency bench/Latency.cpp)
  target_link_libraries(orderbook_latency PRIVATE orderbook_core orderbook_warnings)
endif()
