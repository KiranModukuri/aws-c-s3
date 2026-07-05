/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "s3_tester.h"

#include <aws/http/request_response.h>
#include <aws/s3/private/s3_client_impl.h>
#include <aws/s3/private/s3_meta_request_impl.h>
#include <aws/s3/private/s3_rdma_provider_impl.h>
#include <aws/s3/private/s3_request.h>
#include <aws/s3/s3_rdma_provider.h>
#include <aws/testing/aws_test_harness.h>

#include <stdint.h>

struct s_test_rdma_provider_state {
    bool memory_suitable;
    bool token_generation_fails;
    void *pre_registered_base;
    size_t pre_registered_size;
    size_t register_count;
    size_t deregister_count;
    size_t pre_register_count;
    size_t release_count;
    size_t prepare_get_token_count;
    size_t prepare_put_token_count;
};

static void s_test_provider_destroy(void *user_data) {
    (void)user_data;
}

static struct s_test_rdma_provider_state *s_test_state(struct aws_s3_rdma_provider *provider) {
    return (struct s_test_rdma_provider_state *)provider;
}

static bool s_test_slice_is_pre_registered(
    const struct s_test_rdma_provider_state *state,
    const void *ptr,
    size_t size) {

    if (!state->pre_registered_base || !ptr || size == 0) {
        return false;
    }

    uintptr_t base = (uintptr_t)state->pre_registered_base;
    uintptr_t slice = (uintptr_t)ptr;
    return slice >= base && slice - base <= state->pre_registered_size &&
           size <= state->pre_registered_size - (size_t)(slice - base);
}

static int s_test_register_memory(struct aws_s3_rdma_provider *provider, void *ptr, size_t size) {
    (void)ptr;
    (void)size;

    ++s_test_state(provider)->register_count;
    return AWS_OP_SUCCESS;
}

static int s_test_deregister_memory(struct aws_s3_rdma_provider *provider, void *ptr) {
    (void)ptr;

    ++s_test_state(provider)->deregister_count;
    return AWS_OP_SUCCESS;
}

static int s_test_pre_register_memory(struct aws_s3_rdma_provider *provider, void *base, size_t size) {
    struct s_test_rdma_provider_state *state = s_test_state(provider);
    ++state->pre_register_count;
    state->pre_registered_base = base;
    state->pre_registered_size = size;
    return AWS_OP_SUCCESS;
}

static int s_test_release_memory(struct aws_s3_rdma_provider *provider, void *base) {
    struct s_test_rdma_provider_state *state = s_test_state(provider);
    ++state->release_count;
    if (state->pre_registered_base == base) {
        state->pre_registered_base = NULL;
        state->pre_registered_size = 0;
    }
    return AWS_OP_SUCCESS;
}

static bool s_test_is_slice_pre_registered(struct aws_s3_rdma_provider *provider, const void *ptr, size_t size) {
    return s_test_slice_is_pre_registered(s_test_state(provider), ptr, size);
}

static bool s_test_is_memory_suitable(struct aws_s3_rdma_provider *provider, const void *ptr, size_t size) {
    (void)ptr;
    (void)size;

    return s_test_state(provider)->memory_suitable;
}

static size_t s_test_get_max_transfer_size(struct aws_s3_rdma_provider *provider, const void *ptr) {
    (void)provider;
    (void)ptr;

    return SIZE_MAX;
}

static int s_test_prepare_get_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *s3_key,
    void *buffer,
    size_t size,
    size_t offset,
    struct aws_byte_cursor *out_rdma_token) {

    (void)s3_key;
    (void)buffer;
    (void)size;
    (void)offset;

    struct s_test_rdma_provider_state *state = s_test_state(provider);
    ++state->prepare_get_token_count;
    if (state->token_generation_fails) {
        return aws_raise_error(AWS_ERROR_S3_RDMA_INVALID_TOKEN);
    }

    *out_rdma_token = aws_byte_cursor_from_c_str("test-token");
    return AWS_OP_SUCCESS;
}

static int s_test_prepare_put_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *s3_key,
    const void *buffer,
    size_t size,
    size_t offset,
    struct aws_byte_cursor *out_rdma_token) {

    (void)s3_key;
    (void)buffer;
    (void)size;
    (void)offset;

    struct s_test_rdma_provider_state *state = s_test_state(provider);
    ++state->prepare_put_token_count;
    if (state->token_generation_fails) {
        return aws_raise_error(AWS_ERROR_S3_RDMA_INVALID_TOKEN);
    }

    *out_rdma_token = aws_byte_cursor_from_c_str("test-token");
    return AWS_OP_SUCCESS;
}

static int s_test_process_reply_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *reply_token,
    void *user_data,
    aws_s3_rdma_completion_fn *completion_callback) {

    (void)provider;
    (void)reply_token;
    (void)user_data;
    (void)completion_callback;

    return AWS_OP_SUCCESS;
}

static struct aws_byte_cursor s_test_get_rdma_token_header_name(struct aws_s3_rdma_provider *provider) {
    (void)provider;
    return aws_byte_cursor_from_c_str("x-rdma-token");
}

static struct aws_byte_cursor s_test_get_rdma_reply_header_name(struct aws_s3_rdma_provider *provider) {
    (void)provider;
    return aws_byte_cursor_from_c_str("x-rdma-reply");
}

static struct aws_byte_cursor s_test_get_rdma_bytes_header_name(struct aws_s3_rdma_provider *provider) {
    (void)provider;
    return aws_byte_cursor_from_c_str("x-rdma-bytes");
}

static const struct aws_s3_rdma_provider_vtable s_test_rdma_provider_vtable = {
    .provider_name = "test-rdma-provider",
    .provider_version = AWS_S3_RDMA_PROVIDER_VERSION_2,
    .register_memory = s_test_register_memory,
    .deregister_memory = s_test_deregister_memory,
    .is_memory_suitable = s_test_is_memory_suitable,
    .get_max_transfer_size = s_test_get_max_transfer_size,
    .prepare_put_token = s_test_prepare_put_token,
    .prepare_get_token = s_test_prepare_get_token,
    .process_reply_token = s_test_process_reply_token,
    .get_rdma_token_header_name = s_test_get_rdma_token_header_name,
    .get_rdma_reply_header_name = s_test_get_rdma_reply_header_name,
    .get_rdma_bytes_header_name = s_test_get_rdma_bytes_header_name,
    .pre_register_memory = s_test_pre_register_memory,
    .release_memory = s_test_release_memory,
    .is_slice_pre_registered = s_test_is_slice_pre_registered,
};

static void s_test_provider_init(
    struct aws_allocator *allocator,
    struct aws_s3_rdma_provider *provider,
    struct s_test_rdma_provider_state *state) {

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*state);

    state->memory_suitable = true;

    provider->allocator = allocator;
    provider->vtable = &s_test_rdma_provider_vtable;
    provider->provider_instance = state;
    aws_ref_count_init(&provider->ref_count, provider, s_test_provider_destroy);
}

static struct aws_http_message *s_test_http_request_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor method) {

    struct aws_http_message *message = aws_http_message_new_request(allocator);
    aws_http_message_set_request_method(message, method);
    aws_http_message_set_request_path(message, aws_byte_cursor_from_c_str("/bucket/key"));
    return message;
}

AWS_TEST_CASE(
    test_s3_rdma_buffer_manager_handles_pre_registered_slice,
    s_test_s3_rdma_buffer_manager_handles_pre_registered_slice)
static int s_test_s3_rdma_buffer_manager_handles_pre_registered_slice(
    struct aws_allocator *allocator,
    void *ctx) {

    (void)ctx;

    struct s_test_rdma_provider_state state;
    struct aws_s3_rdma_provider provider;
    s_test_provider_init(allocator, &provider, &state);

    uint8_t buffer[64] = {0};
    state.pre_registered_base = buffer;
    state.pre_registered_size = sizeof(buffer);

    struct aws_s3_rdma_buffer_manager *manager = NULL;
    ASSERT_SUCCESS(aws_s3_rdma_buffer_manager_new(allocator, &provider, &manager));

    struct aws_s3_request request;
    AWS_ZERO_STRUCT(request);
    request.request_type = AWS_S3_REQUEST_TYPE_GET_OBJECT;
    request.send_data.response_body.buffer = buffer + 16;
    request.send_data.response_body.capacity = 32;

    ASSERT_SUCCESS(aws_s3_rdma_buffer_manager_prepare_buffer_for_rdma(
        manager,
        &request,
        request.send_data.response_body.buffer,
        request.send_data.response_body.capacity));
    ASSERT_TRUE(request.rdma_buffer_registered);
    ASSERT_UINT_EQUALS(0, state.register_count);

    ASSERT_SUCCESS(aws_s3_rdma_buffer_manager_finalize_buffer(manager, &request));
    ASSERT_FALSE(request.rdma_buffer_registered);
    ASSERT_UINT_EQUALS(0, state.deregister_count);

    state.pre_registered_base = NULL;
    state.pre_registered_size = 0;
    AWS_ZERO_STRUCT(request);
    request.request_type = AWS_S3_REQUEST_TYPE_GET_OBJECT;
    request.send_data.response_body.buffer = buffer;
    request.send_data.response_body.capacity = sizeof(buffer);

    ASSERT_SUCCESS(aws_s3_rdma_buffer_manager_prepare_buffer_for_rdma(manager, &request, buffer, sizeof(buffer)));
    ASSERT_TRUE(request.rdma_buffer_registered);
    ASSERT_UINT_EQUALS(1, state.register_count);

    ASSERT_SUCCESS(aws_s3_rdma_buffer_manager_finalize_buffer(manager, &request));
    ASSERT_FALSE(request.rdma_buffer_registered);
    ASSERT_UINT_EQUALS(1, state.deregister_count);

    aws_s3_rdma_buffer_manager_release(manager);
    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(
    test_s3_client_rdma_pre_register_api_forwards_to_provider,
    s_test_s3_client_rdma_pre_register_api_forwards_to_provider)
static int s_test_s3_client_rdma_pre_register_api_forwards_to_provider(
    struct aws_allocator *allocator,
    void *ctx) {

    (void)ctx;

    struct s_test_rdma_provider_state state;
    struct aws_s3_rdma_provider provider;
    s_test_provider_init(allocator, &provider, &state);

    uint8_t buffer[64] = {0};
    struct aws_s3_client client = {
        .rdma_provider = &provider,
        .enable_rdma = true,
    };

    ASSERT_SUCCESS(aws_s3_client_pre_register_rdma_buffer(&client, buffer, sizeof(buffer)));
    ASSERT_UINT_EQUALS(1, state.pre_register_count);
    ASSERT_TRUE(buffer == state.pre_registered_base);
    ASSERT_UINT_EQUALS(sizeof(buffer), state.pre_registered_size);

    ASSERT_SUCCESS(aws_s3_client_release_rdma_buffer(&client, buffer));
    ASSERT_UINT_EQUALS(1, state.release_count);
    ASSERT_NULL(state.pre_registered_base);

    struct aws_s3_rdma_provider_vtable v1_vtable = s_test_rdma_provider_vtable;
    v1_vtable.provider_version = AWS_S3_RDMA_PROVIDER_VERSION_1;
    provider.vtable = &v1_vtable;
    ASSERT_ERROR(AWS_ERROR_UNIMPLEMENTED, aws_s3_client_pre_register_rdma_buffer(&client, buffer, sizeof(buffer)));
    ASSERT_FALSE(aws_s3_rdma_provider_is_slice_pre_registered(&provider, buffer, sizeof(buffer)));

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(
    test_s3_rdma_request_handler_requires_complete_user_get,
    s_test_s3_rdma_request_handler_requires_complete_user_get)
static int s_test_s3_rdma_request_handler_requires_complete_user_get(
    struct aws_allocator *allocator,
    void *ctx) {

    (void)ctx;

    struct s_test_rdma_provider_state state;
    struct aws_s3_rdma_provider provider;
    s_test_provider_init(allocator, &provider, &state);

    struct aws_s3_rdma_request_handler *handler = NULL;
    ASSERT_SUCCESS(aws_s3_rdma_request_handler_new(allocator, &provider, &handler));

    struct aws_s3_client client = {.rdma_provider = &provider, .enable_rdma = true};
    struct aws_s3_meta_request meta_request = {.client = &client, .use_rdma = true};
    const char *values[] = {NULL, "invalid", "32", "65", "64"};
    uint8_t buffer[64] = {0};

    for (size_t i = 0; i < AWS_ARRAY_SIZE(values); ++i) {
        struct aws_s3_request request;
        AWS_ZERO_STRUCT(request);
        request.request_type = AWS_S3_REQUEST_TYPE_GET_OBJECT;
        request.is_user_provided_buffer = true;
        request.rdma_buffer_registered = true;
        request.send_data.response_status = AWS_HTTP_STATUS_CODE_206_PARTIAL_CONTENT;
        request.send_data.response_body.buffer = buffer;
        request.send_data.response_body.capacity = sizeof(buffer);

        struct aws_http_header header = {
            .name = aws_byte_cursor_from_c_str("x-rdma-bytes"),
            .value = aws_byte_cursor_from_c_str(values[i] ? values[i] : ""),
        };
        ASSERT_SUCCESS(aws_s3_rdma_request_handler_process_response_headers(
            handler, &meta_request, &request, values[i] ? &header : NULL, values[i] ? 1 : 0));

        if (i + 1 == AWS_ARRAY_SIZE(values)) {
            ASSERT_SUCCESS(aws_s3_rdma_request_handler_validate_content_size(handler, &meta_request, &request));
        } else {
            ASSERT_ERROR(
                AWS_ERROR_S3_RDMA_INVALID_TOKEN,
                aws_s3_rdma_request_handler_validate_content_size(handler, &meta_request, &request));
        }
    }

    aws_s3_rdma_request_handler_release(handler);
    return AWS_OP_SUCCESS;
}

static int s_test_token_failure_case(
    struct aws_allocator *allocator,
    enum aws_s3_request_type request_type,
    bool user_buffer) {

    struct s_test_rdma_provider_state state;
    struct aws_s3_rdma_provider provider;
    s_test_provider_init(allocator, &provider, &state);
    state.token_generation_fails = true;

    uint8_t buffer[64] = {0};
    if (user_buffer) {
        state.pre_registered_base = buffer;
        state.pre_registered_size = sizeof(buffer);
    }

    struct aws_s3_rdma_buffer_manager *manager = NULL;
    ASSERT_SUCCESS(aws_s3_rdma_buffer_manager_new(allocator, &provider, &manager));

    struct aws_s3_rdma_request_handler *handler = NULL;
    ASSERT_SUCCESS(aws_s3_rdma_request_handler_new(allocator, &provider, &handler));

    struct aws_s3_client client = {
        .rdma_provider = &provider,
        .rdma_buffer_manager = manager,
        .rdma_min_transfer_size = 1,
        .enable_rdma = true,
    };
    struct aws_s3_meta_request meta_request = {
        .client = &client,
        .use_rdma = true,
    };

    bool is_get = request_type == AWS_S3_REQUEST_TYPE_GET_OBJECT;
    struct aws_http_message *message =
        s_test_http_request_new(allocator, aws_byte_cursor_from_c_str(is_get ? "GET" : "PUT"));

    struct aws_s3_request request;
    AWS_ZERO_STRUCT(request);
    request.request_type = request_type;
    request.is_user_provided_buffer = user_buffer;
    request.send_data.message = message;
    if (is_get) {
        request.send_data.response_body.buffer = user_buffer ? buffer + 8 : buffer;
        request.send_data.response_body.capacity = user_buffer ? 32 : sizeof(buffer);
    } else {
        request.request_body.buffer = buffer;
        request.request_body.len = sizeof(buffer);
        request.request_body.capacity = sizeof(buffer);
    }

    if (is_get && !user_buffer) {
        ASSERT_SUCCESS(aws_s3_rdma_request_handler_prepare_request(handler, &meta_request, &request));
    } else {
        ASSERT_ERROR(
            AWS_ERROR_S3_RDMA_INVALID_TOKEN,
            aws_s3_rdma_request_handler_prepare_request(handler, &meta_request, &request));
    }
    ASSERT_INT_EQUALS(!user_buffer, request.disable_rdma_on_retry);
    ASSERT_FALSE(request.rdma_buffer_registered);
    ASSERT_UINT_EQUALS(user_buffer ? 0 : 1, state.register_count);
    ASSERT_UINT_EQUALS(user_buffer ? 0 : 1, state.deregister_count);
    ASSERT_UINT_EQUALS(is_get ? 1 : 0, state.prepare_get_token_count);
    ASSERT_UINT_EQUALS(is_get ? 0 : 1, state.prepare_put_token_count);

    aws_http_message_release(message);
    aws_s3_rdma_request_handler_release(handler);
    aws_s3_rdma_buffer_manager_release(manager);
    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(
    test_s3_rdma_request_handler_token_failure_policy,
    s_test_s3_rdma_request_handler_token_failure_policy)
static int s_test_s3_rdma_request_handler_token_failure_policy(
    struct aws_allocator *allocator,
    void *ctx) {

    (void)ctx;
    ASSERT_SUCCESS(s_test_token_failure_case(allocator, AWS_S3_REQUEST_TYPE_GET_OBJECT, false));
    ASSERT_SUCCESS(s_test_token_failure_case(allocator, AWS_S3_REQUEST_TYPE_GET_OBJECT, true));
    ASSERT_SUCCESS(s_test_token_failure_case(allocator, AWS_S3_REQUEST_TYPE_UPLOAD_PART, false));
    return AWS_OP_SUCCESS;
}
