#!/usr/bin/env bash
set -euo pipefail

echo "=== Exchange Project Setup ==="

if ! command -v bazel &>/dev/null; then
    echo "ERROR: bazel is not installed. Install Bazel 9+ first."
    exit 1
fi

echo "Bazel version: $(bazel --version)"
echo "Fetching external dependencies..."
bazel fetch //...
echo "=== Setup complete ==="
