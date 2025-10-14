#!/usr/bin/env bash
set -euo pipefail

# Enhanced build script with CRT dependency management
# Usage: ./build_all.sh [OPTIONS]
# Options:
#   --rebuild-crt           Force rebuild of all CRT dependencies
#   --check-crt-updates     Check if CRT dependencies need updating
#   --skip-crt-check        Skip CRT dependency checks (faster, but may miss updates)
#   --help                  Show this help message

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Parse command line arguments
FORCE_REBUILD_CRT=false
CHECK_CRT_UPDATES=true
SKIP_CRT_CHECK=false

show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --rebuild-crt           Force rebuild of all CRT dependencies"
    echo "  --check-crt-updates     Check if CRT dependencies need updating (default)"
    echo "  --skip-crt-check        Skip CRT dependency checks (faster)"
    echo "  --help                  Show this help message"
    echo ""
    echo "Environment variables (set via scripts/env_rdma.sh):"
    echo "  REPO_ROOT               Root directory of the repository"
    echo "  CRT_PREFIX              Installation prefix for CRT libraries"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --rebuild-crt)
            FORCE_REBUILD_CRT=true
            shift
            ;;
        --check-crt-updates)
            CHECK_CRT_UPDATES=true
            shift
            ;;
        --skip-crt-check)
            SKIP_CRT_CHECK=true
            CHECK_CRT_UPDATES=false
            shift
            ;;
        --help)
            show_help
            ;;
        *)
            echo "[build_all] ERROR: Unknown option: $1" >&2
            echo "Run '$0 --help' for usage information." >&2
            exit 1
            ;;
    esac
done

# Load env defaults if available
if [ -f "$SCRIPT_DIR/env_rdma.sh" ]; then
    # shellcheck disable=SC1091
    source "$SCRIPT_DIR/env_rdma.sh"
fi

# Require env variables (must source env_rdma.sh first)
if [ -z "${REPO_ROOT:-}" ]; then
    echo "[build_all] ERROR: REPO_ROOT not set. Please source scripts/env_rdma.sh first." >&2
    exit 1
fi
if [ -z "${CRT_PREFIX:-}" ]; then
    echo "[build_all] ERROR: CRT_PREFIX not set. Please source scripts/env_rdma.sh first." >&2
    exit 1
fi

CORE_SRC_DIR="$REPO_ROOT"
CORE_BUILD_DIR="$REPO_ROOT/build"
PLUGIN_SRC_DIR="$REPO_ROOT/plugins/cuobject"
PLUGIN_BUILD_DIR="$PLUGIN_SRC_DIR/build"
CRT_DIR="$REPO_ROOT/crt"

# Function to check if a CRT dependency needs updating
check_crt_dependency() {
    local dep_name=$1
    local dep_dir="$CRT_DIR/$dep_name"
    
    if [ ! -d "$dep_dir/.git" ]; then
        echo "[build_all] WARNING: $dep_name is not a git repository, skipping check" >&2
        return 1
    fi
    
    cd "$dep_dir"
    
    # Fetch latest without changing current state
    git fetch origin --quiet 2>/dev/null || {
        echo "[build_all] WARNING: Failed to fetch updates for $dep_name" >&2
        return 1
    }
    
    # Check if local is behind remote
    local local_commit=$(git rev-parse HEAD 2>/dev/null || echo "")
    local remote_commit=$(git rev-parse @{u} 2>/dev/null || echo "")
    
    if [ "$local_commit" != "$remote_commit" ]; then
        local commits_behind=$(git rev-list --count HEAD..@{u} 2>/dev/null || echo "0")
        if [ "$commits_behind" -gt 0 ]; then
            echo "[build_all] INFO: $dep_name is $commits_behind commit(s) behind remote"
            return 0
        fi
    fi
    
    return 1
}

# Function to rebuild a CRT dependency
rebuild_crt_dependency() {
    local dep_name=$1
    local dep_dir="$CRT_DIR/$dep_name"
    
    echo "[build_all] Rebuilding $dep_name..."
    
    if [ ! -d "$dep_dir/.git" ]; then
        echo "[build_all] ERROR: $dep_name directory does not exist or is not a git repository" >&2
        return 1
    fi
    
    cd "$dep_dir"
    
    # Pull latest changes
    echo "[build_all]   Pulling latest changes for $dep_name..."
    git pull origin main --quiet || {
        echo "[build_all] WARNING: Failed to pull latest changes for $dep_name" >&2
    }
    
    # Rebuild and install
    echo "[build_all]   Configuring $dep_name..."
    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF \
        -DCMAKE_PREFIX_PATH="$CRT_PREFIX" -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX" > /dev/null
    
    echo "[build_all]   Building $dep_name..."
    cmake --build build -j > /dev/null
    
    echo "[build_all]   Installing $dep_name to $CRT_PREFIX..."
    cmake --install build > /dev/null
    
    echo "[build_all] ✓ $dep_name rebuilt successfully"
}

# Check and optionally rebuild CRT dependencies
CRT_DEPS_TO_REBUILD=()

if [ "$FORCE_REBUILD_CRT" = true ]; then
    echo "[build_all] Force rebuilding all CRT dependencies..."
    CRT_DEPS_TO_REBUILD=("aws-c-common" "aws-c-io" "aws-c-cal" "aws-c-auth" "aws-c-http" "aws-c-compression" "aws-checksums" "aws-c-sdkutils")
elif [ "$CHECK_CRT_UPDATES" = true ] && [ "$SKIP_CRT_CHECK" = false ]; then
    echo "[build_all] Checking CRT dependencies for updates..."
    
    # Critical dependencies that should be checked (in dependency order)
    CRITICAL_DEPS=("aws-c-common" "aws-checksums" "aws-c-cal" "aws-c-compression" "aws-c-io" "aws-c-http" "aws-c-sdkutils" "aws-c-auth")
    
    for dep in "${CRITICAL_DEPS[@]}"; do
        if [ -d "$CRT_DIR/$dep" ]; then
            if check_crt_dependency "$dep"; then
                CRT_DEPS_TO_REBUILD+=("$dep")
            fi
        fi
    done
    
    if [ ${#CRT_DEPS_TO_REBUILD[@]} -eq 0 ]; then
        echo "[build_all] ✓ All CRT dependencies are up to date"
    else
        echo "[build_all] Found ${#CRT_DEPS_TO_REBUILD[@]} CRT dependency(ies) that need updating: ${CRT_DEPS_TO_REBUILD[*]}"
        echo "[build_all] Rebuilding outdated CRT dependencies..."
    fi
fi

# Rebuild CRT dependencies if needed
if [ ${#CRT_DEPS_TO_REBUILD[@]} -gt 0 ]; then
    for dep in "${CRT_DEPS_TO_REBUILD[@]}"; do
        rebuild_crt_dependency "$dep" || {
            echo "[build_all] ERROR: Failed to rebuild $dep" >&2
            exit 1
        }
    done
    echo "[build_all] CRT dependencies updated successfully"
fi

# Build core aws-c-s3
echo "[build_all] Configuring core at $CORE_SRC_DIR -> $CORE_BUILD_DIR (prefix=$CRT_PREFIX)"
cmake -S "$CORE_SRC_DIR" -B "$CORE_BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF \
    -DCMAKE_PREFIX_PATH="$CRT_PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$CRT_PREFIX" | cat

echo "[build_all] Building & installing core aws-c-s3"
cmake --build "$CORE_BUILD_DIR" --target install -j | cat

# Build plugin
echo "[build_all] Configuring plugin at $PLUGIN_SRC_DIR -> $PLUGIN_BUILD_DIR"

# Check for real cuObject library
EXTRA_CUOBJ=""
if [ -n "${CUOBJECT_LIB_DIR:-}" ]; then
    if [ -f "${CUOBJECT_LIB_DIR}/libcuobjclient.so" ]; then
        EXTRA_CUOBJ="-DCUOBJECT_LIBRARY=${CUOBJECT_LIB_DIR}/libcuobjclient.so"
        echo "[build_all] Using CUOBJECT_LIBRARY=${CUOBJECT_LIB_DIR}/libcuobjclient.so"
    elif [ -f "${CUOBJECT_LIB_DIR}/libcuobjclient.so.0" ]; then
        EXTRA_CUOBJ="-DCUOBJECT_LIBRARY=${CUOBJECT_LIB_DIR}/libcuobjclient.so.0"
        echo "[build_all] Using CUOBJECT_LIBRARY=${CUOBJECT_LIB_DIR}/libcuobjclient.so.0"
    else
        echo "[build_all] CUOBJECT_LIBRARY not found in ${CUOBJECT_LIB_DIR}; building mock plugin (no RDMA)"
    fi
else
    echo "[build_all] CUOBJECT_LIB_DIR unset; building mock plugin (no RDMA)"
fi

cmake -S "$PLUGIN_SRC_DIR" -B "$PLUGIN_BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF \
    -DCMAKE_PREFIX_PATH="$CRT_PREFIX" \
    ${CUOBJECT_ROOT_DIR:+-DCUOBJECT_ROOT_DIR="$CUOBJECT_ROOT_DIR"} \
    $EXTRA_CUOBJ | cat

echo "[build_all] Building plugin"
cmake --build "$PLUGIN_BUILD_DIR" --target cuobject_s3_plugin -j | cat

# Summary
echo ""
echo "[build_all] ✓ Build completed successfully!"
echo ""
echo "Artifacts:"
echo "  Core library:  $CRT_PREFIX/lib/libaws-c-s3.a"
echo "  Plugin library: $PLUGIN_BUILD_DIR/libcuobject_s3_plugin.so"
echo ""

if [ ${#CRT_DEPS_TO_REBUILD[@]} -gt 0 ]; then
    echo "CRT dependencies rebuilt: ${CRT_DEPS_TO_REBUILD[*]}"
    echo ""
fi

if [ -n "${CUOBJECT_ROOT_DIR:-}" ]; then
    echo "RDMA support is ENABLED"
else
    echo "RDMA support is DISABLED (mock plugin built)"
    echo "To enable RDMA, set CUOBJECT_ROOT_DIR in scripts/env_rdma.sh"
fi
