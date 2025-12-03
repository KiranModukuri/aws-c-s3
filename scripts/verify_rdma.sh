#!/usr/bin/env bash
set -euo pipefail

# =============================================================================
# RDMA Verification Script
# 
# Runs an S3 GET test and captures HTTP headers to verify RDMA is working.
#
# Usage: ./scripts/verify_rdma.sh
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "RDMA Verification"
echo "=========================================="
echo ""

# Source environment
if [ -f "$SCRIPT_DIR/env_rdma.sh" ]; then
    # shellcheck disable=SC1091
    source "$SCRIPT_DIR/env_rdma.sh" 2>/dev/null
else
    echo -e "${RED}ERROR: env_rdma.sh not found. Run setup_rdma.sh first.${NC}"
    exit 1
fi

# Check if tests are built
if [ ! -f "$REPO_ROOT/build/tests/aws-c-s3-tests" ]; then
    echo -e "${RED}ERROR: Tests not built. Run setup_rdma.sh first.${NC}"
    exit 1
fi

# Temp file for tcpdump output
TCPDUMP_OUTPUT=$(mktemp)
trap "rm -f $TCPDUMP_OUTPUT" EXIT

echo "[verify] Starting packet capture..."

# Start tcpdump in background
sudo timeout 10 tcpdump -i any -A -s 0 'port 9000' -c 20 > "$TCPDUMP_OUTPUT" 2>/dev/null &
TCPDUMP_PID=$!
sleep 1

echo "[verify] Running test: test_s3_get_object_less_than_part_size"
echo ""

# Run the test
TEST_OUTPUT=$(ctest --test-dir "$REPO_ROOT/build" -V -R '^test_s3_get_object_less_than_part_size$' 2>&1) || true

# Wait for tcpdump
wait $TCPDUMP_PID 2>/dev/null || true

# Check test result
if echo "$TEST_OUTPUT" | grep -q "100% tests passed"; then
    echo -e "${GREEN}✓ Test PASSED${NC}"
else
    echo -e "${RED}✗ Test FAILED${NC}"
    echo "$TEST_OUTPUT" | tail -20
    exit 1
fi

echo ""
echo "=========================================="
echo "HTTP Header Analysis"
echo "=========================================="
echo ""

# Extract and display relevant headers
echo "--- REQUEST HEADERS ---"
if grep -q "x-amz-rdma-token:" "$TCPDUMP_OUTPUT"; then
    TOKEN=$(grep "x-amz-rdma-token:" "$TCPDUMP_OUTPUT" | head -1 | sed 's/.*x-amz-rdma-token: //')
    echo -e "${GREEN}✓ x-amz-rdma-token: ${TOKEN:0:40}...${NC}"
    RDMA_REQUEST=true
else
    echo -e "${RED}✗ x-amz-rdma-token: NOT FOUND${NC}"
    RDMA_REQUEST=false
fi

echo ""
echo "--- RESPONSE HEADERS ---"

# Check Content-Length
if grep -q "Content-Length: 0" "$TCPDUMP_OUTPUT"; then
    echo -e "${GREEN}✓ Content-Length: 0 (data sent via RDMA, not HTTP body)${NC}"
    CONTENT_LENGTH_ZERO=true
else
    echo -e "${YELLOW}! Content-Length: non-zero (data may have gone via HTTP)${NC}"
    CONTENT_LENGTH_ZERO=false
fi

# Check X-Rdma-Reply
if grep -q "X-Rdma-Reply: 200" "$TCPDUMP_OUTPUT"; then
    echo -e "${GREEN}✓ X-Rdma-Reply: 200 (RDMA transfer successful)${NC}"
    RDMA_REPLY_OK=true
elif grep -q "X-Rdma-Reply: 501" "$TCPDUMP_OUTPUT"; then
    echo -e "${RED}✗ X-Rdma-Reply: 501 (RDMA not supported, fell back to HTTP)${NC}"
    RDMA_REPLY_OK=false
elif grep -q "X-Rdma-Reply:" "$TCPDUMP_OUTPUT"; then
    REPLY=$(grep "X-Rdma-Reply:" "$TCPDUMP_OUTPUT" | head -1)
    echo -e "${YELLOW}! $REPLY${NC}"
    RDMA_REPLY_OK=false
else
    echo -e "${RED}✗ X-Rdma-Reply: NOT FOUND${NC}"
    RDMA_REPLY_OK=false
fi

# Check X-Rdma-Bytes
if grep -q "X-Rdma-Bytes:" "$TCPDUMP_OUTPUT"; then
    BYTES=$(grep "X-Rdma-Bytes:" "$TCPDUMP_OUTPUT" | head -1 | sed 's/.*X-Rdma-Bytes: //' | tr -d '\r')
    echo -e "${GREEN}✓ X-Rdma-Bytes: $BYTES${NC}"
    RDMA_BYTES=true
else
    echo -e "${RED}✗ X-Rdma-Bytes: NOT FOUND${NC}"
    RDMA_BYTES=false
fi

echo ""
echo "=========================================="
echo "Summary"
echo "=========================================="
echo ""

if [ "$RDMA_REQUEST" = true ] && [ "$CONTENT_LENGTH_ZERO" = true ] && [ "$RDMA_REPLY_OK" = true ] && [ "$RDMA_BYTES" = true ]; then
    echo -e "${GREEN}✓ RDMA VERIFIED - Data transferred via RDMA, bypassing TCP${NC}"
    echo ""
    echo "The S3 GET operation successfully used RDMA:"
    echo "  • Client sent RDMA token in request"
    echo "  • Server responded with empty HTTP body (Content-Length: 0)"
    echo "  • Server confirmed RDMA success (X-Rdma-Reply: 200)"
    echo "  • Data was transferred directly to client memory via RDMA"
    exit 0
else
    echo -e "${RED}✗ RDMA NOT VERIFIED - Transfer may have fallen back to TCP${NC}"
    echo ""
    echo "Possible issues:"
    if [ "$RDMA_REQUEST" = false ]; then
        echo "  • Client did not send RDMA token (check AWS_S3_RDMA_PLUGIN_PATH)"
    fi
    if [ "$RDMA_REPLY_OK" = false ]; then
        echo "  • Server did not confirm RDMA (check CUOBJ_SERVER_ADDR)"
    fi
    if [ "$CONTENT_LENGTH_ZERO" = false ]; then
        echo "  • Data was sent via HTTP body instead of RDMA"
    fi
    echo ""
    echo "Check configuration:"
    echo "  CUOBJ_SERVER_ADDR=$CUOBJ_SERVER_ADDR"
    echo "  AWS_S3_RDMA_PLUGIN_PATH=$AWS_S3_RDMA_PLUGIN_PATH"
    exit 1
fi

