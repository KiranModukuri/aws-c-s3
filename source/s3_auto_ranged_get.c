/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_auto_ranged_get.h"
#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3_meta_request_impl.h"
#include "aws/s3/private/s3_request_messages.h"
#include "aws/s3/private/s3_util.h"
#include <aws/common/string.h>
#include <aws/common/encoding.h>
#include <inttypes.h>

/* Dont use buffer pool when we know response size, and its below this number,
 * i.e. when user provides explicit range that is small, ex. range = 1-100.
 * Instead of going through the pool in that case, we just use a dynamic buffer
 * for response (pre-mempool behavior). */
const uint64_t s_min_size_response_for_pooling = 1 * 1024 * 1024;
const uint32_t s_conservative_max_requests_in_flight = 8;
const struct aws_byte_cursor g_application_xml_value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("application/xml");

static void s_s3_meta_request_auto_ranged_get_destroy(struct aws_s3_meta_request *meta_request);

static bool s_s3_auto_ranged_get_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request);

static struct aws_future_void *s_s3_auto_ranged_get_prepare_request(struct aws_s3_request *request);

static void s_s3_auto_ranged_get_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code);

static struct aws_s3_meta_request_vtable s_s3_auto_ranged_get_vtable = {
    .update = s_s3_auto_ranged_get_update,
    .send_request_finish = aws_s3_meta_request_send_request_finish_default,
    .prepare_request = s_s3_auto_ranged_get_prepare_request,
    .init_signing_date_time = aws_s3_meta_request_init_signing_date_time_default,
    .sign_request = aws_s3_meta_request_sign_request_default,
    .finished_request = s_s3_auto_ranged_get_request_finished,
    .destroy = s_s3_meta_request_auto_ranged_get_destroy,
    .finish = aws_s3_meta_request_finish_default,
};

static int s_s3_auto_ranged_get_success_status(struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);

    struct aws_s3_auto_ranged_get *auto_ranged_get = meta_request->impl;
    AWS_PRECONDITION(auto_ranged_get);

    if (auto_ranged_get->initial_message_has_range_header) {
        return AWS_HTTP_STATUS_CODE_206_PARTIAL_CONTENT;
    }

    return AWS_HTTP_STATUS_CODE_200_OK;
}

/* Allocate a new auto-ranged-get meta request. */
struct aws_s3_meta_request *aws_s3_meta_request_auto_ranged_get_new(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    size_t part_size,
    const struct aws_s3_meta_request_options *options) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(options);
    AWS_PRECONDITION(options->message);

    struct aws_s3_auto_ranged_get *auto_ranged_get =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_auto_ranged_get));

    /* Try to initialize the base type. */
    if (aws_s3_meta_request_init_base(
            allocator,
            client,
            part_size,
            false,
            false,
            options,
            auto_ranged_get,
            &s_s3_auto_ranged_get_vtable,
            &auto_ranged_get->base)) {

        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p Could not initialize base type for Auto-Ranged-Get Meta Request.",
            (void *)auto_ranged_get);
        aws_mem_release(allocator, auto_ranged_get);
        return NULL;
    }

    struct aws_http_headers *headers = aws_http_message_get_headers(auto_ranged_get->base.initial_request_message);
    AWS_ASSERT(headers != NULL);

    if (aws_http_headers_has(headers, g_range_header_name)) {
        auto_ranged_get->initial_message_has_range_header = true;
        if (aws_s3_parse_request_range_header(
                headers,
                &auto_ranged_get->initial_message_has_start_range,
                &auto_ranged_get->initial_message_has_end_range,
                &auto_ranged_get->initial_range_start,
                &auto_ranged_get->initial_range_end)) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_META_REQUEST,
                "id=%p Could not parse Range header for Auto-Ranged-Get Meta Request.",
                (void *)auto_ranged_get);
            goto on_error;
        }
    }
    auto_ranged_get->initial_message_has_if_match_header = aws_http_headers_has(headers, g_if_match_header_name);
    auto_ranged_get->synced_data.first_part_size = auto_ranged_get->base.part_size;

    /* When user provides a buffer for GET, use that size as the first_part_size hint.
     * This ensures the discovery request doesn't request more data than the buffer can hold. */
    if (options->user_buffer_options != NULL &&
        options->user_buffer_options->transfer_buffer != NULL &&
        options->user_buffer_options->transfer_buffer_size > 0 &&
        options->user_buffer_options->transfer_buffer_size < auto_ranged_get->synced_data.first_part_size) {
        auto_ranged_get->synced_data.first_part_size = options->user_buffer_options->transfer_buffer_size;
        AWS_LOGF_DEBUG(
            AWS_LS_S3_META_REQUEST,
            "id=%p Auto-Ranged GET: Using user buffer size %zu as first_part_size hint",
            (void *)&auto_ranged_get->base,
            options->user_buffer_options->transfer_buffer_size);
    }

    if (options->object_size_hint != NULL) {
        auto_ranged_get->object_size_hint_available = true;
        auto_ranged_get->object_size_hint = *options->object_size_hint;
    }
    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST, "id=%p Created new Auto-Ranged Get Meta Request.", (void *)&auto_ranged_get->base);

    return &auto_ranged_get->base;

on_error:
    /* This will also clean up the auto_ranged_get */
    aws_s3_meta_request_release(&(auto_ranged_get->base));
    return NULL;
}

static void s_s3_meta_request_auto_ranged_get_destroy(struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);

    struct aws_s3_auto_ranged_get *auto_ranged_get = meta_request->impl;
    aws_string_destroy(auto_ranged_get->etag);
    aws_mem_release(meta_request->allocator, auto_ranged_get);
}

/*
 * This function returns the type of first request which we will also use to discover overall object size.
 */
static enum aws_s3_auto_ranged_get_request_type s_s3_get_request_type_for_discovering_object_size(
    const struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);
    struct aws_s3_auto_ranged_get *auto_ranged_get = meta_request->impl;
    AWS_ASSERT(auto_ranged_get);

    /*
     * When we attempt to download an empty file using the `AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE`
     * request type, the request fails with an empty file error. We then reset `object_range_known`
     * (`object_range_empty` is set to true) and try to download the file again with
     * `AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1`. We send another request, even though there is
     * no body, to provide successful response headers to the user. If the file is still empty, successful response
     * headers will be provided to the users. Otherwise, the newer version of the file will be downloaded.
     */
    if (auto_ranged_get->synced_data.object_range_empty != 0) {
        auto_ranged_get->synced_data.object_range_empty = 0;
        return AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1;
    }

    /*
     * If a range header exists but has no start-range (i.e. Range: bytes=-100), we perform a HeadRequest. If the
     * start-range is unknown, we could potentially execute a request from the end-range and keep that request around
     * until the meta request finishes. However, this approach involves the complexity of managing backpressure. For
     * simplicity, we execute a HeadRequest if the start-range is not specified.
     */
    if (auto_ranged_get->initial_message_has_range_header != 0) {
        return auto_ranged_get->initial_message_has_start_range
                   ? AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE
                   : AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_HEAD_OBJECT;
    }

    /* If we don't need checksum validation, then discover the size of the object while trying to get the first part. */
    if (!meta_request->checksum_config.validate_response_checksum) {
        return AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE;
    }

    /* If the object_size_hint indicates that it is a small one part file, then try to get the file directly
     * TODO: Bypass memory limiter so that we don't overallocate memory for small files
     * 
     * Note: Skip the partNumber=1 optimization when RDMA is enabled, because:
     * - If the object is actually multipart, server returns only the first part
     * - RDMA requires exact size matching (allocated buffer vs actual transfer)
     * - This causes RDMA protocol abort and 400 Bad Request
     * - Better to use Range-based GET or HEAD+Range for RDMA
     */
    if (auto_ranged_get->object_size_hint_available && 
        auto_ranged_get->object_size_hint <= meta_request->part_size &&
        !(meta_request->client->enable_rdma && meta_request->use_rdma)) {
        return AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1;
    }

    /* Otherwise, do a headObject so that we can validate checksum if the file was uploaded as a single part */
    return AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_HEAD_OBJECT;
}

static bool s_s3_auto_ranged_get_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(out_request);

    struct aws_s3_auto_ranged_get *auto_ranged_get = meta_request->impl;
    struct aws_s3_request *request = NULL;
    bool work_remaining = false;

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_meta_request_lock_synced_data(meta_request);

        /* If nothing has set the "finish result" then this meta request is still in progress, and we can potentially
         * send additional requests. */
        if (!aws_s3_meta_request_has_finish_result_synced(meta_request)) {

            if ((flags & AWS_S3_META_REQUEST_UPDATE_FLAG_CONSERVATIVE) != 0) {
                uint32_t num_requests_in_flight =
                    (auto_ranged_get->synced_data.num_parts_requested -
                     auto_ranged_get->synced_data.num_parts_completed) +
                    (uint32_t)aws_priority_queue_size(&meta_request->synced_data.pending_body_streaming_requests);

                /* auto-ranged-gets make use of body streaming, which will hold onto response bodies if parts earlier in
                 * the file haven't arrived yet. This can potentially create a lot of backed up requests, causing us to
                 * hit our global request limit. To help mitigate this, when the "conservative" flag is passed in, we
                 * only allow the total amount of requests being sent/streamed to be inside a set limit.  */
                if (num_requests_in_flight > s_conservative_max_requests_in_flight) {
                    goto has_work_remaining;
                }
            }

            /* If the overall range of the object that we are trying to retrieve isn't known yet, then we need to send a
             * request to figure that out. */
            if (!auto_ranged_get->synced_data.object_range_known) {
                if (auto_ranged_get->synced_data.head_object_sent ||
                    auto_ranged_get->synced_data.num_parts_requested > 0) {
                    goto has_work_remaining;
                }
                switch (s_s3_get_request_type_for_discovering_object_size(meta_request)) {
                    case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_HEAD_OBJECT:
                        AWS_LOGF_INFO(
                            AWS_LS_S3_META_REQUEST,
                            "id=%p: Doing a HeadObject to discover the size of the object",
                            (void *)meta_request);
                        request = aws_s3_request_new(
                            meta_request,
                            AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_HEAD_OBJECT,
                            AWS_S3_REQUEST_TYPE_HEAD_OBJECT,
                            0 /*part_number*/,
                            AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);
                        auto_ranged_get->synced_data.head_object_sent = true;
                        break;
                    case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1:
                        AWS_LOGF_INFO(
                            AWS_LS_S3_META_REQUEST,
                            "id=%p: Doing a 'GET_OBJECT_WITH_PART_NUMBER_1' to discover the size of the object and get "
                            "the first part",
                            (void *)meta_request);

                        request = aws_s3_request_new(
                            meta_request,
                            AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1,
                            AWS_S3_REQUEST_TYPE_GET_OBJECT,
                            1 /*part_number*/,
                            AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS |
                                AWS_S3_REQUEST_FLAG_ALLOCATE_BUFFER_FROM_POOL);
                        ++auto_ranged_get->synced_data.num_parts_requested;

                        break;
                    case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE:
                        AWS_LOGF_INFO(
                            AWS_LS_S3_META_REQUEST,
                            "id=%p: Doing a 'GET_OBJECT_WITH_RANGE' to discover the size of the object and get the "
                            "first part",
                            (void *)meta_request);

                        uint64_t part_range_start = 0;
                        uint64_t first_part_size = auto_ranged_get->synced_data.first_part_size;
                        if (auto_ranged_get->initial_message_has_range_header) {
                            /*
                             * Currently, we only discover the size of the object when the initial range header includes
                             * a start-range. If we ever implement skipping the HeadRequest for a Range request without
                             * a start-range, this will need to update.
                             */
                            AWS_ASSERT(auto_ranged_get->initial_message_has_start_range);
                            part_range_start = auto_ranged_get->initial_range_start;

                            if (auto_ranged_get->initial_message_has_end_range) {
                                first_part_size = aws_min_u64(
                                    first_part_size,
                                    auto_ranged_get->initial_range_end - auto_ranged_get->initial_range_start + 1);
                            }

                            auto_ranged_get->synced_data.first_part_size = first_part_size;
                        }
                        AWS_LOGF_INFO(
                            AWS_LS_S3_META_REQUEST,
                            "id=%p: Doing a ranged get to discover the size of the object and get the first part",
                            (void *)meta_request);

                        request = aws_s3_request_new(
                            meta_request,
                            AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE,
                            AWS_S3_REQUEST_TYPE_GET_OBJECT,
                            1 /*part_number*/,
                            AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS |
                                AWS_S3_REQUEST_FLAG_ALLOCATE_BUFFER_FROM_POOL);
                        request->part_range_start = part_range_start;
                        request->part_range_end = part_range_start + first_part_size - 1; /* range-end is inclusive */
                        ++auto_ranged_get->synced_data.num_parts_requested;
                        break;
                    default:
                        AWS_FATAL_ASSERT(
                            0 && "s_s3_get_request_type_for_discovering_object_size returned unexpected discover "
                                 "object size request type");
                }
                request->discovers_object_size = true;
                goto has_work_remaining;
            }

            /* If there are still more parts to be requested */
            if (auto_ranged_get->synced_data.num_parts_requested < auto_ranged_get->synced_data.total_num_parts) {

                if (meta_request->client->enable_read_backpressure) {
                    /* Don't start a part until we have enough window to send bytes to the user.
                     *
                     * Note that we start a part once we have enough window to deliver ANY of its bytes.
                     * If we waited until the window was large enough for the WHOLE part,
                     * we could end up stuck in a situation where the user is
                     * waiting for more bytes before they'll open the window,
                     * and this implementation is waiting for more window before it will send more parts. */
                    uint64_t read_data_requested =
                        auto_ranged_get->synced_data.num_parts_requested * meta_request->part_size;
                    if (read_data_requested >= meta_request->synced_data.read_window_running_total) {

                        /* Avoid spamming users with this DEBUG message */
                        if (auto_ranged_get->synced_data.read_window_warning_issued == 0) {
                            auto_ranged_get->synced_data.read_window_warning_issued = 1;

                            AWS_LOGF_DEBUG(
                                AWS_LS_S3_META_REQUEST,
                                "id=%p: Download paused because read window is zero. "
                                "You must increment to window to continue.",
                                (void *)meta_request);
                        }

                        goto has_work_remaining;
                    }

                    auto_ranged_get->synced_data.read_window_warning_issued = 0;
                }

                request = aws_s3_request_new(
                    meta_request,
                    AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE,
                    AWS_S3_REQUEST_TYPE_GET_OBJECT,
                    auto_ranged_get->synced_data.num_parts_requested + 1 /*part_number*/,
                    AWS_S3_REQUEST_FLAG_ALLOCATE_BUFFER_FROM_POOL);

                aws_s3_calculate_auto_ranged_get_part_range(
                    auto_ranged_get->synced_data.object_range_start,
                    auto_ranged_get->synced_data.object_range_end,
                    meta_request->part_size,
                    auto_ranged_get->synced_data.first_part_size,
                    request->part_number,
                    &request->part_range_start,
                    &request->part_range_end);

                ++auto_ranged_get->synced_data.num_parts_requested;
                goto has_work_remaining;
            }

            /* If there are parts that have not attempted delivery to the caller, then there is still work being done.
             */
            if (meta_request->synced_data.num_parts_delivery_completed < auto_ranged_get->synced_data.total_num_parts) {
                goto has_work_remaining;
            }
        } else {
            /* Else, if there is a finish result set, make sure that all work-in-progress winds down before the meta
             * request completely exits. */

            if (auto_ranged_get->synced_data.head_object_sent && !auto_ranged_get->synced_data.head_object_completed) {
                goto has_work_remaining;
            }

            /* Wait for all requests to complete (successfully or unsuccessfully) before finishing.*/
            if (auto_ranged_get->synced_data.num_parts_completed < auto_ranged_get->synced_data.num_parts_requested) {
                goto has_work_remaining;
            }

            /* If some parts are still being delivered to the caller, then wait for those to finish. */
            if (meta_request->synced_data.num_parts_delivery_completed <
                meta_request->synced_data.num_parts_delivery_sent) {
                goto has_work_remaining;
            }
        }

        goto no_work_remaining;

    has_work_remaining:
        work_remaining = true;

        if (request != NULL) {
            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "id=%p: Returning request %p for part %d of %d",
                (void *)meta_request,
                (void *)request,
                request->part_number,
                auto_ranged_get->synced_data.total_num_parts);
        }

    no_work_remaining:
        /* If some events are still being delivered to caller, then wait for those to finish */
        if (!work_remaining && aws_s3_meta_request_are_events_out_for_delivery_synced(meta_request)) {
            work_remaining = true;
        }

        if (!work_remaining) {
            aws_s3_meta_request_set_success_synced(meta_request, s_s3_auto_ranged_get_success_status(meta_request));
            if (auto_ranged_get->synced_data.num_parts_checksum_validated ==
                auto_ranged_get->synced_data.num_parts_requested) {
                /* If we have validated the checksum for every part, we set the meta request level checksum validation
                 * result.*/
                meta_request->synced_data.finish_result.did_validate = true;
                meta_request->synced_data.finish_result.validation_algorithm = auto_ranged_get->validation_algorithm;
            }
        }

        aws_s3_meta_request_unlock_synced_data(meta_request);
    }
    /* END CRITICAL SECTION */

    if (work_remaining) {
        *out_request = request;
    } else {
        AWS_ASSERT(request == NULL);
        aws_s3_meta_request_finish(meta_request);
    }

    return work_remaining;
}

/**
 * Setup RDMA for GET request including user buffer mapping, buffer allocation, 
 * registration, and token generation.
 * 
 * @param meta_request The meta request
 * @param auto_ranged_get The auto ranged get implementation
 * @param request The request to setup RDMA for
 * @param message The HTTP message to add RDMA headers to
 * @return AWS_OP_SUCCESS on success, AWS_OP_ERR on failure
 */
static int s_prepare_rdma_for_get_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_auto_ranged_get *auto_ranged_get,
    struct aws_s3_request *request,
    struct aws_http_message *message) {
    
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(auto_ranged_get);
    AWS_PRECONDITION(request);
    AWS_PRECONDITION(message);

    /* If user provided a static receive buffer, map this request's part into that buffer before RDMA setup */
    if (meta_request->use_rdma && meta_request->user_buffer_options.transfer_buffer) {
        void *base = meta_request->user_buffer_options.transfer_buffer;
        size_t base_size = meta_request->user_buffer_options.transfer_buffer_size;

        uint64_t part_start = request->part_range_start;
        uint64_t part_end = request->part_range_end;
        size_t part_size = 0;
        uint64_t object_start = 0;

        if (request->request_tag == AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE && part_end >= part_start) {
            part_size = (size_t)((part_end - part_start) + 1);
            object_start = auto_ranged_get->synced_data.object_range_start;
        } else if (request->request_tag == AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1) {
            /* For the first-part discovery request, use first_part_size if known, else part_size */
            part_start = auto_ranged_get->synced_data.object_range_start;
            if (auto_ranged_get->synced_data.first_part_size > 0) {
                part_size = (size_t)auto_ranged_get->synced_data.first_part_size;
            } else {
                part_size = (size_t)meta_request->part_size;
            }
            part_end = part_start + (part_size ? (part_size - 1) : 0);
            object_start = auto_ranged_get->synced_data.object_range_start;
        }

        if (part_size > 0) {
            uint64_t offset_in_object = part_start - object_start;
            if (offset_in_object <= (uint64_t)base_size && (offset_in_object + part_size) <= (uint64_t)base_size) {
                uint8_t *ptr = (uint8_t *)base + offset_in_object;
                request->send_data.response_body.buffer = ptr;
                request->send_data.response_body.len = 0;
                request->send_data.response_body.capacity = part_size;
                request->is_user_provided_buffer = 1;  /* Mark as user buffer slice */
                
                AWS_LOGF_DEBUG(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p GET Part: Using user buffer slice at offset=%llu, size=%zu",
                    (void *)meta_request,
                    (unsigned long long)offset_in_object,
                    part_size);
            } else {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p User buffer too small: offset=%llu size=%zu base_size=%zu",
                    (void *)meta_request,
                    (unsigned long long)offset_in_object,
                    part_size,
                    base_size);
                /* Leave buffer unset; normal pool path will be used */
            }
        }
    }

    /* RDMA ENHANCEMENT: Pre-allocate response buffers and generate RDMA tokens for GET requests */
    struct aws_s3_client *client = meta_request->client;
    if (client && client->enable_rdma && client->rdma_provider && meta_request->use_rdma &&
        (request->request_tag == AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE ||
         request->request_tag == AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1)) {

        /* Calculate expected response size for this GET request */
        size_t expected_response_size = 0;

        if (request->request_tag == AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE) {
            /* For range requests, we know the exact size from range headers */
            if (request->part_range_end >= request->part_range_start) {
                expected_response_size = (request->part_range_end - request->part_range_start) + 1;
            }
        } else if (request->request_tag == AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1) {
            /* For part number 1, use the part size (may be adjusted during object size discovery) */
            expected_response_size = meta_request->part_size;
        }

        /* Only proceed with RDMA if the expected size meets the threshold */
        if (expected_response_size >= client->rdma_min_transfer_size) {

            /* PRE-ALLOCATE response buffer based on known size */
            if (request->send_data.response_body.capacity == 0) {
                /* Use buffer pool allocation if available, otherwise allocate directly */
                if (request->should_allocate_buffer_from_pool && request->ticket != NULL) {
                    request->send_data.response_body = aws_s3_buffer_ticket_claim(request->ticket);
                } else {
                    if (aws_byte_buf_init(&request->send_data.response_body, meta_request->allocator, expected_response_size)) {
                        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST,
                                       "id=%p Failed to allocate response buffer for GET RDMA: size=%zu",
                                       (void *)meta_request, expected_response_size);
                        return AWS_OP_ERR;
                    }
                }
            }

            /* Check RDMA memory suitability */
            void *response_buffer = request->send_data.response_body.buffer;
            size_t buffer_capacity = request->send_data.response_body.capacity;

            if (response_buffer && buffer_capacity > 0 &&
                aws_s3_rdma_provider_is_memory_suitable(client->rdma_provider, response_buffer, buffer_capacity)) {

                /* Register buffer with RDMA provider using buffer manager */
                int register_result = AWS_OP_SUCCESS;
                if (client->rdma_buffer_manager) {
                    register_result = aws_s3_rdma_buffer_manager_prepare_buffer_for_rdma(
                        client->rdma_buffer_manager, request, response_buffer, buffer_capacity);
                }
                if (register_result == AWS_OP_SUCCESS) {

                    /* Generate RDMA token for GET operation */
                    /* Extract S3 key from the HTTP message path */
                    struct aws_byte_cursor s3_key;
                    struct aws_byte_cursor request_path;

                    if (aws_http_message_get_request_path(message, &request_path) != AWS_OP_SUCCESS) {
                        AWS_LOGF_ERROR(AWS_LS_S3_META_REQUEST,
                                       "id=%p Failed to get request path for RDMA token generation",
                                       (void *)meta_request);
                        /* Clean up on path extraction failure */
                        if (client->rdma_buffer_manager) {
                            aws_s3_rdma_buffer_manager_finalize_buffer(client->rdma_buffer_manager, request);
                        }
                        return AWS_OP_ERR;
                    }

                    /* Parse S3 key from path (format: /bucket/key or /key) */
                    s3_key = request_path;
                    if (s3_key.len > 0 && s3_key.ptr[0] == '/') {
                        /* Skip leading slash */
                        aws_byte_cursor_advance(&s3_key, 1);
                    }

                    /* For bucket/key format, find the key after the first slash */
                    struct aws_byte_cursor temp_cursor = s3_key;
                    struct aws_byte_cursor bucket_part;
                    if (aws_byte_cursor_next_split(&temp_cursor, '/', &bucket_part) && temp_cursor.len > 0) {
                        /* Found bucket/key format, use the part after bucket */
                        s3_key = temp_cursor;
                    }

                    /* Remove query parameters if present */
                    struct aws_byte_cursor key_without_query = s3_key;
                    struct aws_byte_cursor query_part;
                    if (aws_byte_cursor_next_split(&key_without_query, '?', &query_part)) {
                        s3_key = query_part; /* First part before '?' */
                    }

                    struct aws_byte_cursor rdma_token_cursor;

                    int rdma_result = aws_s3_rdma_provider_prepare_get_token(
                        client->rdma_provider, &s3_key, response_buffer, buffer_capacity, 0, &rdma_token_cursor);

                    if (rdma_result == AWS_OP_SUCCESS) {
                        /* Add RDMA token header to GET request */
                        struct aws_http_headers *headers = aws_http_message_get_headers(message);
                        if (headers) {
                            struct aws_byte_cursor header_name = aws_s3_rdma_provider_get_rdma_token_header_name(client->rdma_provider);

                            /* Use the RDMA token directly (plugin now generates correct length) */
                            struct aws_byte_cursor header_value = rdma_token_cursor;

                            if (aws_http_headers_set(headers, header_name, header_value) != AWS_OP_SUCCESS) {
                                AWS_LOGF_WARN(
                                    AWS_LS_S3_META_REQUEST,
                                    "id=%p Failed to add GET RDMA token header for request %p",
                                    (void *)meta_request,
                                    (void *)request);

                                /* Clean up on header failure */
                                if (client->rdma_buffer_manager) {
                                    aws_s3_rdma_buffer_manager_finalize_buffer(client->rdma_buffer_manager, request);
                                }
                            }
                        } else {
                            AWS_LOGF_WARN(AWS_LS_S3_META_REQUEST,
                                          "id=%p No headers found in GET request message", (void *)meta_request);
                        }
                    } else {
                        AWS_LOGF_WARN(
                            AWS_LS_S3_META_REQUEST,
                            "id=%p Failed to generate GET RDMA token for request %p (result=%d)",
                            (void *)meta_request,
                            (void *)request,
                            rdma_result);

                        /* Clean up on token generation failure */
                        if (client->rdma_buffer_manager) {
                            aws_s3_rdma_buffer_manager_finalize_buffer(client->rdma_buffer_manager, request);
                        }
                    }
                } else {
                    AWS_LOGF_ERROR(
                        AWS_LS_S3_META_REQUEST,
                        "id=%p Failed to register GET response buffer with RDMA provider (result=%d). Buffer details: ptr=%p, size=%zu, alignment=0x%lx, RDMA provider=%p. Falling back to HTTP transfer.",
                        (void *)meta_request,
                        register_result,
                        response_buffer,
                        buffer_capacity,
                        (unsigned long)response_buffer % 4096,  /* Check 4K alignment */
                        (void *)client->rdma_provider);

                    /* Check if buffer meets basic requirements */
                    bool is_null = (response_buffer == NULL);
                    bool is_zero_size = (buffer_capacity == 0);
                    bool below_threshold = (buffer_capacity < client->rdma_min_transfer_size);

                    AWS_LOGF_ERROR(
                        AWS_LS_S3_META_REQUEST,
                        "id=%p RDMA registration failure analysis: buffer_null=%d, zero_size=%d, below_threshold=%d (threshold=%zu)",
                        (void *)meta_request,
                        is_null, is_zero_size, below_threshold,
                        client->rdma_min_transfer_size);

                    /* Mark request to disable RDMA on retry if this fails */
                    request->disable_rdma_on_retry = 1;

                    /* Continue without RDMA - request will use standard HTTP transfer */
                }
            }
        }
    }

    return AWS_OP_SUCCESS;
}

/* Given a request, prepare it for sending based on its description.
 * Currently, this is actually synchronous. */
static struct aws_future_void *s_s3_auto_ranged_get_prepare_request(struct aws_s3_request *request) {
    AWS_PRECONDITION(request);
    struct aws_s3_meta_request *meta_request = request->meta_request;

    /* Generate a new ranged get request based on the original message. */
    struct aws_http_message *message = NULL;
    struct aws_s3_auto_ranged_get *auto_ranged_get = meta_request->impl;

    bool success = false;

    switch (request->request_tag) {
        case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_HEAD_OBJECT:
            /* A head object will be a copy of the original headers but with a HEAD request method. */
            message = aws_s3_message_util_copy_http_message_no_body_all_headers(
                meta_request->allocator, meta_request->initial_request_message);
            if (message) {
                aws_http_message_set_request_method(message, g_head_method);
            }
            break;
        case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE:
            message = aws_s3_ranged_get_object_message_new(
                meta_request->allocator,
                meta_request->initial_request_message,
                request->part_range_start,
                request->part_range_end);
            break;
        case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1:
            message = aws_s3_message_util_copy_http_message_no_body_all_headers(
                meta_request->allocator, meta_request->initial_request_message);
            if (message) {
                aws_s3_message_util_set_multipart_request_path(
                    meta_request->allocator, NULL, request->part_number, false, message);
            }
            break;
    }

    if (message == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p Could not create message for request with tag %d for auto-ranged-get meta request.",
            (void *)meta_request,
            request->request_tag);
        goto finish;
    }
    if (meta_request->checksum_config.validate_response_checksum) {
        aws_http_headers_set(aws_http_message_get_headers(message), g_request_validation_mode, g_enabled);
    }
    if (!auto_ranged_get->initial_message_has_if_match_header && auto_ranged_get->etag) {
        /* Add the if_match to the request */
        AWS_LOGF_DEBUG(
            AWS_LS_S3_META_REQUEST,
            "id=%p: Added the If-Match header to request %p for part %d",
            (void *)meta_request,
            (void *)request,
            request->part_number);
        aws_http_headers_set(
            aws_http_message_get_headers(message),
            g_if_match_header_name,
            aws_byte_cursor_from_string(auto_ranged_get->etag));
    }

    aws_s3_request_setup_send_data(request, message);

    /* Setup RDMA for GET request if applicable */
    int rdma_setup_result = s_prepare_rdma_for_get_request(meta_request, auto_ranged_get, request, message);
    aws_http_message_release(message);
    if (rdma_setup_result != AWS_OP_SUCCESS) {
        goto finish;
    }
    
    /* Success! */
    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p: Created request %p for part %d allocated from pool %d",
        (void *)meta_request,
        (void *)request,
        request->part_number,
        request->should_allocate_buffer_from_pool);

    success = true;

finish:;
    struct aws_future_void *future = aws_future_void_new(meta_request->allocator);
    if (success) {
        aws_future_void_set_result(future);
    } else {
        aws_future_void_set_error(future, aws_last_error_or_unknown());
    }
    return future;
}

/* Check the finish result of meta request.
 * Return true if the request failed because it downloaded an empty file.
 * Return false if the request failed for any other reason */
static bool s_check_empty_file_download_error(struct aws_s3_request *failed_request) {
    struct aws_http_headers *failed_headers = failed_request->send_data.response_headers;
    struct aws_byte_buf failed_body = failed_request->send_data.response_body;
    if (failed_headers && failed_body.capacity > 0) {
        struct aws_byte_cursor content_type;
        AWS_ZERO_STRUCT(content_type);
        if (!aws_http_headers_get(failed_headers, g_content_type_header_name, &content_type)) {
            /* Content type found */
            if (aws_byte_cursor_eq_ignore_case(&content_type, &g_application_xml_value)) {
                /* XML response */
                struct aws_byte_cursor xml_doc = aws_byte_cursor_from_buf(&failed_body);
                const char *path_to_size[] = {"Error", "ActualObjectSize", NULL};
                struct aws_byte_cursor size = {0};
                aws_xml_get_body_at_path(failed_request->allocator, xml_doc, path_to_size, &size);
                if (aws_byte_cursor_eq_c_str(&size, "0")) {
                    return true;
                }
            }
        }
    }
    return false;
}

static int s_discover_object_range_and_size(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code,
    uint64_t *out_object_range_start,
    uint64_t *out_object_range_end,
    uint64_t *out_object_size,
    uint64_t *out_first_part_size,
    bool *out_empty_file_error) {

    AWS_PRECONDITION(out_object_size);
    AWS_PRECONDITION(out_object_range_start);
    AWS_PRECONDITION(out_object_range_end);
    AWS_PRECONDITION(out_first_part_size);

    int result = AWS_OP_ERR;

    uint64_t content_length = 0;
    uint64_t object_size = 0;
    uint64_t object_range_start = 0;
    uint64_t object_range_end = 0;
    uint64_t first_part_size = 0;

    AWS_ASSERT(request->discovers_object_size);
    struct aws_s3_auto_ranged_get *auto_ranged_get = meta_request->impl;
    switch (request->request_tag) {
        case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_HEAD_OBJECT:
            if (error_code != AWS_ERROR_SUCCESS) {
                /* If the head request failed, there's nothing we can do, so resurface the error code. */
                aws_raise_error(error_code);
                break;
            }

            /* There should be a Content-Length header that indicates the total size of the range.*/
            if (aws_s3_parse_content_length_response_header(
                    meta_request->allocator, request->send_data.response_headers, &content_length)) {

                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Could not find content-length header for request %p",
                    (void *)meta_request,
                    (void *)request);
                break;
            }

            /* if the inital message had a ranged header, there should also be a Content-Range header that specifies the
             * object range and total object size. Otherwise, the size and range should be equal to the
             * total_content_length. */
            if (!auto_ranged_get->initial_message_has_range_header) {
                object_size = content_length;
                if (content_length > 0) {
                    object_range_end = content_length - 1; /* range-end is inclusive */
                }
            } else if (aws_s3_parse_content_range_response_header(
                           meta_request->allocator,
                           request->send_data.response_headers,
                           &object_range_start,
                           &object_range_end,
                           &object_size)) {

                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Could not find content-range header for request %p",
                    (void *)meta_request,
                    (void *)request);
                break;
            }

            result = AWS_OP_SUCCESS;
            break;
        case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1:
            AWS_ASSERT(request->part_number == 1);
            AWS_ASSERT(request->send_data.response_headers != NULL);
            /* There should be a Content-Length header that indicates the size of first part. */
            if (aws_s3_parse_content_length_response_header(
                    meta_request->allocator, request->send_data.response_headers, &content_length)) {

                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Could not find content-length header for request %p",
                    (void *)meta_request,
                    (void *)request);
                break;
            }
            first_part_size = content_length;

            if (first_part_size > 0) {
                /* Parse the object size from the part response. */
                if (aws_s3_parse_content_range_response_header(
                        meta_request->allocator, request->send_data.response_headers, NULL, NULL, &object_size)) {

                    AWS_LOGF_ERROR(
                        AWS_LS_S3_META_REQUEST,
                        "id=%p Could not find content-range header for request %p",
                        (void *)meta_request,
                        (void *)request);
                    break;
                }
                /* When discovering the object size via GET_OBJECT_WITH_PART_NUMBER_1, the object range is the entire
                 * object. */
                object_range_start = 0;
                object_range_end = object_size - 1; /* range-end is inclusive */
            }

            result = AWS_OP_SUCCESS;
            break;
        case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE:
            AWS_ASSERT(request->part_number == 1);

            if (error_code != AWS_ERROR_SUCCESS) {
                /* If we hit an empty file while trying to discover the object-size via part, then this request
                failure
                 * is as designed. */
                if (!auto_ranged_get->initial_message_has_range_header && s_check_empty_file_download_error(request)) {
                    AWS_LOGF_DEBUG(
                        AWS_LS_S3_META_REQUEST,
                        "id=%p Detected empty file with request %p. Sending new request without range header.",
                        (void *)meta_request,
                        (void *)request);

                    object_size = 0ULL;
                    *out_empty_file_error = true;
                    result = AWS_OP_SUCCESS;
                } else {
                    /* Otherwise, resurface the error code. */
                    aws_raise_error(error_code);
                }
                break;
            }

            AWS_ASSERT(request->send_data.response_headers != NULL);

            /* Parse the object size from the part response. */
            if (aws_s3_parse_content_range_response_header(
                    meta_request->allocator,
                    request->send_data.response_headers,
                    &object_range_start,
                    &object_range_end,
                    &object_size)) {

                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p Could not find content-range header for request %p",
                    (void *)meta_request,
                    (void *)request);

                break;
            }
            if (auto_ranged_get->initial_message_has_range_header) {
                if (auto_ranged_get->initial_message_has_end_range) {
                    object_range_end = aws_min_u64(object_size - 1, auto_ranged_get->initial_range_end);
                } else {
                    object_range_end = object_size - 1;
                }
            } else {
                /* When discovering the object size via GET_OBJECT_WITH_RANGE, the object range is the entire object. */
                object_range_start = 0;
                object_range_end = object_size - 1; /* range-end is inclusive */
            }
            result = AWS_OP_SUCCESS;
            break;
        default:
            AWS_ASSERT(false);
            break;
    }

    if (result == AWS_OP_SUCCESS) {
        *out_object_size = object_size;
        *out_object_range_start = object_range_start;
        *out_object_range_end = object_range_end;
        *out_first_part_size = first_part_size;
    }

    return result;
}

static void s_s3_auto_ranged_get_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);
    AWS_PRECONDITION(request);

    struct aws_s3_auto_ranged_get *auto_ranged_get = meta_request->impl;
    AWS_PRECONDITION(auto_ranged_get);

    uint64_t object_range_start = 0ULL;
    uint64_t object_range_end = 0ULL;
    uint64_t object_size = 0ULL;
    uint64_t first_part_size = 0ULL;

    bool found_object_size = false;
    bool request_failed = error_code != AWS_ERROR_SUCCESS;
    bool first_part_size_mismatch = (error_code == AWS_ERROR_S3_INTERNAL_PART_SIZE_MISMATCH_RETRYING_WITH_RANGE);
    bool empty_file_error = false;

    if (request->discovers_object_size) {
        /* Try to discover the object-range and object-size.*/
        if (s_discover_object_range_and_size(
                meta_request,
                request,
                error_code,
                &object_range_start,
                &object_range_end,
                &object_size,
                &first_part_size,
                &empty_file_error)) {

            error_code = aws_last_error_or_unknown();

            goto update_synced_data;
        }
        if ((!request_failed || first_part_size_mismatch) && !auto_ranged_get->initial_message_has_if_match_header) {
            AWS_ASSERT(auto_ranged_get->etag == NULL);
            struct aws_byte_cursor etag_header_value;

            if (aws_http_headers_get(request->send_data.response_headers, g_etag_header_name, &etag_header_value)) {
                aws_raise_error(AWS_ERROR_S3_MISSING_ETAG);
                error_code = AWS_ERROR_S3_MISSING_ETAG;
                goto update_synced_data;
            }

            AWS_LOGF_TRACE(
                AWS_LS_S3_META_REQUEST,
                "id=%p Etag received for the meta request. value is: " PRInSTR "",
                (void *)meta_request,
                AWS_BYTE_CURSOR_PRI(etag_header_value));
            auto_ranged_get->etag = aws_string_new_from_cursor(auto_ranged_get->base.allocator, &etag_header_value);
        }

        /* If we were able to discover the object-range/content length successfully, then any error code that was passed
         * into this function is being handled and does not indicate an overall failure.*/
        error_code = AWS_ERROR_SUCCESS;
        found_object_size = true;

        /* Check for checksums if requested to */
        if (meta_request->checksum_config.validate_response_checksum) {
            if (aws_s3_check_headers_for_checksum(
                    meta_request,
                    request->send_data.response_headers,
                    &meta_request->meta_request_level_running_response_sum,
                    &meta_request->meta_request_level_response_header_checksum,
                    true) != AWS_OP_SUCCESS) {
                error_code = aws_last_error_or_unknown();
                goto update_synced_data;
            }
        }

        if (!empty_file_error && meta_request->headers_callback != NULL) {
            /* Modify the header received to fake the header for the whole meta request. */
            if (request->request_tag == AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE ||
                request->request_tag == AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1) {

                if (auto_ranged_get->initial_message_has_range_header) {
                    /* Populate the header with object_range */
                    char content_range_buffer[64] = "";
                    snprintf(
                        content_range_buffer,
                        sizeof(content_range_buffer),
                        "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64,
                        object_range_start,
                        object_range_end,
                        object_size);
                    aws_http_headers_set(
                        request->send_data.response_headers,
                        g_content_range_header_name,
                        aws_byte_cursor_from_c_str(content_range_buffer));
                } else {
                    /* content range isn't applicable. */
                    aws_http_headers_erase(request->send_data.response_headers, g_content_range_header_name);
                }
            }

            uint64_t content_length = object_size ? object_range_end - object_range_start + 1 : 0;
            char content_length_buffer[64] = "";
            snprintf(content_length_buffer, sizeof(content_length_buffer), "%" PRIu64, content_length);
            aws_http_headers_set(
                request->send_data.response_headers,
                g_content_length_header_name,
                aws_byte_cursor_from_c_str(content_length_buffer));

            if (meta_request->headers_callback(
                    meta_request,
                    request->send_data.response_headers,
                    s_s3_auto_ranged_get_success_status(meta_request),
                    meta_request->user_data)) {

                error_code = aws_last_error_or_unknown();
            }
            meta_request->headers_callback = NULL;
        }
    }

update_synced_data:

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_meta_request_lock_synced_data(meta_request);
        bool finishing_metrics = true;

        /* If the object range was found, then record it. */
        if (found_object_size) {
            AWS_ASSERT(!auto_ranged_get->synced_data.object_range_known);
            auto_ranged_get->synced_data.object_range_known = true;
            auto_ranged_get->synced_data.object_range_empty = (object_size == 0);
            auto_ranged_get->synced_data.object_range_start = object_range_start;
            auto_ranged_get->synced_data.object_range_end = object_range_end;
            if (!first_part_size_mismatch && first_part_size) {
                auto_ranged_get->synced_data.first_part_size = first_part_size;
            }
            if (auto_ranged_get->synced_data.object_range_empty == 0) {
                auto_ranged_get->synced_data.total_num_parts = aws_s3_calculate_auto_ranged_get_num_parts(
                    meta_request->part_size,
                    auto_ranged_get->synced_data.first_part_size,
                    object_range_start,
                    object_range_end);
            }
            
            /* Validate user buffer size if provided */
            if (meta_request->user_buffer_options.transfer_buffer != NULL && 
                meta_request->user_buffer_options.transfer_buffer_size > 0) {
                uint64_t required_size = object_size ? (object_range_end - object_range_start + 1) : 0;
                if (required_size > meta_request->user_buffer_options.transfer_buffer_size) {
                    AWS_LOGF_ERROR(
                        AWS_LS_S3_META_REQUEST,
                        "id=%p User buffer too small for GET object. Required: %" PRIu64 " bytes, Provided: %zu bytes",
                        (void *)meta_request,
                        required_size,
                        meta_request->user_buffer_options.transfer_buffer_size);
                    error_code = aws_raise_error(AWS_ERROR_S3_INVALID_RESPONSE_STATUS);
                }
            }
        }

        switch (request->request_tag) {
            case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_HEAD_OBJECT:
                auto_ranged_get->synced_data.head_object_completed = true;
                AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Head object completed.", (void *)meta_request);
                break;
            case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1:
                AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Get Part Number completed.", (void *)meta_request);
                if (first_part_size_mismatch && found_object_size) {
                    /* We canceled GET_OBJECT_WITH_PART_NUMBER_1 request because the Content-Length was bigger than
                     * part_size. Try to fetch the first part again as a ranged get */
                    auto_ranged_get->synced_data.num_parts_requested = 0;
                    break;
                }
                /* fall through */
            case AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE:
                if (empty_file_error) {
                    /*
                     * Try to download the object again using GET_OBJECT_WITH_PART_NUMBER_1. If the file is still
                     * empty, successful response headers will be provided to users. If not, the newer version of the
                     * file will be downloaded.
                     */
                    auto_ranged_get->synced_data.num_parts_requested = 0;
                    auto_ranged_get->synced_data.object_range_known = 0;
                    break;
                }

                ++auto_ranged_get->synced_data.num_parts_completed;

                if (!request_failed) {

                    /* Record the number of parts that checksum has been validated */
                    if (request->did_validate) {
                        if (auto_ranged_get->validation_algorithm == AWS_SCA_NONE) {
                            auto_ranged_get->validation_algorithm = request->validation_algorithm;
                        }
                        /* They should be the same. */
                        AWS_ASSERT(auto_ranged_get->validation_algorithm == request->validation_algorithm);
                        ++auto_ranged_get->synced_data.num_parts_checksum_validated;
                    }
                    ++auto_ranged_get->synced_data.num_parts_successful;

                    /* Send progress_callback for delivery on io_event_loop thread */
                    if (meta_request->progress_callback != NULL) {
                        struct aws_s3_meta_request_event event = {.type = AWS_S3_META_REQUEST_EVENT_PROGRESS};
                        event.u.progress.info.bytes_transferred = request->send_data.response_body.len;
                        if (auto_ranged_get->synced_data.object_range_empty) {
                            event.u.progress.info.content_length = 0;
                        } else {
                            /* Note that range-end is inclusive */
                            event.u.progress.info.content_length = auto_ranged_get->synced_data.object_range_end + 1 -
                                                                   auto_ranged_get->synced_data.object_range_start;
                        }
                        aws_s3_meta_request_add_event_for_delivery_synced(meta_request, &event);
                    }

                    aws_s3_meta_request_stream_response_body_synced(meta_request, request);
                    /* The body of the request is queued to be streamed, don't finish the metrics yet. */
                    finishing_metrics = false;

                    AWS_LOGF_DEBUG(
                        AWS_LS_S3_META_REQUEST,
                        "id=%p: %d out of %d parts have completed.",
                        (void *)meta_request,
                        (auto_ranged_get->synced_data.num_parts_successful +
                         auto_ranged_get->synced_data.num_parts_failed),
                        auto_ranged_get->synced_data.total_num_parts);
                } else {
                    ++auto_ranged_get->synced_data.num_parts_failed;
                }
                break;
        }

        if (error_code != AWS_ERROR_SUCCESS) {
            if (error_code == AWS_ERROR_S3_INVALID_RESPONSE_STATUS &&
                request->send_data.response_status == AWS_HTTP_STATUS_CODE_412_PRECONDITION_FAILED &&
                !auto_ranged_get->initial_message_has_if_match_header) {
                /* Use more clear error code as we added the if-match header under the hood. */
                error_code = AWS_ERROR_S3_OBJECT_MODIFIED;
            }
            aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
            if (error_code == AWS_ERROR_S3_RESPONSE_CHECKSUM_MISMATCH) {
                /* It's a mismatch of checksum, tell user that we validated the checksum and the algorithm we validated
                 */
                meta_request->synced_data.finish_result.did_validate = true;
                meta_request->synced_data.finish_result.validation_algorithm = request->validation_algorithm;
            }
        }
        if (finishing_metrics) {
            aws_s3_request_finish_up_metrics_synced(request, meta_request);
        }
        aws_s3_meta_request_unlock_synced_data(meta_request);
    }
    /* END CRITICAL SECTION */
}
