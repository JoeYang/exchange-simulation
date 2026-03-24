#!/usr/bin/env bash
set -euo pipefail

echo "=== Building Exchange ==="
bazel build //...
echo "=== Build complete ==="
