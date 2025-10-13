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

#include <aws/s3/s3_rdma_provider.h>
#include <aws/s3/private/s3_rdma_provider_impl.h>
#include <aws/s3/s3.h>
#include <aws/s3/private/s3_client_impl.h>
#include <aws/s3/private/s3_meta_request_impl.h>
#include <aws/s3/private/s3_auto_ranged_get.h>
#include <aws/s3/private/s3_request.h>
#include <aws/s3/private/s3_util.h>
#include <aws/common/allocator.h>
#include <aws/common/logging.h>
#include <aws/common/string.h>
#include <aws/common/byte_buf.h>
#include <aws/common/ref_count.h>
#include <aws/common/encoding.h>
#include <aws/checksums/checksums.h>
#include <aws/checksums/crc.h>
#include <aws/cal/hash.h>
#include <aws/http/request_response.h>
#include <aws/io/stream.h>

/**
 * Internal structure to hold RDMA buffer information during request processing
 */
struct rdma_buffer_info {
    void *buffer;                  /* Buffer pointer */
    size_t size;                   /* Buffer size */
    bool is_eligible;              /* Whether buffer is eligible for RDMA */
    bool is_pre_registered;        /* Whether buffer was already registered */
};

struct aws_s3_rdma_request_handler {
    struct aws_allocator *allocator;
    struct aws_ref_count ref_count;

    /* Associated RDMA provider */
    struct aws_s3_rdma_provider *rdma_provider;

    /* Vtable for request handling operations */
    const struct aws_s3_rdma_request_handler_vtable *vtable;
};

static void s_aws_s3_rdma_request_handler_destroy(void *user_data) {
    struct aws_s3_rdma_request_handler *handler = user_data;

    if (!handler) {
        return;
    }

    AWS_LOGF_DEBUG(AWS_LS_S3_RDMA, "id=%p: Destroying RDMA request handler", (void *)handler);

    /* Release RDMA provider reference */
    if (handler->rdma_provider) {
        aws_s3_rdma_provider_release(handler->rdma_provider);
        handler->rdma_provider = NULL;
    }

    aws_mem_release(handler->allocator, handler);
}

/**
 * Prepare HTTP headers for RDMA PUT operation.
 * Removes body-related headers since data is transferred out-of-band via RDMA.
 */
static void s_prepare_rdma_put_headers(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    struct aws_http_headers *headers) {
    
    (void)meta_request;  /* Unused parameter */
    
    /* For RDMA PUT operations: Set Content-Length to 0 */
    struct aws_byte_cursor content_length_header = aws_byte_cursor_from_c_str("Content-Length");
    struct aws_byte_cursor zero_content_length = aws_byte_cursor_from_c_str("0");
    /* No Content-Length header - add it with value 0 for RDMA PUT */
    aws_http_headers_set(headers, content_length_header, zero_content_length);

    /* Remove HTTP body-related headers added for chunked encoding.
     * IMPORTANT: Preserve user-specified Content-Encoding (e.g., gzip, deflate) as it's object metadata.
     * Only remove aws-chunked which is added for trailing checksums. */
    struct aws_byte_cursor transfer_encoding_header = aws_byte_cursor_from_c_str("Transfer-Encoding");
    aws_http_headers_erase(headers, transfer_encoding_header);

    struct aws_byte_cursor content_encoding_header_name = aws_byte_cursor_from_c_str("Content-Encoding");
    struct aws_byte_cursor content_encoding_value;
    if (aws_http_headers_get(headers, content_encoding_header_name, &content_encoding_value) == AWS_OP_SUCCESS) {
        AWS_LOGF_DEBUG(
            AWS_LS_S3_META_REQUEST,
            "id=%p RDMA PUT: Processing Content-Encoding header: " PRInSTR,
            (void *)meta_request,
            AWS_BYTE_CURSOR_PRI(content_encoding_value));
        /* Check if it contains "aws-chunked" - if so, we need to remove it while preserving user encodings */
        struct aws_byte_cursor aws_chunked = aws_byte_cursor_from_c_str("aws-chunked");
        
        if (aws_byte_cursor_eq_ignore_case(&content_encoding_value, &aws_chunked)) {
            /* Exactly "aws-chunked" with no other encodings - remove entire header */
            aws_http_headers_erase(headers, content_encoding_header_name);
        } else if (aws_byte_cursor_find_exact(&content_encoding_value, &aws_chunked, NULL) == AWS_OP_SUCCESS) {
            /* Contains aws-chunked along with other encodings (e.g., "gzip,aws-chunked").
             * Strip out "aws-chunked" and comma, keeping user's encoding. */
            struct aws_byte_cursor search_comma_aws_chunked = aws_byte_cursor_from_c_str(",aws-chunked");
            struct aws_byte_cursor search_aws_chunked_comma = aws_byte_cursor_from_c_str("aws-chunked,");
            
            if (aws_byte_cursor_find_exact(&content_encoding_value, &search_comma_aws_chunked, NULL) == AWS_OP_SUCCESS) {
                /* Pattern: "user-encoding,aws-chunked" - remove ",aws-chunked" suffix */
                size_t user_encoding_len = content_encoding_value.len - search_comma_aws_chunked.len;
                struct aws_byte_cursor user_encoding = {
                    .ptr = content_encoding_value.ptr,
                    .len = user_encoding_len
                };
                aws_http_headers_set(headers, content_encoding_header_name, user_encoding);
            } else if (aws_byte_cursor_find_exact(&content_encoding_value, &search_aws_chunked_comma, NULL) == AWS_OP_SUCCESS) {
                /* Pattern: "aws-chunked,user-encoding" - remove "aws-chunked," prefix */
                struct aws_byte_cursor user_encoding = {
                    .ptr = content_encoding_value.ptr + search_aws_chunked_comma.len,
                    .len = content_encoding_value.len - search_aws_chunked_comma.len
                };
                aws_http_headers_set(headers, content_encoding_header_name, user_encoding);
            }
            /* else: Malformed or embedded - keep as-is to be safe */
        }
        /* else: No aws-chunked - preserve user's Content-Encoding (gzip, deflate, etc.) as-is */
    }
    struct aws_byte_cursor decoded_content_length_header = aws_byte_cursor_from_c_str("x-amz-decoded-content-length");
    aws_http_headers_erase(headers, decoded_content_length_header);

    struct aws_byte_cursor content_sha256_header = aws_byte_cursor_from_c_str("x-amz-content-sha256");
    aws_http_headers_erase(headers, content_sha256_header);

    /* Clear the flag that would cause STREAMING-UNSIGNED-PAYLOAD-TRAILER to be added after signing.
     * RDMA requests don't use chunked encoding or streaming payloads since data is transferred out-of-band. */
    request->send_data.require_streaming_unsigned_payload_header = 0;
}

/**
 * Prepare HTTP headers for RDMA GET operation.
 * Sets Content-Length to 0 and removes transfer encoding.
 */
static void s_prepare_rdma_get_headers(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    struct aws_http_headers *headers) {

    (void)meta_request;  /* Unused parameter */
    (void)request;       /* Unused parameter */

    /* For GET requests, set Content-Length to 0 and remove Transfer-Encoding */
    struct aws_byte_cursor content_length_header = aws_byte_cursor_from_c_str("Content-Length");
    struct aws_byte_cursor zero_content_length = aws_byte_cursor_from_c_str("0");
    struct aws_byte_cursor transfer_encoding_header = aws_byte_cursor_from_c_str("Transfer-Encoding");

    aws_http_headers_set(headers, content_length_header, zero_content_length);
    aws_http_headers_erase(headers, transfer_encoding_header);
}

/**
 * Extract RDMA buffer information for GET requests.
 */
static void s_extract_rdma_buffer_info_for_get(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    struct aws_s3_client *client,
    struct rdma_buffer_info *buffer_info) {

    (void)meta_request;  /* Currently unused but kept for API consistency */
    AWS_PRECONDITION(request);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(buffer_info);

    AWS_ZERO_STRUCT(*buffer_info);

    /* For GET requests, check if we have a suitable receive buffer */
    if (request->send_data.response_body.capacity > 0) {
        buffer_info->buffer = request->send_data.response_body.buffer;
        buffer_info->size = request->send_data.response_body.capacity;
        buffer_info->is_eligible = (buffer_info->size >= client->rdma_min_transfer_size);
    }
}

/**
 * Extract RDMA buffer information for PUT requests.
 */
static void s_extract_rdma_buffer_info_for_put(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    struct aws_s3_client *client,
    struct rdma_buffer_info *buffer_info) {

    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(buffer_info);

    AWS_ZERO_STRUCT(*buffer_info);

    /* If caller explicitly enabled RDMA and provided a request buffer, use the part-specific slice */
    if (meta_request->user_buffer_options.transfer_buffer &&
        meta_request->user_buffer_options.transfer_buffer_size > 0 &&
        request->is_user_provided_buffer) {
        /* For multipart: request_body.buffer is already offset to the part's slice */
        buffer_info->buffer = request->request_body.buffer;
        
        /* By this point, len is always correct (set when buffer was mapped in s3_auto_ranged_put.c) */
        buffer_info->size = request->request_body.len;
        
        /* Only mark as eligible if size > 0 and meets threshold */
        buffer_info->is_eligible = (buffer_info->size > 0 && buffer_info->size >= client->rdma_min_transfer_size);
        /* User-provided buffers are NOT pre-registered - they need to be registered now */
        buffer_info->is_pre_registered = false;
        
        AWS_LOGF_DEBUG(
            AWS_LS_S3_META_REQUEST,
            "id=%p RDMA user-buffer check: eligible=%d, size=%zu, part=%u",
            (void *)meta_request,
            buffer_info->is_eligible,
            buffer_info->size,
            request->part_number);
        return;
    }

    /* RDMA OPTIMIZATION: Check if buffer was pre-registered during allocation */
    if (client->rdma_buffer_manager &&
        aws_s3_rdma_buffer_manager_is_buffer_rdma_ready(client->rdma_buffer_manager, request)) {
        AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST,
                      "id=%p RDMA: Upload part buffer already registered",
                      (void *)meta_request);

        /* Use the registered buffer information.
         * Note: Use len, not capacity. Even if the full capacity is pre-registered,
         * we only want to transfer the actual data size (len). */
        if (request->request_body.buffer && request->request_body.len > 0) {
            buffer_info->buffer = request->request_body.buffer;
            buffer_info->size = request->request_body.len;
            buffer_info->is_eligible = (buffer_info->size >= client->rdma_min_transfer_size);
            buffer_info->is_pre_registered = true;
            AWS_LOGF_TRACE(AWS_LS_S3_META_REQUEST,
                           "id=%p RDMA: Using pre-registered buffer: %p, size=%zu (len)",
                           (void *)meta_request, buffer_info->buffer, buffer_info->size);
        } else {
            AWS_LOGF_WARN(AWS_LS_S3_META_REQUEST,
                          "id=%p RDMA: Buffer marked as registered but buffer is null or len is 0",
                          (void *)meta_request);
        }
        return;
    }

    /* Fall back to body stream check */
    if (request->send_data.message && aws_http_message_get_body_stream(request->send_data.message)) {
        struct aws_input_stream *body_stream = aws_http_message_get_body_stream(request->send_data.message);

        int64_t stream_length;
        if (aws_input_stream_get_length(body_stream, &stream_length) == AWS_OP_SUCCESS && stream_length >= 0) {
            buffer_info->size = (size_t)stream_length;

            /* Try to extract buffer pointer from cursor-based stream (BytesIO) */
            if (body_stream->impl) {
                /* Define the cursor stream structure locally to match aws-c-io/source/stream.c */
                struct aws_input_stream_byte_cursor_impl {
                    struct aws_input_stream base;
                    struct aws_allocator *allocator;
                    struct aws_byte_cursor original_cursor;
                    struct aws_byte_cursor current_cursor;
                };

                /* Cast to cursor impl to access the buffer */
                struct aws_input_stream_byte_cursor_impl *cursor_impl =
                    (struct aws_input_stream_byte_cursor_impl *)body_stream;

                /* Verify this looks like a cursor stream by checking if original_cursor makes sense */
                if (cursor_impl->original_cursor.ptr &&
                    cursor_impl->original_cursor.len == buffer_info->size) {
                    buffer_info->buffer = (void *)cursor_impl->original_cursor.ptr;
                    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST,
                                  "id=%p Extracted buffer from cursor stream: size=%zu",
                                  (void *)meta_request, buffer_info->size);
                } else {
                    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST,
                                  "id=%p Buffer extraction failed (len=%zu, expected=%zu)",
                                  (void *)meta_request,
                                  cursor_impl->original_cursor.len,
                                  buffer_info->size);
                }
            }

            buffer_info->is_eligible = (buffer_info->buffer && buffer_info->size >= client->rdma_min_transfer_size);
        } else {
            AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p PUT stream length unknown", (void *)meta_request);
        }
        return;
    }

    /* Check if request body is already available (async writes, file uploads after read) */
    if (request->request_body.buffer && request->request_body.len > 0) {
        buffer_info->buffer = request->request_body.buffer;
        
        /* By this point, len is always correct:
         * - Async writes: len was set when buffer was provided
         * - File uploads: len was set after file read completed
         * - User buffers: len was set when buffer was mapped
         * - Empty files: len = 0 (this branch not taken) */
        buffer_info->size = request->request_body.len;
        
        buffer_info->is_eligible = (buffer_info->size >= client->rdma_min_transfer_size);
        AWS_LOGF_DEBUG(
            AWS_LS_S3_META_REQUEST, 
            "id=%p RDMA buffer check: eligible=%d, size=%zu, threshold=%zu",
            (void *)meta_request,
            buffer_info->is_eligible,
            buffer_info->size,
            client->rdma_min_transfer_size);
        return;
    }

    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p PUT request has no body", (void *)meta_request);
}

/**
 * Determine RDMA eligibility and extract buffer information for a request.
 */
static void s_determine_rdma_eligibility_and_buffer_info(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    struct aws_s3_client *client,
    struct rdma_buffer_info *buffer_info) {

    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(buffer_info);

    AWS_ZERO_STRUCT(*buffer_info);

    switch (request->request_type) {
        case AWS_S3_REQUEST_TYPE_GET_OBJECT:
            s_extract_rdma_buffer_info_for_get(meta_request, request, client, buffer_info);
            break;

        case AWS_S3_REQUEST_TYPE_PUT_OBJECT:
        case AWS_S3_REQUEST_TYPE_UPLOAD_PART:
            s_extract_rdma_buffer_info_for_put(meta_request, request, client, buffer_info);
            break;

        default:
            AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Request type %d not suitable for RDMA",
                         (void *)meta_request, request->request_type);
            if (request->request_type != AWS_S3_REQUEST_TYPE_GET_OBJECT) {
                AWS_ASSERT(request->request_body.buffer && request->request_body.len > 0);
            }
            break;
    }
    
    /* Log buffer eligibility results */
    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p RDMA eligibility: type=%d, part=%u, eligible=%d, size=%zu, threshold=%zu",
        (void *)meta_request,
        request->request_type,
        request->part_number,
        buffer_info->is_eligible,
        buffer_info->size,
        client->rdma_min_transfer_size);
}

/**
 * Register buffer with RDMA provider and process RDMA tokens.
 */
static void s_register_and_process_rdma_tokens(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    struct aws_s3_client *client,
    const struct rdma_buffer_info *buffer_info) {

    AWS_PRECONDITION(handler);
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(buffer_info);
    AWS_PRECONDITION(buffer_info->buffer);

    AWS_LOGF_TRACE(AWS_LS_S3_META_REQUEST, "id=%p Request is RDMA eligible - proceeding with RDMA setup",
                   (void *)meta_request);

    /* RDMA OPTIMIZATION: Skip registration if buffer was already registered during allocation */
    int register_result = AWS_OP_SUCCESS;
    if (client->rdma_buffer_manager && !buffer_info->is_pre_registered) {
        register_result = aws_s3_rdma_buffer_manager_prepare_buffer_for_rdma(
            client->rdma_buffer_manager, request, buffer_info->buffer, buffer_info->size);

        if (register_result != AWS_OP_SUCCESS) {
            AWS_LOGF_WARN(
                AWS_LS_S3_META_REQUEST,
                "id=%p Failed to register memory with RDMA provider for request %p (result=%d)",
                (void *)meta_request,
                (void *)request,
                register_result);
            return;
        }
    }

    if (register_result == AWS_OP_SUCCESS) {
        AWS_LOGF_TRACE(
            AWS_LS_S3_META_REQUEST,
            "id=%p Successfully registered memory with RDMA provider for request %p (buffer=%p, size=%zu)",
            (void *)meta_request,
            (void *)request,
            buffer_info->buffer,
            buffer_info->size);

        /* Process RDMA token for request using vtable method */
        if (handler->vtable && handler->vtable->process_request_token) {
            handler->vtable->process_request_token(
                handler, meta_request, request, buffer_info->buffer, buffer_info->size);

            /* Calculate and add RDMA checksum headers if needed */
            if (handler->vtable->calculate_rdma_checksum_and_add_header) {
                handler->vtable->calculate_rdma_checksum_and_add_header(
                    handler, meta_request, request, buffer_info->buffer, buffer_info->size);
            }
        }
    }
}

/* Default implementation for request token processing */
static int s_default_process_request_token(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    void *request_buffer,
    size_t buffer_size) {

    AWS_PRECONDITION(handler);
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);

    (void)handler; /* Unused parameter */

    struct aws_s3_client *client = meta_request->client;

    /* Check if RDMA is enabled and available */
    if (!client->enable_rdma || !client->rdma_provider) {
        AWS_LOGF_TRACE(AWS_LS_S3_META_REQUEST, "id=%p RDMA not available", (void *)meta_request);
        return AWS_OP_SUCCESS;
    }

    /* Skip RDMA token generation for NOOP requests */
    if (request->is_noop) {
        AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Skipping RDMA for NOOP request (part %u)",
                     (void *)meta_request, request->part_number);
        return AWS_OP_SUCCESS;
    }

    /* Check if request is eligible for RDMA acceleration */
    bool is_rdma_eligible = false;
    if (request_buffer && buffer_size >= client->rdma_min_transfer_size) {
        is_rdma_eligible = true;
    }

    if (!is_rdma_eligible) {
        AWS_LOGF_DEBUG(
            AWS_LS_S3_META_REQUEST,
            "id=%p Request not eligible for RDMA (size=%zu < threshold=%zu)",
            (void *)meta_request,
            buffer_size,
            client->rdma_min_transfer_size);
        return AWS_OP_SUCCESS;
    }

    /* Generate RDMA token */
    int rdma_result = -1;
    struct aws_byte_cursor rdma_token_cursor;
    AWS_ZERO_STRUCT(rdma_token_cursor);

    /* Build object key cursor from the HTTP path: /bucket/key -> use substring after first '/'
     * Strip any query string so provider sees bare key. */
    struct aws_byte_cursor path_cursor;
    aws_http_message_get_request_path(request->send_data.message, &path_cursor);
    struct aws_byte_cursor s3_key = path_cursor;
    if (s3_key.len > 0 && s3_key.ptr[0] == '/') {
        s3_key.ptr += 1;
        s3_key.len -= 1;
    }

    /* Calculate effective size for both RDMA token and checksum calculation */
    size_t effective_size = buffer_size;
    /* For PUT: use actual data length if available (len > 0), otherwise use buffer_size (capacity) */
    if (request->request_type != AWS_S3_REQUEST_TYPE_GET_OBJECT && 
        request->request_body.len > 0 && 
        request->request_body.len < effective_size) {
        effective_size = request->request_body.len;  // Use actual payload size for PUT operations
    }

    AWS_LOGF_TRACE(
        AWS_LS_S3_META_REQUEST,
        "id=%p RDMA token key: %.*s (request_type=%d, part_number=%u, buffer=%p, effective_size=%zu)",
        (void *)meta_request,
        (int)s3_key.len,
        (const char *)s3_key.ptr,
        request->request_type,
        request->part_number,
        request_buffer,
        effective_size);

    /* Generate appropriate RDMA token based on request type */
    if (request->request_type == AWS_S3_REQUEST_TYPE_GET_OBJECT) {
        AWS_LOGF_TRACE(AWS_LS_S3_META_REQUEST, "id=%p Generating GET RDMA token", (void *)meta_request);
        rdma_result = aws_s3_rdma_provider_prepare_get_token(
            client->rdma_provider, &s3_key, request_buffer, effective_size, 0, &rdma_token_cursor);
    } else {
        AWS_LOGF_TRACE(AWS_LS_S3_META_REQUEST, "id=%p Generating PUT RDMA token (size=%zu)", (void *)meta_request, effective_size);
        rdma_result = aws_s3_rdma_provider_prepare_put_token(
            client->rdma_provider, &s3_key, request_buffer, effective_size, 0, &rdma_token_cursor);

        AWS_LOGF_DEBUG(
            AWS_LS_S3_META_REQUEST,
            "id=%p RDMA PUT token generation result: %d, token_length: %zu, part_number: %u",
            (void *)meta_request,
            rdma_result,
            rdma_token_cursor.len,
            request->part_number);
    }

    if (rdma_result == 0) {
        /* Add RDMA token header to the request */
        struct aws_http_headers *headers = aws_http_message_get_headers(request->send_data.message);
        if (headers) {
            struct aws_byte_cursor header_name = aws_s3_rdma_provider_get_rdma_token_header_name(client->rdma_provider);

            /* Use the RDMA token directly (plugin now generates correct length) */
            struct aws_byte_cursor header_value = rdma_token_cursor;

            /* Validate that we have a non-empty token */
            if (header_value.len == 0) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p RDMA provider returned empty token",
                    (void *)meta_request);
                if (request->rdma_buffer_registered) {
                    aws_s3_rdma_provider_deregister_memory(client->rdma_provider, request_buffer);
                    request->rdma_buffer_registered = 0;
                }
                return aws_raise_error(AWS_ERROR_INVALID_STATE);
            }

            /* Log the exact token we are about to send for server correlation */
            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "id=%p Adding RDMA token header (len=%zu)",
                (void *)meta_request,
                header_value.len);

            if (aws_http_headers_set(headers, header_name, header_value) ==
                AWS_OP_SUCCESS) {
                /* Successfully added RDMA token header */
                AWS_LOGF_DEBUG(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Successfully added RDMA token header (len=%zu, part=%u)",
                    (void *)meta_request,
                    header_value.len,
                    request->part_number);

                /* Handle headers for RDMA operations - follow documented workflow */
                if (request->request_type != AWS_S3_REQUEST_TYPE_GET_OBJECT) {
                    s_prepare_rdma_put_headers(meta_request, request, headers);
                } else {
                    s_prepare_rdma_get_headers(meta_request, request, headers);
                }
                AWS_LOGF_DEBUG(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p RDMA token generated successfully, keeping request_body intact for retry capability",
                    (void *)meta_request);
            } else {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Failed to add RDMA token header",
                    (void *)meta_request);
                if (request->rdma_buffer_registered) {
                    aws_s3_rdma_provider_deregister_memory(client->rdma_provider, request_buffer);
                    request->rdma_buffer_registered = 0;
                }
                return aws_raise_error(AWS_ERROR_INVALID_STATE);
            }
        }
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p RDMA token generation failed (result=%d), will retry with HTTP",
            (void *)meta_request,
            rdma_result);

        /* Clean up any registered buffer on RDMA token generation failure */
        if (request->rdma_buffer_registered && request_buffer) {
            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "id=%p Deregistering buffer due to RDMA token generation failure",
                (void *)meta_request);
            aws_s3_rdma_provider_deregister_memory(client->rdma_provider, request_buffer);
            request->rdma_buffer_registered = 0;
        }

        /* Disable RDMA for retry - next attempt will use regular HTTP with body */
        request->disable_rdma_on_retry = 1;
        
        /* Return error to trigger retry. The retry will skip RDMA and use normal HTTP path with body. */
        return aws_raise_error(AWS_ERROR_S3_INTERNAL_ERROR);
    }

    return AWS_OP_SUCCESS;
}

/* Default implementation for reply token processing */
static int s_default_process_reply_token(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    const struct aws_byte_cursor *reply_token_value) {

    AWS_PRECONDITION(handler);
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);
    AWS_PRECONDITION(reply_token_value);

    (void)handler; /* Unused parameter */

    struct aws_s3_client *client = meta_request->client;

    /* Check if RDMA is enabled and available */
    if (!client->enable_rdma || !client->rdma_provider) {
        AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p RDMA not enabled, skipping reply token processing", (void *)meta_request);
        return AWS_OP_SUCCESS;
    }

    /* Extract reply token */
    char reply_token[256];
    size_t token_len = aws_min_size(reply_token_value->len, sizeof(reply_token) - 1);
    memcpy(reply_token, reply_token_value->ptr, token_len);
    reply_token[token_len] = '\0';

    /* Get the buffer information for processing the reply */
    void* response_buffer = NULL;

    if (request->request_type == AWS_S3_REQUEST_TYPE_GET_OBJECT &&
        request->send_data.response_body.capacity > 0) {
        response_buffer = request->send_data.response_body.buffer;
    } else if (request->request_type == AWS_S3_REQUEST_TYPE_PUT_OBJECT ||
               request->request_type == AWS_S3_REQUEST_TYPE_UPLOAD_PART) {
        /* For PUT operations, use request_body.buffer directly (no longer cleared for RDMA) */
        if (request->request_body.len > 0) {
            response_buffer = request->request_body.buffer;
        }
    }

    if (!response_buffer) {
        AWS_LOGF_WARN(
            AWS_LS_S3_META_REQUEST,
            "id=%p No response buffer found for RDMA reply token processing (request_type=%d, request_body_buffer=%p, request_body_len=%zu)",
            (void *)meta_request, request->request_type, request->request_body.buffer, request->request_body.len);
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    /* Check for RDMA error codes in the reply token value before processing */
    bool rdma_error_detected = false;
    int rdma_error_code = 0;

    /* Parse numeric error codes from reply token (e.g., "500", "400", etc.) */
    if (reply_token_value->len > 0 && reply_token_value->len <= 3) {
        char *endptr;
        long parsed_code = strtol(reply_token, &endptr, 10);
        if (endptr != reply_token && *endptr == '\0' && parsed_code >= 400) {
            rdma_error_detected = true;
            rdma_error_code = (int)parsed_code;
            AWS_LOGF_ERROR(
                AWS_LS_S3_META_REQUEST,
                "id=%p RDMA error detected in reply token: %d",
                (void *)meta_request,
                rdma_error_code);
        }
    }

    if (rdma_error_detected) {
        /* Handle RDMA-specific errors and disable for retry */
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p RDMA operation failed with code %d, disabling for retry",
            (void *)meta_request,
            rdma_error_code);

        /* Mark request to disable RDMA on retry */
        request->disable_rdma_on_retry = 1;

        /* Clean up registered buffer */
        if (request->rdma_buffer_registered && response_buffer) {
            aws_s3_rdma_provider_deregister_memory(client->rdma_provider, response_buffer);
            AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Deregistered RDMA buffer after error", (void *)meta_request);
            request->rdma_buffer_registered = 0;
        }

        /* Return error to trigger retry with regular HTTP */
        return aws_raise_error(AWS_ERROR_S3_RDMA_INVALID_TOKEN);
    }

    /* Process successful RDMA reply token */
    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p Processing RDMA reply token: %.*s",
        (void *)meta_request,
        (int)reply_token_value->len,
        (const char *)reply_token_value->ptr);

    /* Call provider-specific reply token processing */
    int result = aws_s3_rdma_provider_process_reply_token(
        client->rdma_provider,
        reply_token_value,
        request, /* pass request as user_data */
        NULL     /* no completion callback needed for synchronous processing */
    );

    if (result != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p RDMA provider failed to process reply token",
            (void *)meta_request);
        request->disable_rdma_on_retry = 1;
        return result;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p Successfully processed RDMA reply token",
        (void *)meta_request);

    return AWS_OP_SUCCESS;
}

/* Default implementation for checksum calculation and header addition */
static int s_default_calculate_rdma_checksum_and_add_header(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    const void *buffer,
    size_t buffer_size) {

    AWS_PRECONDITION(handler);
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);

    (void)handler; /* Unused parameter */

    /* Only add checksum for PUT operations */
    if (request->request_type == AWS_S3_REQUEST_TYPE_GET_OBJECT) {
        return AWS_OP_SUCCESS;
    }

    /* Calculate effective size for checksum calculation - use actual request body length */
    size_t effective_size = buffer_size;
    if (request->request_type != AWS_S3_REQUEST_TYPE_GET_OBJECT) {
        effective_size = request->request_body.len;  // Always use actual request body length for PUT operations
    }

    /* Skip checksum for empty buffers (check both buffer_size and effective_size) */
    if (!buffer || buffer_size == 0 || effective_size == 0) {
        AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Skipping checksum for empty buffer (buffer_size=%zu, effective_size=%zu)",
                      (void *)meta_request, buffer_size, effective_size);
        return AWS_OP_SUCCESS;
    }

    /* Add debug logging to track when this function is called */
    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p RDMA checksum calculation called for request type %d, buffer_size=%zu, effective_size=%zu (from request_body.len=%zu)",
                   (void *)meta_request, request->request_type, buffer_size, effective_size, request->request_body.len);

    /* Only process if we're being called from the RDMA flow (buffer and size provided) */
    /* Note: rdma_buffer_registered might not be set yet for multipart uploads */

    struct aws_http_headers *headers = aws_http_message_get_headers(request->send_data.message);
    if (!headers) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    /* Calculate checksum based on the configured algorithm */
    enum aws_s3_checksum_algorithm checksum_algo = meta_request->checksum_config.checksum_algorithm;

    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p RDMA checksum algorithm: %d", (void *)meta_request, checksum_algo);

    if (checksum_algo == AWS_SCA_NONE) {
        /* No checksum required */
        AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p No checksum algorithm configured, skipping", (void *)meta_request);
        return AWS_OP_SUCCESS;
    }

    struct aws_byte_buf encoded_checksum;
    AWS_ZERO_STRUCT(encoded_checksum);

    int result = AWS_OP_ERR;

    /* Calculate checksum using the common checksum interface - use effective size, not buffer size */
    struct aws_byte_cursor input_cursor = aws_byte_cursor_from_array(buffer, effective_size);

    /* Initialize raw checksum buffer using the standard library function (like other code does) */
    struct aws_byte_buf raw_checksum;
    size_t digest_size = aws_get_digest_size_from_checksum_algorithm(checksum_algo);

    if (aws_byte_buf_init(&raw_checksum, meta_request->allocator, digest_size)) {
        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "id=%p Failed to initialize checksum buffer", (void *)meta_request);
        return AWS_OP_ERR;
    }

    /* Calculate raw checksum */
    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Computing checksum for %zu bytes (effective_size, not buffer_size)", (void *)meta_request, effective_size);

    if (aws_checksum_compute(meta_request->allocator, checksum_algo, &input_cursor, &raw_checksum)) {
        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "id=%p Failed to compute checksum", (void *)meta_request);
        aws_byte_buf_clean_up(&raw_checksum);
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Checksum computed successfully, raw length: %zu", (void *)meta_request, raw_checksum.len);

    /* Base64 encode the checksum */
    struct aws_byte_cursor raw_checksum_cursor = aws_byte_cursor_from_buf(&raw_checksum);
    size_t encoded_length = 0;

    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Starting Base64 encoding", (void *)meta_request);

    if (aws_base64_compute_encoded_len(raw_checksum.len, &encoded_length)) {
        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "id=%p Failed to compute Base64 encoded length", (void *)meta_request);
        aws_byte_buf_clean_up(&raw_checksum);
        goto cleanup;
    }

    if (aws_byte_buf_init(&encoded_checksum, meta_request->allocator, encoded_length)) {
        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "id=%p Failed to initialize Base64 buffer", (void *)meta_request);
        aws_byte_buf_clean_up(&raw_checksum);
        goto cleanup;
    }

    if (aws_base64_encode(&raw_checksum_cursor, &encoded_checksum)) {
        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST, "id=%p Failed to Base64 encode checksum", (void *)meta_request);
        aws_byte_buf_clean_up(&raw_checksum);
        goto cleanup;
    }

    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Base64 encoding successful, encoded length: %zu", (void *)meta_request, encoded_checksum.len);

    /* Get appropriate header name for checksum algorithm */
    struct aws_byte_cursor header_name;
    switch (checksum_algo) {
        case AWS_SCA_CRC32:
            header_name = aws_byte_cursor_from_c_str("x-amz-checksum-crc32");
            break;
        case AWS_SCA_CRC32C:
            header_name = aws_byte_cursor_from_c_str("x-amz-checksum-crc32c");
            break;
        case AWS_SCA_SHA1:
            header_name = aws_byte_cursor_from_c_str("x-amz-checksum-sha1");
            break;
        case AWS_SCA_SHA256:
            header_name = aws_byte_cursor_from_c_str("x-amz-checksum-sha256");
            break;
        default:
            aws_byte_buf_clean_up(&raw_checksum);
            result = AWS_OP_SUCCESS; /* Unknown algorithm, skip */
            goto cleanup;
    }

    /* Add checksum header - override any existing checksum header
     * because RDMA checksum is calculated from the actual transfer buffer */
    struct aws_byte_cursor header_value = aws_byte_cursor_from_buf(&encoded_checksum);

    /* Remove existing checksum header first to ensure clean override */
    aws_http_headers_erase(headers, header_name);
    result = aws_http_headers_set(headers, header_name, header_value);

    if (result == AWS_OP_SUCCESS) {
        AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Successfully added RDMA checksum header: %.*s = %.*s",
                       (void *)meta_request,
                       (int)header_name.len, header_name.ptr,
                       (int)header_value.len, header_value.ptr);
    } else {
        AWS_LOGF_WARN(AWS_LS_S3_META_REQUEST, "id=%p Failed to set RDMA checksum header (result=%d)",
                      (void *)meta_request, result);
    }

    aws_byte_buf_clean_up(&raw_checksum);

cleanup:
    aws_byte_buf_clean_up(&encoded_checksum);

    if (result == AWS_OP_SUCCESS) {
        AWS_LOGF_DEBUG(
            AWS_LS_S3_META_REQUEST,
            "id=%p Added RDMA checksum header for algorithm %d",
            (void *)meta_request,
            checksum_algo);
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p Failed to add RDMA checksum header",
            (void *)meta_request);
    }

    return result;
}

/**
 * Process RDMA bytes header from response and update checksum if needed.
 * Returns the number of bytes transferred via RDMA (0 if no RDMA transfer or header not found).
 */
static uint64_t s_process_rdma_bytes_header(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    const struct aws_byte_cursor *header_value) {
    
    /* Parse RDMA bytes header value which can be in two formats:
     * 1. Simple format: "1048576" (just the transferred bytes)
     * 2. Extended format: "1048576/1048576" (transferred/total)
     */
    struct aws_byte_cursor rdma_bytes_cursor = *header_value;
    uint64_t rdma_bytes = 0;
    
    /* Check if the value contains a '/' character */
    struct aws_byte_cursor search_cursor = rdma_bytes_cursor;
    uint8_t *slash_ptr = memchr(search_cursor.ptr, '/', search_cursor.len);
    
    if (slash_ptr != NULL) {
        /* Extended format: "transferred/total" - extract the first number (transferred) */
        size_t transferred_len = slash_ptr - search_cursor.ptr;
        rdma_bytes_cursor.len = transferred_len;
        AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, 
                       "id=%p: RDMA bytes header in extended format, extracting transferred bytes",
                       (void *)meta_request);
    }
    
    /* Parse the transferred bytes value */
    if (aws_byte_cursor_utf8_parse_u64(rdma_bytes_cursor, &rdma_bytes) == AWS_OP_SUCCESS) {
        AWS_LOGF_TRACE(AWS_LS_S3_META_REQUEST, 
                       "id=%p: RDMA bytes header captured - value %llu for request %p (original value: %.*s)",
                       (void *)meta_request, (unsigned long long)rdma_bytes, (void *)request,
                       (int)header_value->len, (const char *)header_value->ptr);
        
        /* For RDMA GET requests: if we have x-rdma-bytes-transferred, server used RDMA.
         * Content-Length will be 0 (no HTTP body). Update checksum now with RDMA data. */
        if (request->request_type == AWS_S3_REQUEST_TYPE_GET_OBJECT &&
            meta_request->checksum_config.validate_response_checksum &&
            request->request_level_running_response_sum &&
            request->send_data.response_body.buffer != NULL) {
            struct aws_byte_cursor rdma_data = {
                .ptr = request->send_data.response_body.buffer,
                .len = (size_t)rdma_bytes
            };
            aws_checksum_update(request->request_level_running_response_sum, &rdma_data);
            AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, 
                           "id=%p: RDMA GET - updated checksum with %llu bytes of RDMA data for request %p, buffer=%p",
                           (void *)meta_request, (unsigned long long)rdma_bytes, (void *)request,
                           request->send_data.response_body.buffer);
        }
    } else {
        AWS_LOGF_WARN(AWS_LS_S3_META_REQUEST,
                      "id=%p: Failed to parse RDMA bytes header value: %.*s",
                      (void *)meta_request, (int)header_value->len, (const char *)header_value->ptr);
    }
    
    return rdma_bytes;
}

/**
 * Update response headers and body length after RDMA transfer.
 */
static void s_update_headers_for_rdma_transfer(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    uint64_t rdma_bytes_transferred) {
    
    (void)meta_request;  /* Unused parameter */
    
    if (rdma_bytes_transferred == 0) {
        return;
    }
    
    if (request->send_data.response_headers) {
        char rdma_bytes_str[32];
        snprintf(rdma_bytes_str, sizeof(rdma_bytes_str), "%llu", (unsigned long long)rdma_bytes_transferred);
        aws_http_headers_set(request->send_data.response_headers,
                            aws_byte_cursor_from_c_str("Content-Length"),
                            aws_byte_cursor_from_c_str(rdma_bytes_str));
    }
    /* Set the response_body.len to the value of RDMA bytes transferred */
    request->send_data.response_body.len = (size_t)rdma_bytes_transferred;
}

/**
 * Validate that object size doesn't exceed part_size for initial GET requests.
 * Handles both RDMA (uses Content-Range) and non-RDMA (uses Content-Length) cases.
 */
static int s_validate_initial_get_object_size(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request) {
    
    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p Validating initial GET object size: request_type=%d, tag=%d, part_size=%zu",
        (void *)meta_request,
        request->request_type,
        request->request_tag,
        meta_request->part_size);
    
    /* For RDMA requests, the server only transfers what fits in the buffer, so Content-Length
     * will reflect the transferred amount, not the actual object size.
     * We need to check the Content-Range header to get the actual object size. */
    if (request->request_type == AWS_S3_REQUEST_TYPE_GET_OBJECT &&
        request->request_tag == AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1) {
        uint64_t range_start = 0, range_end = 0, actual_object_size = 0;
        if (aws_s3_parse_content_range_response_header(
                request->allocator,
                request->send_data.response_headers,
                &range_start,
                &range_end,
                &actual_object_size) == AWS_OP_SUCCESS) {
            
            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "id=%p Content-Range parsed: object_size=%llu, part_size=%zu",
                (void *)meta_request,
                (unsigned long long)actual_object_size,
                meta_request->part_size);
            
            /* Check actual object size (from Content-Range) against part_size */
            if (actual_object_size > meta_request->part_size) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p VALIDATION FAILED: Object size (%llu) > part_size (%zu) - ABORTING REQUEST to retry with range",
                    (void *)meta_request,
                    (unsigned long long)actual_object_size,
                    meta_request->part_size);
                return aws_raise_error(AWS_ERROR_S3_INTERNAL_PART_SIZE_MISMATCH_RETRYING_WITH_RANGE);
            }
            
            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "id=%p Validation passed: object_size (%llu) <= part_size (%zu)",
                (void *)meta_request,
                (unsigned long long)actual_object_size,
                meta_request->part_size);
        } else {
            AWS_LOGF_WARN(
                AWS_LS_S3_META_REQUEST,
                "id=%p Could not parse Content-Range header for GET partNumber=1",
                (void *)meta_request);
        }
    }    
    return AWS_OP_SUCCESS;
}

/* Implementation of prepare_request vtable function - consolidates all RDMA request preparation */
static int s_default_prepare_request(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request) {

    AWS_PRECONDITION(handler);
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);

    struct aws_s3_client *client = meta_request->client;

    if (!client || !client->enable_rdma || !client->rdma_provider || !meta_request->use_rdma) {
        AWS_LOGF_TRACE(AWS_LS_S3_META_REQUEST, "id=%p RDMA not available or not enabled", (void *)meta_request);
        return AWS_OP_SUCCESS;
    }

    /* Determine RDMA eligibility and extract buffer information */
    struct rdma_buffer_info buffer_info;
    s_determine_rdma_eligibility_and_buffer_info(meta_request, request, client, &buffer_info);

    /* Check if RDMA should be disabled for this request */
    bool should_use_rdma = buffer_info.is_eligible && !request->disable_rdma_on_retry;
    
    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p RDMA decision: should_use=%d (eligible=%d, has_buffer=%d)",
        (void *)meta_request,
        should_use_rdma,
        buffer_info.is_eligible,
        buffer_info.buffer != NULL);

    if (should_use_rdma && buffer_info.buffer) {
        bool is_suitable = aws_s3_rdma_provider_is_memory_suitable(
            client->rdma_provider, buffer_info.buffer, buffer_info.size);
        
        AWS_LOGF_DEBUG(
            AWS_LS_S3_META_REQUEST,
            "id=%p Memory suitability: suitable=%d, size=%zu",
            (void *)meta_request,
            is_suitable,
            buffer_info.size);
        
        if (is_suitable) {
            s_register_and_process_rdma_tokens(handler, meta_request, request, client, &buffer_info);
        }
    }

    return AWS_OP_SUCCESS;
}

/* Implementation of process_response_headers vtable function */
static int s_default_process_response_headers(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    const struct aws_http_header *headers,
    size_t headers_count) {

    AWS_PRECONDITION(handler);
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);

    (void)handler;  /* Unused parameter */

    struct aws_s3_client *client = meta_request->client;
    if (!client->rdma_provider) {
        return AWS_OP_SUCCESS;
    }

    uint64_t rdma_bytes_transferred = 0;

    for (size_t i = 0; i < headers_count; ++i) {
        const struct aws_byte_cursor *name = &headers[i].name;
        const struct aws_byte_cursor *value = &headers[i].value;

        /* Process RDMA reply token if present */
        struct aws_byte_cursor rdma_reply_header = aws_s3_rdma_provider_get_rdma_reply_header_name(client->rdma_provider);
        if (aws_byte_cursor_eq_ignore_case(name, &rdma_reply_header)) {
            /* Call reply token processing function */
            int rdma_reply_result = s_default_process_reply_token(handler, meta_request, request, value);
            
            /* If RDMA reply token processing fails, handle based on request type and response status */
            if (rdma_reply_result != AWS_OP_SUCCESS) {
                /* Check if meta request is already finishing (paused/cancelled) before triggering retry */
                aws_s3_meta_request_lock_synced_data(meta_request);
                bool is_finishing = aws_s3_meta_request_has_finish_result_synced(meta_request);
                aws_s3_meta_request_unlock_synced_data(meta_request);

                if (is_finishing) {
                    AWS_LOGF_DEBUG(
                        AWS_LS_S3_META_REQUEST,
                        "id=%p RDMA failed but meta request is already finishing, not triggering retry",
                        (void *)meta_request);
                } else {
                    /* Check if response status indicates success (200, 204, 206) */
                    bool is_success_status = (request->send_data.response_status == 200 ||
                                             request->send_data.response_status == 204 ||
                                             request->send_data.response_status == 206);
                    
                    if (is_success_status) {
                        /* For successful responses, RDMA failure depends on request type:
                         * - GET: Server fell back to HTTP, data delivered via body callback - treat as success
                         * - PUT: Server expected RDMA but didn't receive data - trigger retry */
                        if (request->request_type == AWS_S3_REQUEST_TYPE_GET_OBJECT) {
                            AWS_LOGF_DEBUG(
                                AWS_LS_S3_META_REQUEST,
                                "id=%p GET request: RDMA failed but data delivered via HTTP (status=%d), treating as success",
                                (void *)meta_request,
                                request->send_data.response_status);
                        } else {
                            /* PUT/POST with success status but RDMA failed - operation incomplete, retry needed */
                            AWS_LOGF_DEBUG(
                                AWS_LS_S3_META_REQUEST,
                                "id=%p PUT/POST request: RDMA failed with success status (%d), returning error to trigger retry",
                                (void *)meta_request,
                                request->send_data.response_status);
                            return rdma_reply_result;
                        }
                    } else {
                        /* Error status (4xx, 5xx): Server returned error, no RDMA reply expected.
                         * Allow processing to continue so we can read error body via HTTP. */
                        AWS_LOGF_DEBUG(
                            AWS_LS_S3_META_REQUEST,
                            "id=%p RDMA failed with error status (%d), allowing error body to be processed via HTTP",
                            (void *)meta_request,
                            request->send_data.response_status);
                    }
                }
            }
        }

        /* Process RDMA bytes transferred header */
        if (request->request_type == AWS_S3_REQUEST_TYPE_GET_OBJECT &&
            (request->send_data.response_status == 200 ||
             request->send_data.response_status == 206)) {
            struct aws_byte_cursor rdma_bytes_header = aws_s3_rdma_provider_get_rdma_bytes_header_name(client->rdma_provider);
            if (aws_byte_cursor_eq_ignore_case(name, &rdma_bytes_header)) {
                rdma_bytes_transferred = s_process_rdma_bytes_header(meta_request, request, value);
            }
        }
    }

    /* After all headers have been processed, update Content-Length and response_body.len if we captured RDMA bytes */
    s_update_headers_for_rdma_transfer(meta_request, request, rdma_bytes_transferred);
    
    /* Mark that server completed RDMA transfer if we detected RDMA response headers */
    if (rdma_bytes_transferred > 0) {
        request->rdma_transfer_succeeded = 1;
        AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, 
                       "id=%p: Server used RDMA transfer (%llu bytes)", 
                       (void *)meta_request, (unsigned long long)rdma_bytes_transferred);
    }

    return AWS_OP_SUCCESS;
}

/* Implementation of validate_content_size vtable function */
static int s_default_validate_content_size(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request) {

    AWS_PRECONDITION(handler);
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);

    (void)handler;  /* Unused parameter */

    return s_validate_initial_get_object_size(meta_request, request);
}

/* Default vtable implementation */
static const struct aws_s3_rdma_request_handler_vtable s_default_rdma_request_handler_vtable = {
    .handler_name = "DefaultRDMARequestHandler",
    .init = NULL, /* Not needed for default implementation */
    .cleanup = NULL, /* Not needed for default implementation */
    .process_request_token = s_default_process_request_token,
    .process_reply_token = s_default_process_reply_token,
    .calculate_rdma_checksum_and_add_header = s_default_calculate_rdma_checksum_and_add_header,
    .prepare_request = s_default_prepare_request,
    .process_response_headers = s_default_process_response_headers,
    .validate_content_size = s_default_validate_content_size,
};

int aws_s3_rdma_request_handler_new(
    struct aws_allocator *allocator,
    struct aws_s3_rdma_provider *rdma_provider,
    struct aws_s3_rdma_request_handler **out_handler) {

    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(rdma_provider);
    AWS_PRECONDITION(out_handler);

    struct aws_s3_rdma_request_handler *handler = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_rdma_request_handler));
    if (!handler) {
        return AWS_OP_ERR;
    }

    handler->allocator = allocator;
    aws_ref_count_init(&handler->ref_count, handler, s_aws_s3_rdma_request_handler_destroy);

    /* Hold reference to RDMA provider */
    handler->rdma_provider = rdma_provider;
    aws_ref_count_acquire(&rdma_provider->ref_count);

    /* Use default vtable implementation */
    handler->vtable = &s_default_rdma_request_handler_vtable;

    *out_handler = handler;
    return AWS_OP_SUCCESS;
}

void aws_s3_rdma_request_handler_release(struct aws_s3_rdma_request_handler *handler) {
    if (handler) {
        aws_ref_count_release(&handler->ref_count);
    }
}

int aws_s3_rdma_request_handler_process_request_token(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    void *request_buffer,
    size_t buffer_size) {

    if (!handler || !handler->vtable || !handler->vtable->process_request_token) {
        return AWS_OP_ERR;
    }

    return handler->vtable->process_request_token(handler, meta_request, request, request_buffer, buffer_size);
}

int aws_s3_rdma_request_handler_calculate_rdma_checksum_and_add_header(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    const void *buffer,
    size_t buffer_size) {

    if (!handler || !handler->vtable || !handler->vtable->calculate_rdma_checksum_and_add_header) {
        return AWS_OP_ERR;
    }

    return handler->vtable->calculate_rdma_checksum_and_add_header(handler, meta_request, request, buffer, buffer_size);
}

int aws_s3_rdma_request_handler_prepare_request(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request) {

    if (!handler || !handler->vtable || !handler->vtable->prepare_request) {
        return AWS_OP_ERR;
    }

    return handler->vtable->prepare_request(handler, meta_request, request);
}

int aws_s3_rdma_request_handler_process_response_headers(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    const struct aws_http_header *headers,
    size_t headers_count) {

    if (!handler || !handler->vtable || !handler->vtable->process_response_headers) {
        return AWS_OP_ERR;
    }

    return handler->vtable->process_response_headers(handler, meta_request, request, headers, headers_count);
}


int aws_s3_rdma_request_handler_validate_content_size(
    struct aws_s3_rdma_request_handler *handler,
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request) {

    if (!handler || !handler->vtable || !handler->vtable->validate_content_size) {
        return AWS_OP_ERR;
    }

    return handler->vtable->validate_content_size(handler, meta_request, request);
}
