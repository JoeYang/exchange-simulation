#!/usr/bin/env bash
set -euo pipefail

echo "=== Running Exchange Tests ==="
bazel test //...
echo "=== Tests complete ==="
