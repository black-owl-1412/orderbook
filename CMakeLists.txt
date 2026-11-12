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

  add_executable(orderbook_tests tests/OrderBookTests.cpp)
  target_link_libraries(orderbook_tests PRIVATE orderbook_core orderbook_warnings GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(orderbook_tests)
endif()
