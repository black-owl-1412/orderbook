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
option(ORDERBOOK_BUILD_VIZ "Build the live order-book viewer (Dear ImGui + GLFW)" ON)

if(ORDERBOOK_SANITIZE)
  # Applied globally so the library, tests and any fetched deps agree.
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

if(MINGW)
  # Standalone Windows executables: libstdc++, libgcc and winpthread are linked in, so the
  # .exe files run on any Windows PC without MinGW DLLs next to them.
  add_link_options(-static)
  # std::print's Windows console support (std::__open_terminal, __write_to_terminal)
  # lives in libstdc++exp on MinGW GCC 14/15.
  link_libraries(stdc++exp)
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
                                 tests/MarketSimTests.cpp tests/LiveSimTests.cpp)
  # apps/viz for LiveSim.h, the viewer's UI-free model
  target_include_directories(orderbook_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests
                                                     ${CMAKE_CURRENT_SOURCE_DIR}/apps/viz)
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

# ---- live viewer (Dear ImGui + GLFW + OpenGL3) ----
if(ORDERBOOK_BUILD_VIZ)
  find_package(OpenGL REQUIRED)
  include(FetchContent)

  find_package(glfw3 3.3 QUIET)
  if(NOT glfw3_FOUND)
    FetchContent_Declare(
      glfw
      URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
  endif()

  # Dear ImGui has no CMake build of its own: compile the core and the two backends we use.
  FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  # ImPlot: the live price chart.
  FetchContent_Declare(
    implot
    URL https://github.com/epezent/implot/archive/refs/tags/v0.16.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  # JetBrains Mono (SIL Open Font License), embedded in the viewer (see below).
  FetchContent_Declare(
    jetbrainsmono
    URL https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(imgui implot jetbrainsmono)

  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
                                                 ${implot_SOURCE_DIR})
  target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

  # The font is compiled into the viewer, so the executable needs no files next to it.
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFiles.cmake)
  set(ORDERBOOK_FONT_TTF ${jetbrainsmono_SOURCE_DIR}/fonts/ttf)
  embed_files(${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedFonts.cpp orderbook_fonts
    JetBrainsMonoRegular ${ORDERBOOK_FONT_TTF}/JetBrainsMono-Regular.ttf
    JetBrainsMonoBold ${ORDERBOOK_FONT_TTF}/JetBrainsMono-Bold.ttf)

  add_executable(orderbook_viz apps/viz/main.cpp ${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedFonts.cpp)
  # tests/ for the header-only order-flow simulator in tests/sim/
  target_include_directories(orderbook_viz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(orderbook_viz PRIVATE orderbook_core imgui orderbook_warnings)

  if(WIN32)
    # A windowed app (no console window), with an icon and version information.
    enable_language(RC)
    configure_file(apps/viz/windows/orderbook_viz.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/orderbook_viz.rc @ONLY)
    target_sources(orderbook_viz PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/orderbook_viz.rc)
    set_target_properties(orderbook_viz PROPERTIES WIN32_EXECUTABLE ON)
  endif()
endif()
