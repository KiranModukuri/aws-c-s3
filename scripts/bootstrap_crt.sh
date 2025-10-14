#!/usr/bin/env bash
set -euo pipefail

# Bootstrap AWS CRT C dependencies to a local prefix.
# Usage: source this file or run directly after setting REPO_ROOT. Optionally set CRT_PREFIX.

# Load env defaults (REPO_ROOT, CRT_PREFIX, library paths) from env_rdma.sh if available
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "$SCRIPT_DIR/env_rdma.sh" ]; then
    # shellcheck disable=SC1091
    source "$SCRIPT_DIR/env_rdma.sh"
fi

# Require env variables (must source env_rdma.sh first)
if [ -z "${REPO_ROOT:-}" ]; then
  echo "[bootstrap] ERROR: REPO_ROOT not set. Please source scripts/env_rdma.sh first." >&2
  exit 1
fi
if [ -z "${CRT_PREFIX:-}" ]; then
  echo "[bootstrap] ERROR: CRT_PREFIX not set. Please source scripts/env_rdma.sh first." >&2
  exit 1
fi

mkdir -p "$REPO_ROOT/crt"
cd "$REPO_ROOT/crt"

echo "[bootstrap] Installing AWS CRT C libs to prefix: $CRT_PREFIX"

# Crypto + TLS
if [ ! -d aws-lc/.git ]; then git clone --depth 1 https://github.com/aws/aws-lc.git; fi
cmake -S aws-lc -B aws-lc/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"
cmake --build aws-lc/build -j
cmake --install aws-lc/build

if [ ! -d s2n-tls/.git ]; then git clone --depth 1 https://github.com/aws/s2n-tls.git; fi
cmake -S s2n-tls -B s2n-tls/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH="$CRT_PREFIX" -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"
cmake --build s2n-tls/build -j
cmake --install s2n-tls/build

# Common
if [ ! -d aws-c-common/.git ]; then git clone --depth 1 https://github.com/awslabs/aws-c-common.git; fi
cmake -S aws-c-common -B aws-c-common/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"
cmake --build aws-c-common/build -j
cmake --install aws-c-common/build

# Checksums
if [ ! -d aws-checksums/.git ]; then git clone --depth 1 https://github.com/awslabs/aws-checksums.git; fi
cmake -S aws-checksums -B aws-checksums/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"
cmake --build aws-checksums/build -j
cmake --install aws-checksums/build

# Crypto Abstraction Layer
if [ ! -d aws-c-cal/.git ]; then git clone --depth 1 https://github.com/awslabs/aws-c-cal.git; fi
cmake -S aws-c-cal -B aws-c-cal/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH="$CRT_PREFIX" -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"
cmake --build aws-c-cal/build -j
cmake --install aws-c-cal/build

# Compression
if [ ! -d aws-c-compression/.git ]; then git clone --depth 1 https://github.com/awslabs/aws-c-compression.git; fi
cmake -S aws-c-compression -B aws-c-compression/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH="$CRT_PREFIX" -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"
cmake --build aws-c-compression/build -j
cmake --install aws-c-compression/build

# IO
if [ ! -d aws-c-io/.git ]; then git clone --depth 1 https://github.com/awslabs/aws-c-io.git; fi
cmake -S aws-c-io -B aws-c-io/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH="$CRT_PREFIX" -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"
cmake --build aws-c-io/build -j
cmake --install aws-c-io/build

# HTTP
if [ ! -d aws-c-http/.git ]; then git clone --depth 1 https://github.com/awslabs/aws-c-http.git; fi
cmake -S aws-c-http -B aws-c-http/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH="$CRT_PREFIX" -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"
cmake --build aws-c-http/build -j
cmake --install aws-c-http/build

# SDK utils (required by aws-c-auth)
if [ ! -d aws-c-sdkutils/.git ]; then git clone --depth 1 https://github.com/awslabs/aws-c-sdkutils.git; fi
cmake -S aws-c-sdkutils -B aws-c-sdkutils/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH="$CRT_PREFIX" -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"
cmake --build aws-c-sdkutils/build -j
cmake --install aws-c-sdkutils/build

# Auth
if [ ! -d aws-c-auth/.git ]; then git clone --depth 1 https://github.com/awslabs/aws-c-auth.git; fi
cmake -S aws-c-auth -B aws-c-auth/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH="$CRT_PREFIX" -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX"
cmake --build aws-c-auth/build -j
cmake --install aws-c-auth/build

echo "[bootstrap] Done. Prefix: $CRT_PREFIX"


