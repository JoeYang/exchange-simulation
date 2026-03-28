#!/usr/bin/env bash
set -uo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${BOLD}=== KRX Exchange Test Suite ===${NC}"
echo "Date: $(date)"
echo ""

TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0
FAILED_LIST=()
FAILED_OUTPUT=()

run_test() {
    local target="$1"
    local label="$2"
    TOTAL=$((TOTAL + 1))

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
# Section 1: KRX unit tests
# =========================================================================

echo -e "${CYAN}${BOLD}[1/4] KRX Unit Tests${NC}"
echo ""
printf "${BOLD}  %-55s %s${NC}\n" "Test Target" "Result"
printf '  %.0s─' {1..65}
echo ""

# Core KRX
run_test "//krx:krx_exchange_test"               "KrxExchange CRTP"
run_test "//krx:krx_products_test"               "KRX product configs"
run_test "//krx:krx_simulator_test"              "KrxSimulator wrapper"
run_test "//krx:krx_sim_config_test"             "KRX sim config"

# FIX protocol
run_test "//krx/fix:krx_fix_messages_test"       "KRX FIX 4.4 messages"
run_test "//krx/fix:krx_fix_gateway_test"        "KRX FIX gateway"
run_test "//krx/fix:krx_fix_exec_publisher_test" "KRX FIX exec publisher"

# FAST codec
run_test "//krx/fast:fast_types_test"            "FAST 1.1 types"
run_test "//krx/fast:fast_encoder_test"          "FAST encoder"
run_test "//krx/fast:fast_decoder_test"          "FAST decoder"
run_test "//krx/fast:fast_publisher_test"        "FAST feed publisher"
run_test "//krx/fast:fast_codec_test"            "FAST codec round-trip"

echo ""

# =========================================================================
# Section 2: KRX integration tests
# =========================================================================

echo -e "${CYAN}${BOLD}[2/4] KRX Integration Tests${NC}"
echo ""

run_test "//krx:krx_integration_test" "KRX integration (engine-level scenarios)"

echo ""

# =========================================================================
# Section 3: KRX E2E journal tests
# =========================================================================

echo -e "${CYAN}${BOLD}[3/4] KRX E2E Journal Tests${NC}"
echo ""

KRX_JOURNAL_DIR="test-journals/krx"
if [ -d "$KRX_JOURNAL_DIR" ] && ls "$KRX_JOURNAL_DIR"/*.journal >/dev/null 2>&1; then
    JOURNAL_COUNT=$(ls "$KRX_JOURNAL_DIR"/*.journal 2>/dev/null | wc -l)
    echo -e "  Found ${JOURNAL_COUNT} KRX journal scenarios"
    echo ""

    run_test "//krx/e2e:krx_e2e_journal_test" "KRX E2E journal harness"
else
    echo -e "  ${YELLOW}No KRX journal files found (test-journals/krx/*.journal)${NC}"
    echo -e "  ${YELLOW}Skipping E2E journal section${NC}"
fi

echo ""

# =========================================================================
# Section 4: Binary build verification
# =========================================================================

echo -e "${CYAN}${BOLD}[4/4] Binary Build Verification${NC}"
echo ""

TOTAL=$((TOTAL + 1))
build_output=$(bazel build //krx:krx-sim 2>&1)
build_rc=$?
if [ $build_rc -eq 0 ]; then
    printf "  %-55s ${GREEN}PASS${NC}\n" "krx-sim binary builds"
    PASSED=$((PASSED + 1))
else
    printf "  %-55s ${RED}FAIL${NC}\n" "krx-sim binary builds"
    FAILED=$((FAILED + 1))
    FAILED_LIST+=("krx-sim binary build")
    FAILED_OUTPUT+=("$build_output")
fi

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
