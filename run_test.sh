#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────
# run_test.sh — Build, start the C++ server, run the Python load
#               test, and shut everything down cleanly.
# ──────────────────────────────────────────────────────────────────
set -euo pipefail

PORT=9000
DRONES=100
DURATION=30
RATE=20
BUILD_DIR="build"
SERVER_BIN="${BUILD_DIR}/counter_drone_server"

# ── 1. Build (silently) ─────────────────────────────────────────────
cmake -B "${BUILD_DIR}" -S . > /dev/null 2>&1
cmake --build "${BUILD_DIR}" -j"$(nproc)" > /dev/null 2>&1

# ── 2. Start the C++ server in the background (silently) ───────────
"${SERVER_BIN}" "${PORT}" > /dev/null 2>&1 &
SERVER_PID=$!

# Give the server a moment to bind
sleep 1

# Ensure the server is killed on exit (Ctrl+C, error, or normal end)
cleanup() {
    kill -SIGINT "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
}
trap cleanup EXIT

# ── 3. Run the Python load test ────────────────────────────────────
python3 tests/drone_load_test.py \
    --host 127.0.0.1 \
    --port "${PORT}" \
    --drones "${DRONES}" \
    --duration "${DURATION}" \
    --rate "${RATE}"
