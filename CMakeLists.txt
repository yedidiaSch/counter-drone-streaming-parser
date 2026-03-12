cmake_minimum_required(VERSION 3.20)
project(CounterDroneComm
    VERSION 1.0.0
    DESCRIPTION "Counter-Drone TCP Communication Layer"
    LANGUAGES CXX
)

# ── C++20 Standard ───────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler warnings ────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# ── Threading ────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── 1. Core Library (Compile once, link everywhere) ──────────────
add_library(counter_drone_core STATIC
    src/ringBuffer.cpp
    src/listener.cpp
    src/streamingParser.cpp
    src/businessLogic.cpp
    src/asyncLogger.cpp
)
target_include_directories(counter_drone_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(counter_drone_core PUBLIC Threads::Threads)

# ── 2. Main server executable ────────────────────────────────────
add_executable(counter_drone_server src/main.cpp)
target_link_libraries(counter_drone_server PRIVATE counter_drone_core)