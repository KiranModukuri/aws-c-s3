/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
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


#include "s3_tester.h"
#include <aws/s3/s3_client.h>
#include <aws/common/byte_buf.h>
#include <aws/common/clock.h>
#include <aws/testing/aws_test_harness.h>
#include <inttypes.h>
#include <string.h>

/**
 * Basic User Buffer Tests (no explicit RDMA flag)
 * 
 * These tests exercise user-provided buffers for S3 transfers without
 * explicitly setting use_rdma. The library will use RDMA if available
 * and fall back to HTTP if not.
 */

/* Test: Single-part GET with user buffer */
AWS_TEST_CASE(test_s3_get_object_user_buffer_single_part, s_test_s3_get_object_user_buffer_single_part)
static int s_test_s3_get_object_user_buffer_single_part(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 20 * 1024 * 1024,
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    size_t object_size = 1 * 1024 * 1024; // 1MB
    struct aws_byte_buf user_buffer;
    aws_byte_buf_init(&user_buffer, allocator, object_size);

    struct aws_s3_user_buffer_options user_buffer_options = {
        .transfer_buffer = user_buffer.buffer,
        .transfer_buffer_size = user_buffer.capacity,
    };

    struct aws_s3_tester_meta_request_options options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .client = client,
        .get_options =
            {
                .object_path = aws_byte_cursor_from_c_str("/pre-existing-1MB"),
            },
        .user_buffer_options = &user_buffer_options,
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &options, NULL));

    aws_byte_buf_clean_up(&user_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

/* Test: Multipart GET with user buffer */
AWS_TEST_CASE(test_s3_get_object_user_buffer_multipart, s_test_s3_get_object_user_buffer_multipart)
static int s_test_s3_get_object_user_buffer_multipart(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 5 * 1024 * 1024,
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    size_t object_size = 10 * 1024 * 1024; // 10MB - 2 parts
    struct aws_byte_buf user_buffer;
    aws_byte_buf_init(&user_buffer, allocator, object_size);

    struct aws_s3_user_buffer_options user_buffer_options = {
        .transfer_buffer = user_buffer.buffer,
        .transfer_buffer_size = user_buffer.capacity,
    };

    struct aws_s3_tester_meta_request_options options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .client = client,
        .get_options =
            {
                .object_path = aws_byte_cursor_from_c_str("/pre-existing-10MB"),
            },
        .user_buffer_options = &user_buffer_options,
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &options, NULL));

    aws_byte_buf_clean_up(&user_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

/**
 * Test: GET with user buffer SMALLER than part_size
 * This is the actual bug scenario - when buffer < part_size, the discovery
 * request asks for part_size bytes which exceeds the buffer, potentially
 * causing data to go to pool buffer instead of user buffer.
 */
AWS_TEST_CASE(test_s3_get_user_buffer_smaller_than_part_size, s_test_s3_get_user_buffer_smaller_than_part_size)
static int s_test_s3_get_user_buffer_smaller_than_part_size(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 20 * 1024 * 1024, /* 20MB - larger than buffer! */
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    size_t object_size = 10 * 1024 * 1024; /* 10MB object */
    size_t buffer_size = 5 * 1024 * 1024;  /* 5MB buffer - SMALLER than part_size! */

    /* First, upload a 10MB file with known pattern */
    struct aws_byte_buf put_buffer;
    aws_byte_buf_init(&put_buffer, allocator, object_size);
    for (size_t i = 0; i < object_size; ++i) {
        put_buffer.buffer[i] = (uint8_t)(i % 256);
    }
    put_buffer.len = object_size;

    uint64_t timestamp;
    aws_high_res_clock_get_ticks(&timestamp);
    char object_path_buffer[128];
    snprintf(object_path_buffer, sizeof(object_path_buffer),
             "/small_buffer_test_%llu.txt", (unsigned long long)timestamp);

    struct aws_http_message *put_message = aws_s3_test_put_object_request_new_without_body(
        allocator,
        &g_test_endpoint,
        g_test_body_content_type,
        aws_byte_cursor_from_c_str(object_path_buffer),
        object_size,
        0);

    struct aws_s3_user_buffer_options put_user_buffer_options = {
        .transfer_buffer = put_buffer.buffer,
        .transfer_buffer_size = put_buffer.len,
    };

    struct aws_s3_tester_meta_request_options put_options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT,
        .message = put_message,
        .client = client,
        .user_buffer_options = &put_user_buffer_options,
        .use_rdma = true,
        .validate_type = AWS_S3_TESTER_VALIDATE_TYPE_NO_VALIDATE, /* Server doesn't return ETag for RDMA */
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &put_options, NULL));
    aws_http_message_release(put_message);

    /* Now GET with a buffer SMALLER than part_size but EQUAL to object size
     * This tests the core fix: part_size=20MB, buffer=10MB, object=10MB
     * Without fix: discovery asks for 20MB via RDMA with 20MB pool buffer
     * With fix: discovery asks for 10MB (clamped to user buffer), uses user buffer
     *
     * We use object_size == buffer_size so the full download succeeds,
     * but the fix is needed to use the correct buffer for RDMA.
     */
    struct aws_byte_buf get_buffer;
    aws_byte_buf_init(&get_buffer, allocator, object_size); /* Buffer = object size = 10MB */

    /* Poison buffer to detect if data was NOT written */
    memset(get_buffer.buffer, 0xDE, object_size);

    struct aws_s3_user_buffer_options get_user_buffer_options = {
        .transfer_buffer = get_buffer.buffer,
        .transfer_buffer_size = get_buffer.capacity,
    };

    struct aws_s3_meta_request_test_results test_results;
    aws_s3_meta_request_test_results_init(&test_results, allocator);

    struct aws_s3_tester_meta_request_options get_options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .client = client,
        .get_options =
            {
                .object_path = aws_byte_cursor_from_c_str(object_path_buffer),
            },
        .user_buffer_options = &get_user_buffer_options,
        .use_rdma = true,
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &get_options, &test_results));

    /* Verify all data matches what we uploaded */
    ASSERT_UINT_EQUALS(object_size, test_results.received_body_size);
    ASSERT_TRUE(memcmp(get_buffer.buffer, put_buffer.buffer, object_size) == 0);

    aws_s3_meta_request_test_results_clean_up(&test_results);
    aws_byte_buf_clean_up(&put_buffer);
    aws_byte_buf_clean_up(&get_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

/**
 * Test: GET with user buffer LARGER than part_size but NOT a multiple
 * Tests edge case: part_size=5MB, buffer=12MB, object=12MB (2.4 parts)
 */
AWS_TEST_CASE(test_s3_get_user_buffer_non_multiple_of_part_size, s_test_s3_get_user_buffer_non_multiple_of_part_size)
static int s_test_s3_get_user_buffer_non_multiple_of_part_size(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 5 * 1024 * 1024, /* 5MB parts */
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    size_t object_size = 12 * 1024 * 1024; /* 12MB = 2.4 parts (5+5+2) */

    /* First, upload a 12MB file with known pattern */
    struct aws_byte_buf put_buffer;
    aws_byte_buf_init(&put_buffer, allocator, object_size);
    for (size_t i = 0; i < object_size; ++i) {
        put_buffer.buffer[i] = (uint8_t)(i % 256);
    }
    put_buffer.len = object_size;

    uint64_t timestamp;
    aws_high_res_clock_get_ticks(&timestamp);
    char object_path_buffer[128];
    snprintf(object_path_buffer, sizeof(object_path_buffer),
             "/non_multiple_test_%llu.txt", (unsigned long long)timestamp);

    struct aws_http_message *put_message = aws_s3_test_put_object_request_new_without_body(
        allocator,
        &g_test_endpoint,
        g_test_body_content_type,
        aws_byte_cursor_from_c_str(object_path_buffer),
        object_size,
        0);

    struct aws_s3_user_buffer_options put_user_buffer_options = {
        .transfer_buffer = put_buffer.buffer,
        .transfer_buffer_size = put_buffer.len,
    };

    struct aws_s3_tester_meta_request_options put_options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT,
        .message = put_message,
        .client = client,
        .user_buffer_options = &put_user_buffer_options,
        .use_rdma = true,
        .validate_type = AWS_S3_TESTER_VALIDATE_TYPE_NO_VALIDATE, /* Server doesn't return ETag for RDMA */
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &put_options, NULL));
    aws_http_message_release(put_message);

    /* GET with buffer = object_size = 12MB (not multiple of 5MB part_size) */
    struct aws_byte_buf get_buffer;
    aws_byte_buf_init(&get_buffer, allocator, object_size);
    memset(get_buffer.buffer, 0xDE, object_size);

    struct aws_s3_user_buffer_options get_user_buffer_options = {
        .transfer_buffer = get_buffer.buffer,
        .transfer_buffer_size = get_buffer.capacity,
    };

    struct aws_s3_meta_request_test_results test_results;
    aws_s3_meta_request_test_results_init(&test_results, allocator);

    struct aws_s3_tester_meta_request_options get_options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .client = client,
        .get_options =
            {
                .object_path = aws_byte_cursor_from_c_str(object_path_buffer),
            },
        .user_buffer_options = &get_user_buffer_options,
        .use_rdma = true,
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &get_options, &test_results));

    /* Verify all data matches */
    ASSERT_UINT_EQUALS(object_size, test_results.received_body_size);
    ASSERT_TRUE(memcmp(get_buffer.buffer, put_buffer.buffer, object_size) == 0);

    aws_s3_meta_request_test_results_clean_up(&test_results);
    aws_byte_buf_clean_up(&put_buffer);
    aws_byte_buf_clean_up(&get_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

/* Test: Single-part PUT with user buffer and RDMA */
AWS_TEST_CASE(test_s3_put_user_buffer_rdma_single_part, s_test_s3_put_user_buffer_rdma_single_part)
static int s_test_s3_put_user_buffer_rdma_single_part(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 20 * 1024 * 1024,
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    /* Allocate and fill user buffer (1MB) */
    size_t object_size = 1 * 1024 * 1024;
    struct aws_byte_buf user_buffer;
    aws_byte_buf_init(&user_buffer, allocator, object_size);
    
    for (size_t i = 0; i < object_size; ++i) {
        user_buffer.buffer[i] = (uint8_t)(i % 256);
    }
    user_buffer.len = object_size;

    uint64_t timestamp;
    aws_high_res_clock_get_ticks(&timestamp);
    char object_path_buffer[128];
    snprintf(object_path_buffer, sizeof(object_path_buffer),
             "/put_rdma_user_buffer_1MB_%llu.txt",
             (unsigned long long)timestamp);

    /* Create HTTP message without Content-Length - will be derived from transfer_buffer_size */
    struct aws_http_message *message = aws_s3_test_put_object_request_new_without_body(
        allocator,
        &g_test_endpoint,
        g_test_body_content_type,
        aws_byte_cursor_from_c_str(object_path_buffer),
        0,  /* No content-length - derived from user_buffer_options */
        0);

    struct aws_s3_user_buffer_options user_buffer_options = {
        .transfer_buffer = user_buffer.buffer,
        .transfer_buffer_size = user_buffer.len,
    };

    struct aws_s3_tester_meta_request_options options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT,
        .message = message,
        .client = client,
        .user_buffer_options = &user_buffer_options,
        .use_rdma = true,
        .validate_type = AWS_S3_TESTER_VALIDATE_TYPE_NO_VALIDATE,  /* Skip ETag check for RDMA - server may not return it */
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &options, NULL));

    aws_http_message_release(message);

    aws_byte_buf_clean_up(&user_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

/* Test: Multipart PUT with user buffer and RDMA */
AWS_TEST_CASE(test_s3_put_user_buffer_rdma_multipart, s_test_s3_put_user_buffer_rdma_multipart)
static int s_test_s3_put_user_buffer_rdma_multipart(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 5 * 1024 * 1024,
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    /* Allocate and fill user buffer (10MB - 2 parts) */
    size_t object_size = 10 * 1024 * 1024;
    struct aws_byte_buf user_buffer;
    aws_byte_buf_init(&user_buffer, allocator, object_size);
    
    for (size_t i = 0; i < object_size; ++i) {
        user_buffer.buffer[i] = (uint8_t)(i % 256);
    }
    user_buffer.len = object_size;

    uint64_t timestamp;
    aws_high_res_clock_get_ticks(&timestamp);
    char object_path_buffer[128];
    snprintf(object_path_buffer, sizeof(object_path_buffer),
             "/put_rdma_user_buffer_10MB_%llu.txt",
             (unsigned long long)timestamp);

    /* Create HTTP message without Content-Length - will be derived from transfer_buffer_size */
    struct aws_http_message *message = aws_s3_test_put_object_request_new_without_body(
        allocator,
        &g_test_endpoint,
        g_test_body_content_type,
        aws_byte_cursor_from_c_str(object_path_buffer),
        0,  /* No content-length - derived from user_buffer_options */
        0);

    struct aws_s3_user_buffer_options user_buffer_options = {
        .transfer_buffer = user_buffer.buffer,
        .transfer_buffer_size = user_buffer.len,
    };

    struct aws_s3_tester_meta_request_options options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT,
        .message = message,
        .client = client,
        .user_buffer_options = &user_buffer_options,
        .use_rdma = true,
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &options, NULL));

    aws_http_message_release(message);

    aws_byte_buf_clean_up(&user_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

/* Test: PUT with user buffer, RDMA, and CRC32 checksum */
AWS_TEST_CASE(test_s3_put_user_buffer_rdma_crc32, s_test_s3_put_user_buffer_rdma_crc32)
static int s_test_s3_put_user_buffer_rdma_crc32(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 20 * 1024 * 1024,
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    size_t object_size = 1 * 1024 * 1024;
    struct aws_byte_buf user_buffer;
    aws_byte_buf_init(&user_buffer, allocator, object_size);
    
    for (size_t i = 0; i < object_size; ++i) {
        user_buffer.buffer[i] = (uint8_t)(i % 256);
    }
    user_buffer.len = object_size;

    uint64_t timestamp;
    aws_high_res_clock_get_ticks(&timestamp);
    char object_path_buffer[128];
    snprintf(object_path_buffer, sizeof(object_path_buffer),
             "/put_rdma_crc32_1MB_%llu.txt",
             (unsigned long long)timestamp);

    /* Create HTTP message without Content-Length - will be derived from transfer_buffer_size */
    struct aws_http_message *message = aws_s3_test_put_object_request_new_without_body(
        allocator,
        &g_test_endpoint,
        g_test_body_content_type,
        aws_byte_cursor_from_c_str(object_path_buffer),
        0,  /* No content-length - derived from user_buffer_options */
        0);

    struct aws_s3_user_buffer_options user_buffer_options = {
        .transfer_buffer = user_buffer.buffer,
        .transfer_buffer_size = user_buffer.len,
    };

    struct aws_s3_tester_meta_request_options options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT,
        .message = message,
        .client = client,
        .user_buffer_options = &user_buffer_options,
        .use_rdma = true,
        .checksum_algorithm = AWS_SCA_CRC32,
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &options, NULL));

    aws_http_message_release(message);

    aws_byte_buf_clean_up(&user_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

/* Test: Multipart PUT with user buffer, RDMA, and CRC32C checksum */
AWS_TEST_CASE(test_s3_put_user_buffer_rdma_crc32c_multipart, s_test_s3_put_user_buffer_rdma_crc32c_multipart)
static int s_test_s3_put_user_buffer_rdma_crc32c_multipart(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 5 * 1024 * 1024,
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    size_t object_size = 10 * 1024 * 1024;
    struct aws_byte_buf user_buffer;
    aws_byte_buf_init(&user_buffer, allocator, object_size);
    
    for (size_t i = 0; i < object_size; ++i) {
        user_buffer.buffer[i] = (uint8_t)(i % 256);
    }
    user_buffer.len = object_size;

    uint64_t timestamp;
    aws_high_res_clock_get_ticks(&timestamp);
    char object_path_buffer[128];
    snprintf(object_path_buffer, sizeof(object_path_buffer),
             "/put_rdma_crc32c_10MB_%llu.txt",
             (unsigned long long)timestamp);

    /* Create HTTP message without Content-Length - will be derived from transfer_buffer_size */
    struct aws_http_message *message = aws_s3_test_put_object_request_new_without_body(
        allocator,
        &g_test_endpoint,
        g_test_body_content_type,
        aws_byte_cursor_from_c_str(object_path_buffer),
        0,  /* No content-length - derived from user_buffer_options */
        0);

    struct aws_s3_user_buffer_options user_buffer_options = {
        .transfer_buffer = user_buffer.buffer,
        .transfer_buffer_size = user_buffer.len,
    };

    struct aws_s3_tester_meta_request_options options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT,
        .message = message,
        .client = client,
        .user_buffer_options = &user_buffer_options,
        .use_rdma = true,
        .checksum_algorithm = AWS_SCA_CRC32C,
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &options, NULL));

    aws_http_message_release(message);

    aws_byte_buf_clean_up(&user_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

/* Test: GET with user buffer and checksum validation */
AWS_TEST_CASE(test_s3_get_user_buffer_checksum_validate, s_test_s3_get_user_buffer_checksum_validate)
static int s_test_s3_get_user_buffer_checksum_validate(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 20 * 1024 * 1024,
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    size_t object_size = 1 * 1024 * 1024;
    struct aws_byte_buf user_buffer;
    aws_byte_buf_init(&user_buffer, allocator, object_size);

    struct aws_s3_user_buffer_options user_buffer_options = {
        .transfer_buffer = user_buffer.buffer,
        .transfer_buffer_size = user_buffer.capacity,
    };

    struct aws_s3_tester_meta_request_options options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .client = client,
        .get_options =
            {
                .object_path = aws_byte_cursor_from_c_str("/pre-existing-1MB"),
            },
        .user_buffer_options = &user_buffer_options,
        .validate_get_response_checksum = true,
        .use_rdma = true,
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &options, NULL));

    aws_byte_buf_clean_up(&user_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

/* Test: Round-trip with user buffer (PUT + GET) */
AWS_TEST_CASE(test_s3_user_buffer_put_get_roundtrip, s_test_s3_user_buffer_put_get_roundtrip)
static int s_test_s3_user_buffer_put_get_roundtrip(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 5 * 1024 * 1024,
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    /* Allocate and fill PUT buffer (10MB) */
    size_t object_size = 10 * 1024 * 1024;
    struct aws_byte_buf put_buffer;
    aws_byte_buf_init(&put_buffer, allocator, object_size);
    
    for (size_t i = 0; i < object_size; ++i) {
        put_buffer.buffer[i] = (uint8_t)((i * 13 + 7) % 256);
    }
    put_buffer.len = object_size;

    uint64_t timestamp;
    aws_high_res_clock_get_ticks(&timestamp);
    char object_path_buffer[128];
    snprintf(object_path_buffer, sizeof(object_path_buffer),
             "/roundtrip_10MB_%llu.txt",
             (unsigned long long)timestamp);

    /* PUT */
    /* Create HTTP message with Content-Length header (standard approach) */
    struct aws_http_message *put_message = aws_s3_test_put_object_request_new_without_body(
        allocator,
        &g_test_endpoint,
        g_test_body_content_type,
        aws_byte_cursor_from_c_str(object_path_buffer),
        object_size,
        0);

    struct aws_s3_user_buffer_options put_user_buffer_options = {
        .transfer_buffer = put_buffer.buffer,
        .transfer_buffer_size = put_buffer.len,
    };

    struct aws_s3_tester_meta_request_options put_options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT,
        .message = put_message,
        .client = client,
        .user_buffer_options = &put_user_buffer_options,
        .use_rdma = true,
        .checksum_algorithm = AWS_SCA_CRC32,
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &put_options, NULL));

    aws_http_message_release(put_message);

    /* GET */
    struct aws_byte_buf get_buffer;
    aws_byte_buf_init(&get_buffer, allocator, object_size);

    /* Poison buffer to verify data actually gets written */
    memset(get_buffer.buffer, 0xDE, object_size);

    struct aws_s3_user_buffer_options get_user_buffer_options = {
        .transfer_buffer = get_buffer.buffer,
        .transfer_buffer_size = get_buffer.capacity,
    };

    struct aws_s3_meta_request_test_results test_results;
    aws_s3_meta_request_test_results_init(&test_results, allocator);

    struct aws_s3_tester_meta_request_options get_options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .client = client,
        .get_options =
            {
                .object_path = aws_byte_cursor_from_c_str(object_path_buffer),
            },
        .user_buffer_options = &get_user_buffer_options,
        .use_rdma = true,
        .validate_get_response_checksum = true,
    };

    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &get_options, &test_results));

    /* Verify data integrity */
    ASSERT_UINT_EQUALS(object_size, test_results.received_body_size);
    ASSERT_TRUE(memcmp(get_buffer.buffer, put_buffer.buffer, object_size) == 0);

    aws_s3_meta_request_test_results_clean_up(&test_results);
    aws_byte_buf_clean_up(&put_buffer);
    aws_byte_buf_clean_up(&get_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

/* Test: User buffer too small error */
AWS_TEST_CASE(test_s3_user_buffer_too_small_error, s_test_s3_user_buffer_too_small_error)
static int s_test_s3_user_buffer_too_small_error(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    struct aws_s3_tester tester;
    ASSERT_SUCCESS(aws_s3_tester_init(allocator, &tester));

    struct aws_s3_client_config client_config = {
        .part_size = 20 * 1024 * 1024,
    };

    ASSERT_SUCCESS(aws_s3_tester_bind_client(
        &tester, &client_config, AWS_S3_TESTER_BIND_CLIENT_REGION | AWS_S3_TESTER_BIND_CLIENT_SIGNING));

    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);
    ASSERT_TRUE(client != NULL);

    /* Buffer too small for 1MB object */
    size_t buffer_size = 512 * 1024; // 512KB
    struct aws_byte_buf user_buffer;
    aws_byte_buf_init(&user_buffer, allocator, buffer_size);

    struct aws_s3_user_buffer_options user_buffer_options = {
        .transfer_buffer = user_buffer.buffer,
        .transfer_buffer_size = user_buffer.capacity,
    };

    struct aws_s3_tester_meta_request_options options = {
        .allocator = allocator,
        .meta_request_type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT,
        .client = client,
        .validate_type = AWS_S3_TESTER_VALIDATE_TYPE_EXPECT_FAILURE,
        .get_options =
            {
                .object_path = aws_byte_cursor_from_c_str("/pre-existing-1MB"),
            },
        .user_buffer_options = &user_buffer_options,
    };

    /* Should fail due to buffer size */
    ASSERT_SUCCESS(aws_s3_tester_send_meta_request_with_options(&tester, &options, NULL));

    aws_byte_buf_clean_up(&user_buffer);
    client = aws_s3_client_release(client);
    aws_s3_tester_clean_up(&tester);

    return 0;
}

