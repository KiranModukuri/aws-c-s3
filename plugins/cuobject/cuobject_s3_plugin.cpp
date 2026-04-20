/*
 * Copyright 1993-2025 NVIDIA Corporation.  All rights reserved.
 *
 * NOTICE TO LICENSEE:
 *
 * This source code and/or documentation ("Licensed Deliverables") are
 * subject to NVIDIA intellectual property rights under U.S. and
 * international Copyright laws.
 *
 * These Licensed Deliverables contained herein is PROPRIETARY and
 * CONFIDENTIAL to NVIDIA and is being provided under the terms and
 * conditions of a form of NVIDIA software license agreement by and
 * between NVIDIA and Licensee ("License Agreement") or electronically
 * accepted by Licensee.  Notwithstanding any terms or conditions to
 * the contrary in the License Agreement, reproduction or disclosure
 * of the Licensed Deliverables to any third party without the express
 * written consent of NVIDIA is prohibited.
 *
 * NOTWITHSTANDING ANY TERMS OR CONDITIONS TO THE CONTRARY IN THE
 * LICENSE AGREEMENT, NVIDIA MAKES NO REPRESENTATION ABOUT THE
 * SUITABILITY OF THESE LICENSED DELIVERABLES FOR ANY PURPOSE.  IT IS
 * PROVIDED "AS IS" WITHOUT EXPRESS OR IMPLIED WARRANTY OF ANY KIND.
 * NVIDIA DISCLAIMS ALL WARRANTIES WITH REGARD TO THESE LICENSED
 * DELIVERABLES, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY,
 * NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE.
 * NOTWITHSTANDING ANY TERMS OR CONDITIONS TO THE CONTRARY IN THE
 * LICENSE AGREEMENT, IN NO EVENT SHALL NVIDIA BE LIABLE FOR ANY
 * SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, OR ANY
 * DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THESE LICENSED DELIVERABLES.
 *
 * U.S. Government End Users.  These Licensed Deliverables are a
 * "commercial item" as that term is defined at 48 C.F.R. 2.101 (OCT
 * 1995), consisting of "commercial computer software" and "commercial
 * computer software documentation" as such terms are used in 48
 * C.F.R. 12.212 (SEPT 1995) and is provided to the U.S. Government
 * only as a commercial end item.  Consistent with 48 C.F.R.12.212 and
 * 48 C.F.R. 227.7202-1 through 227.7202-4 (JUNE 1995), all
 * U.S. Government End Users acquire the Licensed Deliverables with
 * only those rights set forth herein.
 *
 * Any use of the Licensed Deliverables in individual and commercial
 * software must include, in the user documentation and internal
 * comments to the code, the above Disclaimer and U.S. Government End
 * Users Notice.
 */

#include "cuobject_s3_plugin.h"
#include "cuobjclient.h"

#include <aws/common/allocator.h>
#include <aws/common/byte_buf.h>
#include <aws/common/json.h>
#include <aws/common/logging.h>
#include <aws/common/string.h>

#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <cstring>
#include <atomic>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <stdlib.h>
#include <vector>

extern "C" {

/**
 * cuObject provider instance
 */
struct cuobject_provider {
    struct aws_allocator *allocator;

    /* cuObjClient C++ instance */
    std::unique_ptr<cuObjClient> client;

    /* Configuration */
    struct cuobject_plugin_config config;

    /* Memory registration tracking */
    std::unordered_map<void*, size_t> registered_memory;
    std::mutex memory_mutex;

    /* RDMA token tracking */
    std::unordered_map<std::string, void*> pending_operations;
    std::mutex operations_mutex;

    /* Token storage for memory lifecycle management */
    std::unordered_map<std::string, std::string> stored_tokens;
    std::mutex tokens_mutex;

    /* Token counter for unique IDs */
    std::atomic<uint64_t> token_counter;
};

/**
 * Context for cuObjClient callbacks
 */
struct cuobject_operation_context {
    struct cuobject_provider *provider;
    std::string s3_key;
    std::string rdma_token;
    void *buffer;
    void *base_ptr;           // Registered base pointer
    size_t size;
    size_t offset;
    size_t buffer_offset;     // Offset from base pointer
    cuObjOpType_t operation_type;

    // Storage for RDMA info received from synchronous callbacks
    cufileRDMAInfo_t rdma_info;
    char rdma_desc_buffer[1024];  // Our own writable buffer for RDMA descriptor
    bool rdma_info_received;

    // Note: For future async implementation, we might need to add:
    // - Signal handling for async completion
    // - Thread pools for non-blocking operations
    // - Event loops for callback management
};

// Forward declarations
static int parse_plugin_config(struct cuobject_plugin_config *out_config);
static std::string generate_rdma_token_via_cuobject(struct cuobject_provider *provider, const std::string &s3_key,
                                                   void *buffer, void *base_ptr, size_t size, size_t buffer_offset, cuObjOpType_t op_type);

/**
 * cuObjClient callback for PUT operations
 */
static ssize_t cuobject_put_callback(
    const void *handle,
    const char* buf,
    size_t size,
    loff_t offset,
    const cufileRDMAInfo_t *rdma_info) {

    cuobject_operation_context *ctx = static_cast<cuobject_operation_context*>(
        cuObjClient::getCtx(handle));

    if (!ctx || !rdma_info) {
        return -1;
    }

    // Store RDMA info in context (synchronous - no locking needed)
    if (rdma_info->desc_len > 0 && rdma_info->desc_str) {
        // Copy descriptor string to our writable buffer
        size_t copy_len = std::min(static_cast<size_t>(rdma_info->desc_len), sizeof(ctx->rdma_desc_buffer) - 1);
        memcpy(ctx->rdma_desc_buffer, rdma_info->desc_str, copy_len);

        // Check if original descriptor already has NULL terminator
        if (copy_len > 0 && ctx->rdma_desc_buffer[copy_len - 1] == '\0') {
            // Original descriptor included NULL terminator - exclude it from length
            ctx->rdma_info.desc_len = copy_len - 1;
        } else {
            // Original descriptor was clean - add NULL terminator for C string safety but don't count it
            ctx->rdma_desc_buffer[copy_len] = '\0';
            ctx->rdma_info.desc_len = copy_len;  // Length excludes NULL terminator
        }

        // Point the rdma_info desc_str to our buffer
        ctx->rdma_info.desc_str = ctx->rdma_desc_buffer;
    } else {
        ctx->rdma_info.desc_len = 0;
    }
    ctx->rdma_info_received = true;

    if (ctx->provider->config.debug_logging) {
        printf("[CUOBJECT_PLUGIN] PUT callback: handle=%p, buf=%p, size=%zu, offset=%lld\n",
               handle, (void*)buf, size, (long long)offset);
        printf("[CUOBJECT_PLUGIN] RDMA descriptor: original_len=%d, final_len=%d, desc=%.*s\n",
                rdma_info->desc_len, ctx->rdma_info.desc_len, ctx->rdma_info.desc_len, ctx->rdma_info.desc_str);
    }

    // Return 0 to indicate we've stored the RDMA info and don't need actual data transfer
    return 0;
}

/**
 * cuObjClient callback for GET operations
 */
static ssize_t cuobject_get_callback(
    const void *handle,
    char* buf,
    size_t size,
    loff_t offset,
    const cufileRDMAInfo_t *rdma_info) {

    cuobject_operation_context *ctx = static_cast<cuobject_operation_context*>(
        cuObjClient::getCtx(handle));

    if (!ctx || !rdma_info) {
        return -1;
    }

    // Store RDMA info in context (synchronous - no locking needed)
    if (rdma_info->desc_len > 0 && rdma_info->desc_str) {
        // Copy descriptor string to our writable buffer
        size_t copy_len = std::min(static_cast<size_t>(rdma_info->desc_len), sizeof(ctx->rdma_desc_buffer) - 1);
        memcpy(ctx->rdma_desc_buffer, rdma_info->desc_str, copy_len);

        // Check if original descriptor already has NULL terminator
        if (copy_len > 0 && ctx->rdma_desc_buffer[copy_len - 1] == '\0') {
            // Original descriptor included NULL terminator - exclude it from length
            ctx->rdma_info.desc_len = copy_len - 1;
        } else {
            // Original descriptor was clean - add NULL terminator for C string safety but don't count it
            ctx->rdma_desc_buffer[copy_len] = '\0';
            ctx->rdma_info.desc_len = copy_len;  // Length excludes NULL terminator
        }

        // Point the rdma_info desc_str to our buffer
        ctx->rdma_info.desc_str = ctx->rdma_desc_buffer;
    } else {
        ctx->rdma_info.desc_len = 0;
    }
    ctx->rdma_info_received = true;

    if (ctx->provider->config.debug_logging) {
        printf("[CUOBJECT_PLUGIN] GET callback: handle=%p, buf=%p, size=%zu, offset=%lld\n",
               handle, (void*)buf, size, (long long)offset);
        printf("[CUOBJECT_PLUGIN] RDMA descriptor: original_len=%d, final_len=%d, desc=%.*s\n",
                rdma_info->desc_len, ctx->rdma_info.desc_len, ctx->rdma_info.desc_len, ctx->rdma_info.desc_str);
    }

    // Return 0 to indicate we've stored the RDMA info and don't need actual data transfer
    return 0;
}

/**
 * Initialize cuObject provider
 */
static int cuobject_init(
    struct aws_allocator *allocator,
    struct aws_s3_rdma_provider **out_provider) {

    if (!allocator || !out_provider) {
        return AWS_OP_ERR;
    }

    try {
        auto provider = std::make_unique<cuobject_provider>();
        provider->allocator = allocator;
        provider->token_counter = 1;

        // Parse configuration
        if (parse_plugin_config(&provider->config) != AWS_OP_SUCCESS) {
            return AWS_OP_ERR;
        }

        // Set up cuObjClient callbacks
        CUObjIOOps ops = {
            .get = cuobject_get_callback,
            .put = cuobject_put_callback
        };


        provider->client = std::make_unique<cuObjClient>(ops, CUOBJ_PROTO_RDMA_DC_V1);
        if (!provider->client->isConnected()) {
            printf("[CUOBJECT_PLUGIN] Warning: cuObjClient not connected to RDMA server\n");
            printf("[CUOBJECT_PLUGIN] Note: This is expected if cuObject server is not running\n");
            printf("[CUOBJECT_PLUGIN] Plugin will continue with mock RDMA operations for testing\n");
        }

        printf("[CUOBJECT_PLUGIN] Initialized cuObject provider\n");

        *out_provider = reinterpret_cast<struct aws_s3_rdma_provider*>(provider.release());
        return AWS_OP_SUCCESS;

    } catch (const std::exception& e) {
        printf("[CUOBJECT_PLUGIN] Error initializing provider: %s\n", e.what());
        return AWS_OP_ERR;
    }
}

/**
 * Cleanup cuObject provider
 */
static void cuobject_cleanup(struct aws_s3_rdma_provider *provider) {
    if (!provider) {
        return;
    }

    cuobject_provider *cuobj_provider = reinterpret_cast<cuobject_provider*>(provider);

    // Cleanup C++ objects (unique_ptr handles cuObjClient cleanup)
    delete cuobj_provider;
}

/**
 * Register memory for RDMA
 */
static int cuobject_register_memory(
    struct aws_s3_rdma_provider *provider,
    void *ptr,
    size_t size) {

    if (!provider || !ptr) {
        return AWS_OP_ERR;
    }

    cuobject_provider *cuobj_provider = reinterpret_cast<cuobject_provider*>(provider);

    // Check memory type
    cuObjMemoryType_t mem_type = cuObjClient::getMemoryType(ptr);

    if (cuobj_provider->config.debug_logging) {
        printf("[CUOBJECT_PLUGIN] Registering memory: ptr=%p, size=%zu, type=%d\n",
               ptr, size, (int)mem_type);
    }

    // Register with cuObjClient
    cuObjErr_t result = cuobj_provider->client->cuMemObjGetDescriptor(ptr, size);
    if (result != CU_OBJ_SUCCESS) {
        return AWS_OP_ERR;
    }

    // Track registered memory
    {
        std::lock_guard<std::mutex> lock(cuobj_provider->memory_mutex);
        cuobj_provider->registered_memory[ptr] = size;
    }

    if (cuobj_provider->config.debug_logging) {
        printf("[CUOBJECT_PLUGIN] Registered memory: ptr=%p, size=%zu, type=%d\n",
               ptr, size, (int)mem_type);
    }

    return AWS_OP_SUCCESS;
}

/**
 * Deregister memory
 */
static int cuobject_deregister_memory(
    struct aws_s3_rdma_provider *provider,
    void *ptr) {

    if (!provider || !ptr) {
        return AWS_OP_ERR;
    }

    cuobject_provider *cuobj_provider = reinterpret_cast<cuobject_provider*>(provider);

    {
        std::lock_guard<std::mutex> lock(cuobj_provider->memory_mutex);
        auto it = cuobj_provider->registered_memory.find(ptr);
        if (it != cuobj_provider->registered_memory.end()) {
            cuObjErr_t result = cuobj_provider->client->cuMemObjPutDescriptor(ptr);
            cuobj_provider->registered_memory.erase(it);

            if (cuobj_provider->config.debug_logging) {
                printf("[CUOBJECT_PLUGIN] Deregistered memory: ptr=%p\n", ptr);
            }

            return (result == CU_OBJ_SUCCESS) ? AWS_OP_SUCCESS : AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS; // Not registered, no error
}

/**
 * Check if memory is suitable for RDMA
 */
static bool cuobject_is_memory_suitable(
    struct aws_s3_rdma_provider *provider,
    const void *ptr,
    size_t size) {

    if (!provider || !ptr) {
        return false;
    }

    cuobject_provider *cuobj_provider = reinterpret_cast<cuobject_provider*>(provider);

    // Check memory type
    cuObjMemoryType_t mem_type = cuObjClient::getMemoryType(ptr);

    // Accept both GPU memory and system memory for RDMA
    // GPU memory gets direct RDMA, system memory can use staging/copy mechanisms
    bool suitable = (mem_type == CUOBJ_MEMORY_CUDA_DEVICE ||
                    mem_type == CUOBJ_MEMORY_CUDA_MANAGED ||
                    mem_type == CUOBJ_MEMORY_SYSTEM);

    if (cuobj_provider->config.debug_logging) {
        printf("[CUOBJECT_PLUGIN] Memory suitability check: ptr=%p, size=%zu, type=%d, suitable=%s\n",
               ptr, size, (int)mem_type, suitable ? "yes" : "no");
    }

    return suitable;
}

/**
 * Get maximum transfer size
 */
static size_t cuobject_get_max_transfer_size(
    struct aws_s3_rdma_provider *provider,
    const void *ptr) {

    if (!provider || !ptr) {
        return 0;
    }

    cuobject_provider *cuobj_provider = reinterpret_cast<cuobject_provider*>(provider);
    return cuobj_provider->client->cuMemObjGetMaxRequestCallbackSize(const_cast<void*>(ptr));
}

/**
 * Prepare RDMA token for PUT operation
 */
static int cuobject_prepare_put_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *s3_key,
    const void *buffer,
    size_t size,
    size_t offset,
    struct aws_byte_cursor *out_rdma_token) {

    if (!provider || !s3_key || !buffer || !out_rdma_token) {
        return AWS_OP_ERR;
    }

    cuobject_provider *cuobj_provider = reinterpret_cast<cuobject_provider*>(provider);

    std::string key_str(reinterpret_cast<const char*>(s3_key->ptr), s3_key->len);

    // For now, assume buffer is the base pointer and offset is 0
    // This may need adjustment based on actual memory registration patterns
    void *base_ptr = const_cast<void*>(buffer);
    size_t buffer_offset = offset;

    std::string token = generate_rdma_token_via_cuobject(cuobj_provider, key_str, const_cast<void*>(buffer),
                                                        base_ptr, size, buffer_offset, CUOBJ_PUT);

    // Check if token generation failed
    if (token.empty()) {
        if (cuobj_provider->config.debug_logging) {
            printf("[CUOBJECT_PLUGIN] PUT token generation failed - returning error to AWS CRT\n");
        }
        return AWS_OP_ERR; // AWS CRT will fall back to HTTP transfer
    }

    // Store token in provider instance for proper lifecycle management
    // This ensures the memory remains valid until the provider is cleaned up
    {
        std::lock_guard<std::mutex> lock(cuobj_provider->tokens_mutex);
        cuobj_provider->stored_tokens[token] = token;
    }

    // Get reference to stored token (safe because map doesn't reallocate existing entries)
    const std::string& stored_token = cuobj_provider->stored_tokens[token];

    out_rdma_token->ptr = (uint8_t*)stored_token.c_str();
    out_rdma_token->len = stored_token.length();

    if (cuobj_provider->config.debug_logging) {
        printf("[CUOBJECT_PLUGIN] Generated PUT token: %s for key: %s\n", token.c_str(), key_str.c_str());
    }

    return AWS_OP_SUCCESS;
}

/**
 * Prepare RDMA token for GET operation
 */
static int cuobject_prepare_get_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *s3_key,
    void *buffer,
    size_t size,
    size_t offset,
    struct aws_byte_cursor *out_rdma_token) {

    if (!provider || !s3_key || !buffer || !out_rdma_token) {
        return AWS_OP_ERR;
    }

    cuobject_provider *cuobj_provider = reinterpret_cast<cuobject_provider*>(provider);

    std::string key_str(reinterpret_cast<const char*>(s3_key->ptr), s3_key->len);

    // For now, assume buffer is the base pointer and offset is 0
    // This may need adjustment based on actual memory registration patterns
    void *base_ptr = buffer;
    size_t buffer_offset = offset;

    std::string token = generate_rdma_token_via_cuobject(cuobj_provider, key_str, buffer,
                                                        base_ptr, size, buffer_offset, CUOBJ_GET);

    // Check if token generation failed
    if (token.empty()) {
        if (cuobj_provider->config.debug_logging) {
            printf("[CUOBJECT_PLUGIN] GET token generation failed - returning error to AWS CRT\n");
        }
        return AWS_OP_ERR; // AWS CRT will fall back to HTTP transfer
    }

    // Store token in provider instance for proper lifecycle management
    // This ensures the memory remains valid until the provider is cleaned up
    {
        std::lock_guard<std::mutex> lock(cuobj_provider->tokens_mutex);
        cuobj_provider->stored_tokens[token] = token;
    }

    // Get reference to stored token (safe because map doesn't reallocate existing entries)
    const std::string& stored_token = cuobj_provider->stored_tokens[token];

    out_rdma_token->ptr = (uint8_t*)stored_token.c_str();
    out_rdma_token->len = stored_token.length();

    if (cuobj_provider->config.debug_logging) {
        printf("[CUOBJECT_PLUGIN] Generated GET token: %s for key: %s\n", token.c_str(), key_str.c_str());
    }

    return AWS_OP_SUCCESS;
}

/**
 * Process RDMA reply token from server
 */
static int cuobject_process_reply_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *rdma_reply_token,
    void *user_data,
    aws_s3_rdma_completion_fn *completion_callback) {

    if (!provider || !rdma_reply_token) {
        return AWS_OP_ERR;
    }

    cuobject_provider *cuobj_provider = reinterpret_cast<cuobject_provider*>(provider);

    std::string reply_token(reinterpret_cast<const char*>(rdma_reply_token->ptr), rdma_reply_token->len);

    if (cuobj_provider->config.debug_logging) {
        printf("[CUOBJECT_PLUGIN] Processing reply token: %s\n", reply_token.c_str());
    }

    // In a real implementation, this would:
    // 1. Parse the reply token to get operation details
    // 2. Trigger cuObjClient to perform the actual RDMA operation
    // 3. Wait for completion and invoke the callback

    // For testing, we'll just invoke the callback immediately with success
    if (completion_callback) {
        completion_callback(user_data, AWS_ERROR_SUCCESS, rdma_reply_token);
    }

    return AWS_OP_SUCCESS;
}

// Helper functions

static int parse_plugin_config(struct cuobject_plugin_config *out_config) {
    // Use default configuration
    out_config->max_buffer_size = 1024 * 1024 * 1024; // 1GB
    out_config->protocol_version = CUOBJ_PROTO_RDMA_DC_V1;

    // Turn on debug logging by setting env CUOBJECT_DEBUG_LOGGING=1
    const char* dbg_log = std::getenv("CUOBJECT_DEBUG_LOGGING");
    out_config->debug_logging = (dbg_log != nullptr && std::string(dbg_log) == "1");

    return AWS_OP_SUCCESS;
}

/**
 * Generate RDMA token by calling cuObjClient PUT/GET operations
 *
 * Returns empty string on failure - AWS CRT will fall back to HTTP transfer
 */
static std::string generate_rdma_token_via_cuobject(struct cuobject_provider *provider, const std::string &s3_key,
                                                   void *buffer, void *base_ptr, size_t size, size_t buffer_offset, cuObjOpType_t op_type) {
    // Create operation context for the cuObjClient operation
    auto ctx = std::make_unique<cuobject_operation_context>();
    ctx->provider = provider;
    ctx->s3_key = s3_key;
    ctx->buffer = buffer;
    ctx->base_ptr = base_ptr;
    ctx->size = size;
    ctx->buffer_offset = buffer_offset;
    ctx->operation_type = op_type;
    ctx->rdma_info_received = false;

    if (provider->config.debug_logging) {
        printf("[CUOBJECT_PLUGIN] Generating RDMA token via cuObjClient: key=%s, base_ptr=%p, buffer=%p, size=%zu, offset=%zu\n",
               s3_key.c_str(), base_ptr, buffer, size, buffer_offset);
        printf("[CUOBJECT_PLUGIN] cuObjClient connected: %s\n", provider->client->isConnected() ? "yes" : "no");
    }

    // Check if cuObjClient is connected
    if (!provider->client->isConnected()) {
        if (provider->config.debug_logging) {
            printf("[CUOBJECT_PLUGIN] cuObjClient not connected - RDMA token generation failed\n");
        }
        return ""; // Return empty string to indicate failure
    }

    try {
        // Call cuObjClient operation with registered base pointer and buffer offset
        ssize_t result;
        if (op_type == CUOBJ_PUT) {
            if (provider->config.debug_logging) {
                printf("[CUOBJECT_PLUGIN] Calling cuObjPut with ctx=%p, key=%s, ptr=%p, size=%zu, offset=%zu\n",
                       ctx.get(), s3_key.c_str(), base_ptr, size, buffer_offset);
            }
            result = provider->client->cuObjPut(ctx.get(), base_ptr, size, 0, buffer_offset);
        } else {
            if (provider->config.debug_logging) {
                printf("[CUOBJECT_PLUGIN] Calling cuObjGet with ctx=%p, key=%s, ptr=%p, size=%zu, offset=%zu\n",
                       ctx.get(), s3_key.c_str(), base_ptr, size, buffer_offset);
            }
            result = provider->client->cuObjGet(ctx.get(), base_ptr, size, 0, buffer_offset);
        }

        if (provider->config.debug_logging) {
            printf("[CUOBJECT_PLUGIN] cuObjClient operation result: %zd (success: >=0)\n", result);
        }

        if (result < 0) {
            printf("[CUOBJECT_PLUGIN] ERROR: cuObjClient operation failed: %zd - RDMA token generation failed\n", result);
            return ""; // Return empty string to indicate failure
        }

        // Since cuObjPut/Get are synchronous, RDMA info should already be available
        // Create token from actual RDMA descriptor
        if (ctx->rdma_info_received && ctx->rdma_info.desc_len > 0 && ctx->rdma_info.desc_str[0] != '\0') {
            // For PUT, patch size field (2nd colon-delimited field) to the payload size used
            if (op_type == CUOBJ_PUT) {
                char *desc_buf = ctx->rdma_desc_buffer;
                size_t desc_len = (size_t)ctx->rdma_info.desc_len;
                // Locate the first two ':' characters in the descriptor.
                // The RDMA token format is "<addr>:<size_hex>:..."; we rewrite <size_hex> in place.
                size_t first_colon = SIZE_MAX, second_colon = SIZE_MAX;
                for (size_t i = 0; i < desc_len; ++i) {
                    if (desc_buf[i] == ':') { first_colon = i; break; }
                }
                for (size_t i = first_colon + 1; i < desc_len; ++i) {
                    if (desc_buf[i] == ':') { second_colon = i; break; }
                }
                {
                    // Compute the width of the size field and overwrite it with zero-padded lowercase hex of payload size
                    size_t width = (second_colon > first_colon + 1) ? (second_colon - first_colon - 1) : 0;
                    char tmp[32];
                    if (width > 0 && width < sizeof(tmp)) {
                        char *field_start = desc_buf + first_colon + 1;
                        int n = snprintf(tmp, sizeof(tmp), "%0*zx", (int)width, size);
                        if (n > 0) {
                            memcpy(field_start, tmp, width);
                            if (provider->config.debug_logging) {
                                printf("[CUOBJECT_PLUGIN] RDMA: patched token size=%zu bytes\n", size);
                            }
                        }
                    }
                }
            }
            // Use the (possibly patched) RDMA descriptor as the token
            // Callbacks now ensure correct length without NULL terminator issues
            std::string rdma_token = std::string(ctx->rdma_info.desc_str, ctx->rdma_info.desc_len);

            // Store operation context for later retrieval
            {
                std::lock_guard<std::mutex> lock(provider->operations_mutex);
                provider->pending_operations[rdma_token] = buffer;
            }

            return rdma_token;
        } else {
            if (provider->config.debug_logging) {
                printf("[CUOBJECT_PLUGIN] ERROR: No valid RDMA descriptor received - RDMA token generation failed\n");
                printf("[CUOBJECT_PLUGIN] RDMA info received: %s, desc_len: %d\n",
                       ctx->rdma_info_received ? "yes" : "no", ctx->rdma_info.desc_len);
            }
            return ""; // Return empty string to indicate failure
        }

    } catch (const std::exception& e) {
        printf("[CUOBJECT_PLUGIN] ERROR: Exception during RDMA token generation: %s - RDMA token generation failed\n", e.what());
        return ""; // Return empty string to indicate failure
    }
}

// Configurable RDMA header names (initialized from environment variables)
static std::string g_rdma_token_header_name = "x-amz-rdma-token";
static std::string g_rdma_reply_header_name = "x-amz-rdma-reply";
static std::string g_rdma_bytes_header_name = "x-amz-rdma-bytes";
static bool g_header_names_initialized = false;

// Initialize header names from environment variables
static void init_header_names_from_env() {
    if (g_header_names_initialized) {
        return;
    }
    
    const char* token_header = std::getenv("CUOBJECT_RDMA_TOKEN_HEADER_NAME");
    if (token_header && strlen(token_header) > 0) {
        g_rdma_token_header_name = token_header;
        printf("[CUOBJECT_PLUGIN] Using custom RDMA token header: %s\n", token_header);
    }
    
    const char* reply_header = std::getenv("CUOBJECT_RDMA_REPLY_HEADER_NAME");
    if (reply_header && strlen(reply_header) > 0) {
        g_rdma_reply_header_name = reply_header;
        printf("[CUOBJECT_PLUGIN] Using custom RDMA reply header: %s\n", reply_header);
    }
    
    const char* bytes_header = std::getenv("CUOBJECT_RDMA_BYTES_HEADER_NAME");
    if (bytes_header && strlen(bytes_header) > 0) {
        g_rdma_bytes_header_name = bytes_header;
        printf("[CUOBJECT_PLUGIN] Using custom RDMA bytes header: %s\n", bytes_header);
    }
    
    g_header_names_initialized = true;
}

// Helper function to create byte cursor from C string without AWS Common dependency
static struct aws_byte_cursor byte_cursor_from_c_str(const char *c_str) {
    struct aws_byte_cursor cursor;
    cursor.ptr = (uint8_t *)c_str;
    cursor.len = strlen(c_str);
    return cursor;
}

// Header name functions
static struct aws_byte_cursor cuobject_get_rdma_token_header_name(struct aws_s3_rdma_provider *provider) {
    (void)provider; /* unused parameter */
    init_header_names_from_env();
    return byte_cursor_from_c_str(g_rdma_token_header_name.c_str());
}

static struct aws_byte_cursor cuobject_get_rdma_reply_header_name(struct aws_s3_rdma_provider *provider) {
    (void)provider; /* unused parameter */
    init_header_names_from_env();
    return byte_cursor_from_c_str(g_rdma_reply_header_name.c_str());
}

static struct aws_byte_cursor cuobject_get_rdma_bytes_header_name(struct aws_s3_rdma_provider *provider) {
    (void)provider; /* unused parameter */
    init_header_names_from_env();
    return byte_cursor_from_c_str(g_rdma_bytes_header_name.c_str());
}

// Plugin vtable
static const struct aws_s3_rdma_provider_vtable s_cuobject_vtable = {
    .provider_name = "cuObject RDMA Provider",
    .provider_version = 1,
    .init = cuobject_init,
    .cleanup = cuobject_cleanup,
    .register_memory = cuobject_register_memory,
    .deregister_memory = cuobject_deregister_memory,
    .is_memory_suitable = cuobject_is_memory_suitable,
    .get_max_transfer_size = cuobject_get_max_transfer_size,
    .prepare_put_token = cuobject_prepare_put_token,
    .prepare_get_token = cuobject_prepare_get_token,
    .process_reply_token = cuobject_process_reply_token,
    .get_rdma_token_header_name = cuobject_get_rdma_token_header_name,
    .get_rdma_reply_header_name = cuobject_get_rdma_reply_header_name,
    .get_rdma_bytes_header_name = cuobject_get_rdma_bytes_header_name,
};

// Plugin entry point
__attribute__((visibility("default")))
const struct aws_s3_rdma_provider_vtable *aws_s3_rdma_provider_get_vtable(void) {
    return &s_cuobject_vtable;
}

} // extern "C"
