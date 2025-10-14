# AWS C S3 with RDMA Support - Installation Guide

This guide provides step-by-step instructions for building and installing aws-c-s3 with RDMA support via the NVIDIA cuObject plugin.

## Prerequisites

### Required Software
- **CMake** >= 3.9
- **GCC/Clang** C/C++ compiler
- **Git**
- **CUDA Toolkit** (for RDMA support)
- **cuObject Library** (for RDMA support)

### System Requirements
- Linux operating system
- NVIDIA GPU with CUDA support (for RDMA features)
- GPUDirect Storage capable hardware (for RDMA features)

## Quick Start

### 1. Clone the Repository

```bash
git clone <repository-url> aws-c-s3
cd aws-c-s3
```

### 2. Configure Environment

Copy and customize the environment configuration template:

```bash
cd scripts
cp env_rdma.sh.template env_rdma.sh
```

Edit `env_rdma.sh` and set the following variables according to your system:

```bash
# Required: Path to this repository
export REPO_ROOT=/path/to/aws-c-s3

# Required for RDMA support: Path to cuObject installation
export CUOBJECT_ROOT_DIR=/path/to/cuObject

# Optional: Customize cuFile paths if different from cuObject root
export CUFILE_LIB_DIR=${CUOBJECT_ROOT_DIR}/lib/cufile
export CUFILE_RDMA_LIB_DIR=${CUOBJECT_ROOT_DIR}/lib/mlx
export CUFILE_INCLUDE_DIR=${CUOBJECT_ROOT_DIR}/lib/cufile
export CUFILE_ENV_PATH_JSON=${CUOBJECT_ROOT_DIR}/cuobj.json

# Optional: AWS credentials for testing
export AWS_ACCESS_KEY_ID=your_access_key
export AWS_SECRET_ACCESS_KEY=your_secret_key
export AWS_REGION=us-east-1
```

Source the environment file:

```bash
source env_rdma.sh
```

### 3. Bootstrap CRT Dependencies

Download and build the AWS Common Runtime (CRT) dependencies:

```bash
./bootstrap_crt.sh
```

This will:
- Clone AWS CRT libraries (aws-lc, s2n-tls, aws-c-common, aws-checksums, aws-c-cal, aws-c-compression, aws-c-io, aws-c-http, aws-c-sdkutils, aws-c-auth)
- Build and install them to `$REPO_ROOT/local-install`

**Note:** This step takes 10-15 minutes on first run. CRT dependencies are cached in `$REPO_ROOT/crt/`.

### 4. Build AWS C S3 Core and RDMA Plugin

```bash
./build_all.sh
```

This will:
- Build the core aws-c-s3 library
- Build the NVIDIA cuObject RDMA plugin
- Install libraries to `$REPO_ROOT/local-install`

**Build artifacts:**
- Core library: `$REPO_ROOT/local-install/lib/libaws-c-s3.a`
- RDMA Plugin: `$REPO_ROOT/plugins/cuobject/build/libcuobject_s3_plugin.so`

## Build Options

### Build Script Options

```bash
./build_all.sh [OPTIONS]
```

Options:
- `--rebuild-crt` - Force rebuild of all CRT dependencies
- `--check-crt-updates` - Check if CRT dependencies need updating (default)
- `--skip-crt-check` - Skip CRT dependency checks (faster builds)
- `--help` - Show help message

### Building Without RDMA Support

If you don't have cuObject installed, the build will automatically create a mock plugin without RDMA capabilities:

```bash
# Unset cuObject variables or leave them empty
unset CUOBJECT_ROOT_DIR
unset CUOBJECT_LIB_DIR

source scripts/env_rdma.sh
./build_all.sh
```

### Manual Build Steps

If you prefer to build components individually:

#### Build Core Library Only

```bash
source scripts/env_rdma.sh

cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF \
    -DCMAKE_PREFIX_PATH="$CRT_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"

cmake --build build -j
cmake --install build
```

#### Build RDMA Plugin Only

```bash
source scripts/env_rdma.sh

cmake -S plugins/cuobject -B plugins/cuobject/build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF \
    -DCMAKE_PREFIX_PATH="$CRT_PREFIX" \
    -DCUOBJECT_ROOT_DIR="$CUOBJECT_ROOT_DIR" \
    -DCUOBJECT_LIBRARY="$CUOBJECT_LIB_DIR/libcuobjclient.so"

cmake --build plugins/cuobject/build -j
```

## Testing

### Run RDMA Tests

After building with RDMA support, you can run tests:

```bash
source scripts/env_rdma.sh
./scripts/run_test_rdma_io.sh
```

### Configure Test Endpoint

By default, tests use a local MinIO endpoint. Configure the test endpoint in `env_rdma.sh`:

```bash
export CRT_S3_TEST_ENDPOINT="127.0.0.1:9000"
export CRT_S3_TEST_BUCKET_NAME="test-bucket"
export S3_HTTP_ENDPOINT="http://127.0.0.1:9000"
```

## Installation Layout

After building, the installation structure is:

```
$REPO_ROOT/
├── local-install/          # CRT libraries and aws-c-s3
│   ├── lib/
│   │   ├── libaws-c-s3.a
│   │   ├── libaws-c-*.a    # CRT libraries
│   │   └── cmake/          # CMake config files
│   └── include/
│       └── aws/            # Header files
├── plugins/cuobject/build/ # RDMA plugin
│   └── libcuobject_s3_plugin.so
└── crt/                    # CRT source repositories
    ├── aws-c-common/
    ├── aws-c-io/
    └── ...
```

## Environment Variables Reference

### Required Variables

| Variable | Description | Example |
|----------|-------------|---------|
| `REPO_ROOT` | Path to aws-c-s3 repository | `/home/user/aws-c-s3` |
| `CRT_PREFIX` | Installation prefix for libraries | `$REPO_ROOT/local-install` |

### RDMA-Specific Variables

| Variable | Description | Example |
|----------|-------------|---------|
| `CUOBJECT_ROOT_DIR` | Path to cuObject installation | `/opt/cuObject` |
| `CUOBJECT_LIB_DIR` | Path to cuObject libraries | `$CUOBJECT_ROOT_DIR/build/lib_src` |
| `CUFILE_LIB_DIR` | Path to cuFile libraries | `$CUOBJECT_ROOT_DIR/lib/cufile` |
| `CUFILE_RDMA_LIB_DIR` | Path to cuFile RDMA libraries | `$CUOBJECT_ROOT_DIR/lib/mlx` |
| `CUFILE_ENV_PATH_JSON` | Path to cuFile configuration | `$CUOBJECT_ROOT_DIR/cuobj.json` |

### Runtime Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `AWS_S3_RDMA_PLUGIN_PATH` | Path to RDMA plugin | `$PLUGIN_BUILD_DIR/libcuobject_s3_plugin.so` |
| `AWS_S3_FORCE_RDMA` | Force RDMA for all requests | `1` |
| `AWS_S3_RDMA_THRESHOLD_BYTES` | Minimum size for RDMA | `1048576` (1MB) |
| `LD_PRELOAD` | Preload cuFile libraries | Set automatically |

## Troubleshooting

### CMake Cannot Find CRT Dependencies

**Problem:** `Could not find a package configuration file provided by "aws-c-common"`

**Solution:** Ensure CRT dependencies are bootstrapped and `CRT_PREFIX` is set:
```bash
source scripts/env_rdma.sh
./scripts/bootstrap_crt.sh
```

### cuObject Library Not Found

**Problem:** `CUOBJECT_LIBRARY not found in ${CUOBJECT_LIB_DIR}`

**Solution:** Verify cuObject paths in `env_rdma.sh`:
```bash
ls -l $CUOBJECT_LIB_DIR/libcuobjclient.so
```

If the file exists but has a different name (e.g., `libcuobjclient.so.0`), the build script will detect it automatically.

### Plugin Fails to Load at Runtime

**Problem:** Plugin loading errors or RDMA not being used

**Solution:** Check environment variables are set:
```bash
source scripts/env_rdma.sh
echo $AWS_S3_RDMA_PLUGIN_PATH
echo $LD_PRELOAD
```

Verify the plugin file exists:
```bash
ls -l $AWS_S3_RDMA_PLUGIN_PATH
```

### Build Fails with Missing Test Files

**Problem:** CMake errors about missing `test_*.c` files

**Solution:** Build with testing disabled:
```bash
# Edit build_all.sh and change -DBUILD_TESTING=ON to -DBUILD_TESTING=OFF
# Or build manually:
cmake -S plugins/cuobject -B plugins/cuobject/build -DBUILD_TESTING=OFF ...
```

## Incremental Builds

### Rebuild Only Core Library

```bash
source scripts/env_rdma.sh
cmake --build build -j
cmake --install build
```

### Rebuild Only Plugin

```bash
source scripts/env_rdma.sh
cmake --build plugins/cuobject/build -j
```

### Update and Rebuild CRT Dependencies

```bash
source scripts/env_rdma.sh
./build_all.sh --rebuild-crt
```

## Clean Build

To start fresh:

```bash
# Remove build artifacts
rm -rf build plugins/cuobject/build

# Remove installed libraries (optional)
rm -rf local-install

# Remove CRT sources and start over (optional)
rm -rf crt

# Rebuild from scratch
./scripts/bootstrap_crt.sh
./scripts/build_all.sh
```

## Integration with Applications

To use the built library in your application:

### CMake Integration

```cmake
list(APPEND CMAKE_PREFIX_PATH "/path/to/aws-c-s3/local-install")
find_package(aws-c-s3 REQUIRED)
target_link_libraries(your_app PRIVATE AWS::aws-c-s3)
```

### Environment Setup

```bash
export CMAKE_PREFIX_PATH=/path/to/aws-c-s3/local-install:$CMAKE_PREFIX_PATH
export LD_LIBRARY_PATH=/path/to/aws-c-s3/local-install/lib:$LD_LIBRARY_PATH
export AWS_S3_RDMA_PLUGIN_PATH=/path/to/aws-c-s3/plugins/cuobject/build/libcuobject_s3_plugin.so
```

## Additional Resources

- [RDMA Protocol Specification](RDMA_PROTOCOL_SPEC.md)
- [RDMA README](RDMA_README.md)
- [AWS CRT Documentation](https://github.com/awslabs/aws-c-common)

