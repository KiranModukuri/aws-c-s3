# RDMA Setup Guide for aws-c-s3-kiran

This guide explains how to set up and run S3 RDMA tests against a MinIO server with RDMA support.

## Prerequisites

- MinIO server running with RDMA support on `200.1.84.163`
- cuObject server running on `200.1.84.163:17123`
- RDMA network connectivity between client and server
- cuObject/cuFile libraries installed at `/home/lab/tim/nvme-direct/`

## Network Topology

```
Client (cuobjcli)                    Server (cuobjserver)
─────────────────                    ────────────────────
RDMA NIC: 200.1.84.164      ←RDMA→   RDMA NIC: 200.1.84.163
                                     MinIO S3:  :9000
                                     cuObject:  :17123
```

## Quick Start

### 1. One-time Setup (First Time Only)

```bash
cd ~/tim/aws-c-s3-kiran

# Run the setup script (bootstraps CRT deps + builds everything)
./scripts/setup_rdma.sh
```

This will:
- Create `scripts/env_rdma.sh` from the template
- Bootstrap all CRT dependencies (~5 minutes)
- Build aws-c-s3 library and RDMA plugin

### 2. Running Tests

```bash
cd ~/tim/aws-c-s3-kiran

# Source the environment
source scripts/env_rdma.sh

# Run a specific test
ctest --test-dir build -V -R '^test_s3_get_object_less_than_part_size$'

# Run all GET object tests
ctest --test-dir build -V -R 'get_object'

# List all available tests
ctest --test-dir build -N
```

## Verifying RDMA is Working

### Method 1: Check Test Output

Look for plugin logs showing RDMA token generation:
```
[CUOBJECT_PLUGIN] Generated GET token: 000071675bfff000:01400000:00182ceb:...
```

### Method 2: Capture HTTP Headers

Run tcpdump while executing a test:

```bash
# Terminal 1: Start packet capture
sudo tcpdump -i any -A -s 0 'port 9000' -c 10

# Terminal 2: Run test
source scripts/env_rdma.sh
ctest --test-dir build -R '^test_s3_get_object_less_than_part_size$'
```

**RDMA Success Indicators:**

| Header | Expected Value | Meaning |
|--------|----------------|---------|
| `x-amz-rdma-token` (request) | `00007...` | Client sent RDMA token |
| `Content-Length` (response) | `0` | Data went via RDMA, not HTTP |
| `X-Rdma-Reply` (response) | `200` | Server confirms RDMA success |
| `X-Rdma-Bytes` (response) | `1048576` | Bytes transferred via RDMA |

### Method 3: Use the Verify Script

```bash
./scripts/verify_rdma.sh
```

## Configuration Files

### scripts/env_rdma.sh

Main environment configuration. Key variables:

| Variable | Value | Description |
|----------|-------|-------------|
| `CRT_S3_TEST_ENDPOINT` | `200.1.84.163:9000` | MinIO S3 endpoint |
| `CUOBJ_SERVER_ADDR` | `200.1.84.163:17123` | cuObject RDMA server |
| `CUFILE_ENV_PATH_JSON` | `.../cuobj.json` | cuFile configuration |
| `AWS_S3_RDMA_THRESHOLD_BYTES` | `1048576` | Min size for RDMA (1MB) |

### /home/lab/tim/nvme-direct/cuObject/cuobj.json

cuFile/cuObject configuration. Critical setting:

```json
"rdma_dev_addr_list": ["200.1.84.164"]
```

This must be the **client's** RDMA NIC IP address.

## Troubleshooting

### "cuObject server not running"

Ensure the cuObject server is running on the MinIO server:
```bash
# On server (200.1.84.163)
ps aux | grep cuobj
```

### RDMA Falls Back to TCP

Check these common issues:

1. **Wrong `CUOBJ_SERVER_ADDR`**: Must point to server's RDMA IP (200.1.84.163)
2. **Wrong `rdma_dev_addr_list`**: Must be client's RDMA IP (200.1.84.164)
3. **Bucket doesn't exist**: Create it first:
   ```bash
   aws --endpoint-url http://200.1.84.163:9000 s3 mb s3://aws-c-s3-test-bucket
   aws --endpoint-url http://200.1.84.163:9000 s3 cp /dev/urandom s3://aws-c-s3-test-bucket/pre-existing-1MB --content-length 1048576
   ```

### Rebuilding

```bash
# Rebuild just aws-c-s3 and plugin (fast)
source scripts/env_rdma.sh
./scripts/build_all.sh --skip-crt-check

# Full rebuild including CRT dependencies
./scripts/build_all.sh --rebuild-crt
```

## Test Bucket Setup

The tests expect a bucket with specific test files:

```bash
export AWS_ACCESS_KEY_ID=minioadmin
export AWS_SECRET_ACCESS_KEY=minioadmin

# Create bucket
aws --endpoint-url http://200.1.84.163:9000 s3 mb s3://aws-c-s3-test-bucket

# Create 1MB test file
dd if=/dev/urandom bs=1M count=1 | aws --endpoint-url http://200.1.84.163:9000 \
    s3 cp - s3://aws-c-s3-test-bucket/pre-existing-1MB
```

## Directory Structure

```
aws-c-s3-kiran/
├── scripts/
│   ├── env_rdma.sh          # Environment config (create from template)
│   ├── env_rdma.sh.template # Template
│   ├── setup_rdma.sh        # One-time setup script
│   ├── verify_rdma.sh       # RDMA verification script
│   ├── bootstrap_crt.sh     # CRT dependency bootstrap
│   └── build_all.sh         # Build script
├── build/                   # Build output
├── local-install/           # CRT libraries
├── plugins/cuobject/        # RDMA plugin source
│   └── build/
│       └── libcuobject_s3_plugin.so
└── crt/                     # CRT dependency sources
```

