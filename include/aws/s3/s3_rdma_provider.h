#ifndef AWS_S3_RDMA_PROVIDER_H
#define AWS_S3_RDMA_PROVIDER_H

/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <aws/common/common.h>
#include <aws/common/byte_buf.h>
#include <aws/common/ref_count.h>
#include <aws/s3/exports.h>

AWS_PUSH_SANE_WARNING_LEVEL

AWS_EXTERN_C_BEGIN

/**
 * @file s3_rdma_provider.h
 * @brief Generic RDMA provider interface for AWS S3 CRT client
 *
 * This interface allows pluggable RDMA providers to be dynamically loaded
 * and used for high-performance GPU-to-storage transfers.
 */

struct aws_s3_rdma_provider;
struct aws_s3_request;
struct aws_s3_meta_request;
struct aws_allocator;
struct aws_byte_cursor;
struct aws_http_message;
struct aws_http_header;
struct aws_s3_rdma_request_handler;
struct aws_s3_rdma_buffer_manager;

/**
 * @brief RDMA provider configuration
 */
struct aws_s3_rdma_provider_config {
    struct aws_allocator *allocator;
    
    /* Plugin library path (e.g., "libcuobject_s3_plugin.so") */
    struct aws_byte_cursor plugin_path;
    
    /* Enable/disable RDMA */
    bool enable_rdma;
    
    /* Minimum transfer size to use RDMA (smaller transfers use regular HTTP) */
    size_t rdma_threshold;
    
    /* Maximum number of concurrent RDMA operations */
    size_t max_concurrent_operations;
};

/**
 * @brief RDMA operation completion callback
 */
typedef void aws_s3_rdma_completion_fn(
    void *user_data,
    int error_code,
    const struct aws_byte_cursor *rdma_reply_token);

#define AWS_S3_RDMA_PROVIDER_VERSION_1 1u
#define AWS_S3_RDMA_PROVIDER_VERSION_2 2u

/**
 * @brief RDMA provider vtable interface
 * 
 * This vtable is implemented by RDMA provider plugins and loaded dynamically.
 */
struct aws_s3_rdma_provider_vtable {
    /* Provider name for logging/debugging */
    const char *provider_name;
    
    /* Provider version */
    uint32_t provider_version;
    
    /**
     * Initialize the RDMA provider
     * @param allocator Memory allocator
     * @param out_provider Output provider instance
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*init)(
        struct aws_allocator *allocator,
        struct aws_s3_rdma_provider **out_provider);
    
    /**
     * Cleanup the RDMA provider
     * @param provider Provider instance to cleanup
     */
    void (*cleanup)(struct aws_s3_rdma_provider *provider);
    
    /**
     * Register memory for RDMA operations
     * @param provider Provider instance
     * @param ptr Memory pointer
     * @param size Memory size
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*register_memory)(
        struct aws_s3_rdma_provider *provider,
        void *ptr,
        size_t size);
    
    /**
     * Deregister memory from RDMA operations
     * @param provider Provider instance
     * @param ptr Memory pointer
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*deregister_memory)(
        struct aws_s3_rdma_provider *provider,
        void *ptr);

    /**
     * Check if memory is suitable for RDMA operations
     * @param provider Provider instance
     * @param ptr Memory pointer
     * @param size Memory size
     * @return true if suitable for RDMA, false otherwise
     */
    bool (*is_memory_suitable)(
        struct aws_s3_rdma_provider *provider,
        const void *ptr,
        size_t size);
    
    /**
     * Get maximum transfer size for given memory
     * @param provider Provider instance
     * @param ptr Memory pointer
     * @return Maximum transfer size, or 0 if not suitable
     */
    size_t (*get_max_transfer_size)(
        struct aws_s3_rdma_provider *provider,
        const void *ptr);
    
    /**
     * Prepare RDMA token for PUT operation
     * @param provider Provider instance
     * @param s3_key S3 object key
     * @param buffer Memory buffer
     * @param size Transfer size
     * @param offset Transfer offset
     * @param out_rdma_token Output RDMA token for x-rdma-token header
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*prepare_put_token)(
        struct aws_s3_rdma_provider *provider,
        const struct aws_byte_cursor *s3_key,
        const void *buffer,
        size_t size,
        size_t offset,
        struct aws_byte_cursor *out_rdma_token);
    
    /**
     * Prepare RDMA token for GET operation
     * @param provider Provider instance
     * @param s3_key S3 object key
     * @param buffer Memory buffer
     * @param size Transfer size
     * @param offset Transfer offset
     * @param out_rdma_token Output RDMA token for x-rdma-token header
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*prepare_get_token)(
        struct aws_s3_rdma_provider *provider,
        const struct aws_byte_cursor *s3_key,
        void *buffer,
        size_t size,
        size_t offset,
        struct aws_byte_cursor *out_rdma_token);
    
    /**
     * Process RDMA reply token from server
     * @param provider Provider instance
     * @param rdma_reply_token Token from x-rdma-reply header
     * @param user_data User context for completion callback
     * @param completion_callback Callback to invoke when RDMA operation completes
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*process_reply_token)(
        struct aws_s3_rdma_provider *provider,
        const struct aws_byte_cursor *rdma_reply_token,
        void *user_data,
        aws_s3_rdma_completion_fn *completion_callback);
    
    /**
     * Get RDMA token header name (e.g., "x-amz-rdma-token")
     * @param provider Provider instance
     * @return Byte cursor pointing to the header name string
     */
    struct aws_byte_cursor (*get_rdma_token_header_name)(
        struct aws_s3_rdma_provider *provider);
    
    /**
     * Get RDMA reply header name (e.g., "x-amz-rdma-reply")
     * @param provider Provider instance
     * @return Byte cursor pointing to the header name string
     */
    struct aws_byte_cursor (*get_rdma_reply_header_name)(
        struct aws_s3_rdma_provider *provider);
    
    /**
     * Get RDMA bytes header name (e.g., "x-amz-rdma-bytes")
     * @param provider Provider instance
     * @return Byte cursor pointing to the header name string
     */
    struct aws_byte_cursor (*get_rdma_bytes_header_name)(
        struct aws_s3_rdma_provider *provider);

    /* Version 2: register a caller-owned base allocation once and reuse its slices. */
    int (*pre_register_memory)(
        struct aws_s3_rdma_provider *provider,
        void *base,
        size_t size);

    int (*release_memory)(
        struct aws_s3_rdma_provider *provider,
        void *base);

    bool (*is_slice_pre_registered)(
        struct aws_s3_rdma_provider *provider,
        const void *ptr,
        size_t size);
};

/**
 * @brief RDMA request handler vtable interface
 * 
 * This vtable abstracts RDMA request processing logic that was previously
 * scattered throughout the core S3 library files.
 */
struct aws_s3_rdma_request_handler_vtable {
    /* Handler name for logging/debugging */
    const char *handler_name;
    
    /**
     * Initialize the request handler
     * @param allocator Memory allocator
     * @param rdma_provider Associated RDMA provider
     * @param out_handler Output request handler instance
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*init)(
        struct aws_allocator *allocator,
        struct aws_s3_rdma_provider *rdma_provider,
        struct aws_s3_rdma_request_handler **out_handler);
    
    /**
     * Cleanup the request handler
     * @param handler Request handler instance to cleanup
     */
    void (*cleanup)(struct aws_s3_rdma_request_handler *handler);
    
    /**
     * Process RDMA token for outgoing requests (PUT/GET)
     * @param handler Request handler instance
     * @param meta_request Meta request context
     * @param request Individual request
     * @param request_buffer Buffer for RDMA operations
     * @param buffer_size Size of the buffer
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*process_request_token)(
        struct aws_s3_rdma_request_handler *handler,
        struct aws_s3_meta_request *meta_request,
        struct aws_s3_request *request,
        void *request_buffer,
        size_t buffer_size);
    
    /**
     * Process RDMA reply token from incoming responses
     * @param handler Request handler instance
     * @param meta_request Meta request context
     * @param request Individual request
     * @param reply_token_value Token from x-rdma-reply header
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*process_reply_token)(
        struct aws_s3_rdma_request_handler *handler,
        struct aws_s3_meta_request *meta_request,
        struct aws_s3_request *request,
        const struct aws_byte_cursor *reply_token_value);
    
    /**
     * Calculate and add RDMA checksum header if needed
     * @param handler Request handler instance
     * @param meta_request Meta request context
     * @param request Individual request
     * @param buffer Buffer to calculate checksum for
     * @param buffer_size Size of the buffer
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*calculate_rdma_checksum_and_add_header)(
        struct aws_s3_rdma_request_handler *handler,
        struct aws_s3_meta_request *meta_request,
        struct aws_s3_request *request,
        const void *buffer,
        size_t buffer_size);
    
    /**
     * Prepare request for RDMA (determines eligibility, registers buffers, processes tokens)
     * This consolidates RDMA request preparation logic that was scattered across meta_request files.
     * @param handler Request handler instance
     * @param meta_request Meta request context
     * @param request Individual request
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*prepare_request)(
        struct aws_s3_rdma_request_handler *handler,
        struct aws_s3_meta_request *meta_request,
        struct aws_s3_request *request);
    
    /**
     * Process incoming response headers for RDMA-specific headers (reply tokens, bytes transferred)
     * @param handler Request handler instance
     * @param meta_request Meta request context
     * @param request Individual request
     * @param headers Array of response headers
     * @param headers_count Number of headers
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*process_response_headers)(
        struct aws_s3_rdma_request_handler *handler,
        struct aws_s3_meta_request *meta_request,
        struct aws_s3_request *request,
        const struct aws_http_header *headers,
        size_t headers_count);
    
    /**
     * Validate content size for initial GET requests (handles RDMA vs non-RDMA differences)
     * @param handler Request handler instance
     * @param meta_request Meta request context
     * @param request Individual request
     * @return AWS_OP_SUCCESS if validation passes, error code otherwise
     */
    int (*validate_content_size)(
        struct aws_s3_rdma_request_handler *handler,
        struct aws_s3_meta_request *meta_request,
        struct aws_s3_request *request);
};

/**
 * @brief RDMA buffer manager vtable interface
 * 
 * This vtable abstracts RDMA buffer management logic including registration,
 * state tracking, and cleanup operations.
 */
struct aws_s3_rdma_buffer_manager_vtable {
    /* Manager name for logging/debugging */
    const char *manager_name;
    
    /**
     * Initialize the buffer manager
     * @param allocator Memory allocator
     * @param rdma_provider Associated RDMA provider
     * @param out_manager Output buffer manager instance
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*init)(
        struct aws_allocator *allocator,
        struct aws_s3_rdma_provider *rdma_provider,
        struct aws_s3_rdma_buffer_manager **out_manager);
    
    /**
     * Cleanup the buffer manager
     * @param manager Buffer manager instance to cleanup
     */
    void (*cleanup)(struct aws_s3_rdma_buffer_manager *manager);
    
    /**
     * Prepare buffer for RDMA operations
     * @param manager Buffer manager instance
     * @param request Request to prepare buffer for
     * @param buffer Buffer pointer
     * @param buffer_size Size of the buffer
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*prepare_buffer_for_rdma)(
        struct aws_s3_rdma_buffer_manager *manager,
        struct aws_s3_request *request,
        void *buffer,
        size_t buffer_size);
    
    /**
     * Finalize buffer after RDMA operations
     * @param manager Buffer manager instance
     * @param request Request to finalize buffer for
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*finalize_buffer)(
        struct aws_s3_rdma_buffer_manager *manager,
        struct aws_s3_request *request);
    
    /**
     * Check if buffer is ready for RDMA
     * @param manager Buffer manager instance
     * @param request Request to check
     * @return true if buffer is RDMA ready, false otherwise
     */
    bool (*is_buffer_rdma_ready)(
        struct aws_s3_rdma_buffer_manager *manager,
        struct aws_s3_request *request);
    
    /**
     * Handle RDMA error for request buffer
     * @param manager Buffer manager instance
     * @param request Request that encountered RDMA error
     * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
     */
    int (*handle_rdma_error)(
        struct aws_s3_rdma_buffer_manager *manager,
        struct aws_s3_request *request);
};

/**
 * @brief Standard plugin entry point function signature
 * 
 * All RDMA provider plugins must export this function:
 * const struct aws_s3_rdma_provider_vtable *aws_s3_rdma_provider_get_vtable(void);
 */
typedef const struct aws_s3_rdma_provider_vtable *aws_s3_rdma_provider_get_vtable_fn(void);

/**
 * Create RDMA provider from plugin
 * @param allocator Memory allocator
 * @param config RDMA provider configuration
 * @param out_provider Output provider instance
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_provider_new_from_plugin(
    struct aws_allocator *allocator,
    const struct aws_s3_rdma_provider_config *config,
    struct aws_s3_rdma_provider **out_provider);

/**
 * Cleanup RDMA provider
 * @param provider Provider instance to cleanup
 */
AWS_S3_API
void aws_s3_rdma_provider_release(struct aws_s3_rdma_provider *provider);

/**
 * Check if memory is suitable for RDMA operations
 * @param provider Provider instance (can be NULL)
 * @param ptr Memory pointer
 * @param size Memory size
 * @return true if suitable for RDMA, false otherwise
 */
AWS_S3_API
bool aws_s3_rdma_provider_is_memory_suitable(
    struct aws_s3_rdma_provider *provider,
    const void *ptr,
    size_t size);

/**
 * Register memory for RDMA operations
 * @param provider Provider instance
 * @param ptr Memory pointer
 * @param size Memory size
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_provider_register_memory(
    struct aws_s3_rdma_provider *provider,
    void *ptr,
    size_t size);

/**
 * Deregister memory from RDMA operations
 * @param provider Provider instance
 * @param ptr Memory pointer
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_provider_deregister_memory(
    struct aws_s3_rdma_provider *provider,
    void *ptr);

AWS_S3_API
int aws_s3_rdma_provider_pre_register_memory(
    struct aws_s3_rdma_provider *provider,
    void *base,
    size_t size);

AWS_S3_API
int aws_s3_rdma_provider_release_memory(
    struct aws_s3_rdma_provider *provider,
    void *base);

AWS_S3_API
bool aws_s3_rdma_provider_is_slice_pre_registered(
    struct aws_s3_rdma_provider *provider,
    const void *ptr,
    size_t size);

/**
 * Prepare RDMA token for PUT operation
 * @param provider Provider instance
 * @param s3_key S3 object key
 * @param buffer Memory buffer
 * @param size Transfer size
 * @param offset Transfer offset
 * @param out_rdma_token Output RDMA token
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_provider_prepare_put_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *s3_key,
    const void *buffer,
    size_t size,
    size_t offset,
    struct aws_byte_cursor *out_rdma_token);

/**
 * Prepare RDMA token for GET operation
 * @param provider Provider instance
 * @param s3_key S3 object key
 * @param buffer Memory buffer
 * @param size Transfer size
 * @param offset Transfer offset
 * @param out_rdma_token Output RDMA token
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_provider_prepare_get_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *s3_key,
    void *buffer,
    size_t size,
    size_t offset,
    struct aws_byte_cursor *out_rdma_token);

/**
 * Process RDMA reply token from server
 * @param provider Provider instance
 * @param rdma_reply_token Token from x-rdma-reply header
 * @param user_data User context
 * @param completion_callback Completion callback
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_provider_process_reply_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *rdma_reply_token,
    void *user_data,
    aws_s3_rdma_completion_fn *completion_callback);

/**
 * Get RDMA token header name from provider
 * @param provider Provider instance
 * @return Byte cursor pointing to the header name string (e.g., "x-amz-rdma-token")
 */
AWS_S3_API
struct aws_byte_cursor aws_s3_rdma_provider_get_rdma_token_header_name(
    struct aws_s3_rdma_provider *provider);

/**
 * Get RDMA reply header name from provider
 * @param provider Provider instance
 * @return Byte cursor pointing to the header name string (e.g., "x-amz-rdma-reply")
 */
AWS_S3_API
struct aws_byte_cursor aws_s3_rdma_provider_get_rdma_reply_header_name(
    struct aws_s3_rdma_provider *provider);

/**
 * Get RDMA bytes header name from provider
 * @param provider Provider instance
 * @return Byte cursor pointing to the header name string (e.g., "x-amz-rdma-bytes")
 */
AWS_S3_API
struct aws_byte_cursor aws_s3_rdma_provider_get_rdma_bytes_header_name(
    struct aws_s3_rdma_provider *provider);

/**
 * Create RDMA request handler
 * @param allocator Memory allocator
 * @param rdma_provider Associated RDMA provider
 * @param out_handler Output request handler instance
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_request_handler_new(
    struct aws_allocator *allocator,
    struct aws_s3_rdma_provider *rdma_provider,
    struct aws_s3_rdma_request_handler **out_handler);

/**
 * Release RDMA request handler
 * @param handler Request handler instance to release
 */
AWS_S3_API
void aws_s3_rdma_request_handler_release(struct aws_s3_rdma_request_handler *handler);

/**
 * Process RDMA token for outgoing requests
 * @param handler Request handler instance  
 * @param meta_request Meta request context
 * @param request Individual request
 * @param request_buffer Buffer for RDMA operations
 * @param buffer_size Size of the buffer
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_request_handler_process_request_token(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    void *request_buffer,
    size_t buffer_size);

/**
 * Calculate and add RDMA checksum header if needed
 * @param handler Request handler instance
 * @param meta_request Meta request context
 * @param request Individual request
 * @param buffer Buffer to calculate checksum for
 * @param buffer_size Size of the buffer
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_request_handler_calculate_rdma_checksum_and_add_header(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    const void *buffer,
    size_t buffer_size);

/**
 * Prepare request for RDMA operations
 * Determines eligibility, extracts buffer info, registers buffers, and processes tokens.
 * Consolidates RDMA preparation logic scattered across meta_request files.
 * @param handler Request handler instance
 * @param meta_request Meta request context
 * @param request Individual request
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_request_handler_prepare_request(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request);

/**
 * Process incoming response headers for RDMA-specific headers
 * Handles reply tokens, bytes transferred headers, and updates response metadata.
 * @param handler Request handler instance
 * @param meta_request Meta request context
 * @param request Individual request
 * @param headers Array of response headers
 * @param headers_count Number of headers in array
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_request_handler_process_response_headers(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    const struct aws_http_header *headers,
    size_t headers_count);

/**
 * Validate content size for initial GET requests
 * Handles differences between RDMA (uses Content-Range) and non-RDMA (uses Content-Length).
 * @param handler Request handler instance
 * @param meta_request Meta request context
 * @param request Individual request
 * @return AWS_OP_SUCCESS if validation passes, error code otherwise
 */
AWS_S3_API
int aws_s3_rdma_request_handler_validate_content_size(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request);

/**
 * Create RDMA buffer manager
 * @param allocator Memory allocator
 * @param rdma_provider Associated RDMA provider
 * @param out_manager Output buffer manager instance
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_buffer_manager_new(
    struct aws_allocator *allocator,
    struct aws_s3_rdma_provider *rdma_provider,
    struct aws_s3_rdma_buffer_manager **out_manager);

/**
 * Release RDMA buffer manager
 * @param manager Buffer manager instance to release
 */
AWS_S3_API
void aws_s3_rdma_buffer_manager_release(struct aws_s3_rdma_buffer_manager *manager);

/**
 * Prepare buffer for RDMA operations
 * @param manager Buffer manager instance
 * @param request Request to prepare buffer for
 * @param buffer Buffer pointer
 * @param buffer_size Size of the buffer
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_buffer_manager_prepare_buffer_for_rdma(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request,
    void *buffer,
    size_t buffer_size);

/**
 * Finalize buffer after RDMA operations
 * @param manager Buffer manager instance
 * @param request Request to finalize buffer for
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_buffer_manager_finalize_buffer(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request);

/**
 * Check if buffer is ready for RDMA
 * @param manager Buffer manager instance
 * @param request Request to check
 * @return true if buffer is RDMA ready, false otherwise
 */
AWS_S3_API
bool aws_s3_rdma_buffer_manager_is_buffer_rdma_ready(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request);

/**
 * Handle RDMA error for request buffer
 * @param manager Buffer manager instance
 * @param request Request that encountered RDMA error
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
AWS_S3_API
int aws_s3_rdma_buffer_manager_handle_rdma_error(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request);

/**
 * Initialize RDMA provider library
 * @param allocator Memory allocator
 */
AWS_S3_API
void aws_s3_rdma_provider_library_init(struct aws_allocator *allocator);

/**
 * Clean up RDMA provider library
 */
AWS_S3_API
void aws_s3_rdma_provider_library_clean_up(void);

AWS_EXTERN_C_END

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_S3_RDMA_PROVIDER_H */
