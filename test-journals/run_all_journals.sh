#!/usr/bin/env bash
set -euo pipefail

echo "=== Exchange Core Journal Test Suite ==="
echo "Date: $(date)"
echo ""

# Build the test
echo "Building..."
bazel build //test-journals:journal_test 2>/dev/null

# Run with verbose output
echo ""
echo "Running all journal scenarios..."
echo ""

RESULT=$(bazel test //test-journals:journal_test --test_output=streamed 2>&1)

# Extract pass/fail counts
PASSED=$(echo "$RESULT" | grep -c "\[       OK \]" || true)
FAILED=$(echo "$RESULT" | grep -c "\[  FAILED  \]" || true)
TOTAL=$((PASSED + FAILED))

echo ""
echo "=========================================="
echo "Results: $PASSED/$TOTAL passed, $FAILED failed"
echo "=========================================="

if [ "$FAILED" -gt 0 ]; then
    echo ""
    echo "FAILED scenarios:"
    echo "$RESULT" | grep "\[  FAILED  \]" | sed 's/.*RunJournal\//  - /' | sed 's/,.*//'
    exit 1
else
    echo "All journal scenarios passed!"
    exit 0
fi
