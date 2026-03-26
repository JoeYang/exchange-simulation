#!/usr/bin/env bash
set -uo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${BOLD}=== ICE Exchange Test Suite ===${NC}"
echo "Date: $(date)"
echo ""

TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0
FAILED_LIST=()
FAILED_OUTPUT=()

# Run a single bazel test target. Records pass/fail/skip.
run_test() {
    local target="$1"
    local label="$2"
    TOTAL=$((TOTAL + 1))

    # Check if target exists by querying bazel
    if ! bazel query "$target" >/dev/null 2>&1; then
        printf "  %-55s ${YELLOW}SKIP${NC}\n" "$label"
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    local output
    output=$(bazel test "$target" --test_output=errors 2>&1)
    local rc=$?

    if [ $rc -eq 0 ]; then
        printf "  %-55s ${GREEN}PASS${NC}\n" "$label"
        PASSED=$((PASSED + 1))
    else
        printf "  %-55s ${RED}FAIL${NC}\n" "$label"
        FAILED=$((FAILED + 1))
        FAILED_LIST+=("$label")
        FAILED_OUTPUT+=("$output")
    fi
}

# =========================================================================
# Section 1: ICE unit tests
# =========================================================================

echo -e "${CYAN}${BOLD}[1/3] ICE Unit Tests${NC}"
echo ""
printf "${BOLD}  %-55s %s${NC}\n" "Test Target" "Result"
printf '  %.0s─' {1..65}
echo ""

# Core ICE
run_test "//ice:gtbpr_match_test"                "GTBPR matching algorithm"
run_test "//ice:ice_exchange_test"               "IceExchange CRTP"
run_test "//ice:ice_products_test"               "ICE product configs"

# FIX protocol
run_test "//ice/fix:fix_parser_test"             "FIX 4.2 parser"
run_test "//ice/fix:fix_encoder_test"            "FIX 4.2 encoder"
run_test "//ice/fix:ice_fix_exec_publisher_test" "FIX exec publisher"
run_test "//ice/fix:ice_fix_gateway_test"        "FIX gateway"

# iMpact protocol
run_test "//ice/impact:impact_messages_test"     "iMpact message structs"
run_test "//ice/impact:impact_encoder_test"      "iMpact encoder"
run_test "//ice/impact:impact_decoder_test"      "iMpact decoder"
run_test "//ice/impact:impact_publisher_test"    "iMpact publisher"
run_test "//ice/impact:impact_codec_test"        "iMpact codec round-trip"

echo ""

# =========================================================================
# Section 2: ICE E2E journal tests
# =========================================================================

echo -e "${CYAN}${BOLD}[2/3] ICE E2E Journal Tests${NC}"
echo ""

ICE_JOURNAL_DIR="test-journals/ice"
if [ -d "$ICE_JOURNAL_DIR" ] && ls "$ICE_JOURNAL_DIR"/*.journal >/dev/null 2>&1; then
    JOURNAL_COUNT=$(ls "$ICE_JOURNAL_DIR"/*.journal 2>/dev/null | wc -l)
    echo -e "  Found ${JOURNAL_COUNT} ICE journal scenarios"
    echo ""

    # Run the E2E harness target if it exists
    run_test "//ice/e2e:ice_e2e_journal_test" "ICE E2E journal harness"
else
    echo -e "  ${YELLOW}No ICE journal files found yet (test-journals/ice/*.journal)${NC}"
    echo -e "  ${YELLOW}Skipping E2E journal section${NC}"
fi

echo ""

# =========================================================================
# Section 3: GTBPR integration tests
# =========================================================================

echo -e "${CYAN}${BOLD}[3/3] GTBPR Integration Tests${NC}"
echo ""

run_test "//ice:gtbpr_integration_test" "GTBPR integration (IceExchange engine-level)"

echo ""

# =========================================================================
# Summary
# =========================================================================

printf '%.0s═' {1..65}
echo ""
echo -e "${BOLD}Summary:${NC}"
echo -e "  Total:   ${TOTAL}"
echo -e "  ${GREEN}Passed:  ${PASSED}${NC}"
if [ "$SKIPPED" -gt 0 ]; then
    echo -e "  ${YELLOW}Skipped: ${SKIPPED}${NC}"
fi
if [ "$FAILED" -gt 0 ]; then
    echo -e "  ${RED}Failed:  ${FAILED}${NC}"
fi

# Show failure details
if [ "$FAILED" -gt 0 ]; then
    echo ""
    echo -e "${RED}${BOLD}=== FAILURE DETAILS ===${NC}"
    for i in "${!FAILED_LIST[@]}"; do
        echo ""
        echo -e "${RED}--- ${FAILED_LIST[$i]} ---${NC}"
        echo "${FAILED_OUTPUT[$i]}" | grep -E "FAIL|Error|error:|expected|actual" | head -6
    done
    echo ""
    echo -e "${RED}${BOLD}RESULT: FAIL (${FAILED} of ${TOTAL} failed)${NC}"
    exit 1
else
    echo ""
    echo -e "${GREEN}${BOLD}RESULT: ALL ${PASSED} TESTS PASSED${NC}"
    exit 0
fi
