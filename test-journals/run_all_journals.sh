#!/usr/bin/env bash
set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${BOLD}=== Exchange Core Journal Test Suite ===${NC}"
echo "Date: $(date)"
echo "CPU:  $(lscpu 2>/dev/null | grep 'Model name' | sed 's/.*: *//' || echo 'unknown')"
echo ""

# Build
echo -e "${CYAN}Building test runner...${NC}"
bazel build //test-journals:journal_test 2>/dev/null
echo ""

# Count journals
JOURNAL_DIR="test-journals"
JOURNALS=($(ls ${JOURNAL_DIR}/*.journal 2>/dev/null | sort))
TOTAL=${#JOURNALS[@]}

echo -e "${BOLD}Found ${TOTAL} journal scenarios${NC}"
echo ""

# Run and capture full output
OUTPUT=$(bazel test //test-journals:journal_test --test_output=all 2>&1)

# Parse results
PASSED=0
FAILED=0
FAILED_LIST=()

printf "${BOLD}  %-48s %s${NC}\n" "Scenario" "Result"
printf '%.0s─' {1..60}
echo ""

for journal in "${JOURNALS[@]}"; do
    name=$(basename "$journal" .journal)

    if echo "$OUTPUT" | grep -q "\[  FAILED  \].*${name}"; then
        printf "  %-48s ${RED}FAIL${NC}\n" "$name"
        FAILED=$((FAILED + 1))
        FAILED_LIST+=("$name")
    elif echo "$OUTPUT" | grep -q "\[       OK \].*${name}"; then
        printf "  %-48s ${GREEN}PASS${NC}\n" "$name"
        PASSED=$((PASSED + 1))
    else
        printf "  %-48s ${YELLOW}SKIP${NC}\n" "$name"
    fi
done

echo ""
printf '%.0s─' {1..65}
echo ""

# Summary
echo -e "${BOLD}Summary:${NC}"
echo -e "  Total:   ${TOTAL}"
echo -e "  ${GREEN}Passed:  ${PASSED}${NC}"
if [ "$FAILED" -gt 0 ]; then
    echo -e "  ${RED}Failed:  ${FAILED}${NC}"
fi

# Show failure details
if [ "$FAILED" -gt 0 ]; then
    echo ""
    echo -e "${RED}${BOLD}=== FAILURE DETAILS ===${NC}"
    for name in "${FAILED_LIST[@]}"; do
        echo ""
        echo -e "${RED}--- ${name} ---${NC}"
        echo "$OUTPUT" | grep -A10 "RunJournal/${name}" | grep -E "Action|Expected|Actual|Diff" | head -6
    done
    echo ""
    echo -e "${RED}${BOLD}RESULT: FAIL${NC}"
    exit 1
else
    echo ""
    echo -e "${GREEN}${BOLD}RESULT: ALL ${PASSED} SCENARIOS PASSED${NC}"
    exit 0
fi
