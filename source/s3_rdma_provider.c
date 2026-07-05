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
#include <aws/common/allocator.h>
#include <aws/common/logging.h>
#include <aws/common/string.h>
#include <aws/common/byte_buf.h>
#include <aws/common/ref_count.h>

#include <dlfcn.h>
#include <string.h>

/* RDMA error codes are defined in aws/s3/s3.h and registered by the main S3 library */

static struct aws_log_subject_info s_s3_rdma_log_subject_infos[] = {
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_S3_RDMA, "s3-rdma", "S3 RDMA provider"),
};

static struct aws_log_subject_info_list s_s3_rdma_log_subject_list = {
    .subject_list = s_s3_rdma_log_subject_infos,
    .count = AWS_ARRAY_SIZE(s_s3_rdma_log_subject_infos),
};

/* Track if RDMA library is initialized to avoid logging during cleanup */
static bool s_rdma_library_initialized = false;


static void s_aws_s3_rdma_provider_destroy(void *user_data) {
    struct aws_s3_rdma_provider *provider = user_data;
    
    if (!provider) {
        return;
    }
    
    /* Only log if library is still initialized to avoid crashes during cleanup */
    if (s_rdma_library_initialized) {
        AWS_LOGF_DEBUG(AWS_LS_S3_RDMA, "id=%p: Destroying RDMA provider", (void *)provider);
    }
    
    /* Cleanup provider instance */
    if (provider->vtable && provider->vtable->cleanup && provider->provider_instance) {
        provider->vtable->cleanup(provider->provider_instance);
        provider->provider_instance = NULL;
    }
    
    /* Close plugin handle */
    if (provider->plugin_handle) {
        dlclose(provider->plugin_handle);
        provider->plugin_handle = NULL;
    }
    
    /* Clean up configuration strings - only if they were allocated as byte_buf */
    /* Note: These might be stack-allocated cursors, so check if cleanup is needed */
    
    aws_mem_release(provider->allocator, provider);
}

void aws_s3_rdma_provider_library_init(struct aws_allocator *allocator) {
    (void)allocator; /* unused parameter */
    aws_register_log_subject_info_list(&s_s3_rdma_log_subject_list);
    s_rdma_library_initialized = true;
}

void aws_s3_rdma_provider_library_clean_up(void) {
    s_rdma_library_initialized = false;
    aws_unregister_log_subject_info_list(&s_s3_rdma_log_subject_list);
}

int aws_s3_rdma_provider_new_from_plugin(
    struct aws_allocator *allocator,
    const struct aws_s3_rdma_provider_config *config,
    struct aws_s3_rdma_provider **out_provider) {
    
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(config);
    AWS_PRECONDITION(out_provider);
    
    if (!config->enable_rdma || config->plugin_path.len == 0) {
        *out_provider = NULL;
        return AWS_OP_SUCCESS;
    }
    
    struct aws_s3_rdma_provider *provider = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_rdma_provider));
    if (!provider) {
        return AWS_OP_ERR;
    }
    
    provider->allocator = allocator;
    aws_ref_count_init(&provider->ref_count, provider, s_aws_s3_rdma_provider_destroy);
    
    /* Copy configuration */
    provider->config = *config;
    
    /* Create null-terminated plugin path */
    struct aws_byte_buf plugin_path_buf;
    aws_byte_buf_init(&plugin_path_buf, allocator, config->plugin_path.len + 1);
    aws_byte_buf_append_dynamic(&plugin_path_buf, &config->plugin_path);
    aws_byte_buf_append_byte_dynamic(&plugin_path_buf, 0);
    
    /* Load plugin */
    AWS_LOGF_INFO(AWS_LS_S3_RDMA, "id=%p: Loading RDMA provider plugin: %s", 
                  (void *)provider, (char *)plugin_path_buf.buffer);
    
    provider->plugin_handle = dlopen((char *)plugin_path_buf.buffer, RTLD_LAZY);
    aws_byte_buf_clean_up(&plugin_path_buf);
    
    if (!provider->plugin_handle) {
        AWS_LOGF_ERROR(AWS_LS_S3_RDMA, "id=%p: Failed to load plugin: %s", 
                       (void *)provider, dlerror());
        aws_raise_error(AWS_ERROR_S3_RDMA_PLUGIN_LOAD_FAILED);
        goto error;
    }
    
    /* Get plugin entry point */
    void *symbol = dlsym(provider->plugin_handle, "aws_s3_rdma_provider_get_vtable");
    aws_s3_rdma_provider_get_vtable_fn *get_vtable_fn;
    /* Use memcpy to avoid ISO C warning about object-to-function pointer conversion */
    memcpy(&get_vtable_fn, &symbol, sizeof(symbol));
    
    if (!symbol) {
        AWS_LOGF_ERROR(AWS_LS_S3_RDMA, "id=%p: Plugin missing entry point function: %s", 
                       (void *)provider, dlerror());
        aws_raise_error(AWS_ERROR_S3_RDMA_PLUGIN_LOAD_FAILED);
        goto error;
    }
    
    /* Get vtable */
    provider->vtable = get_vtable_fn();
    if (!provider->vtable) {
        AWS_LOGF_ERROR(AWS_LS_S3_RDMA, "id=%p: Plugin returned NULL vtable", (void *)provider);
        aws_raise_error(AWS_ERROR_S3_RDMA_PLUGIN_LOAD_FAILED);
        goto error;
    }
    
    AWS_LOGF_INFO(AWS_LS_S3_RDMA, "id=%p: Loaded RDMA provider: %s v%u", 
                  (void *)provider, provider->vtable->provider_name, provider->vtable->provider_version);
    
    /* Initialize provider */
    if (provider->vtable->init) {
        int result = provider->vtable->init(
            allocator, 
            (struct aws_s3_rdma_provider **)&provider->provider_instance);
        
        if (result != AWS_OP_SUCCESS) {
            AWS_LOGF_ERROR(AWS_LS_S3_RDMA, "id=%p: Failed to initialize RDMA provider", (void *)provider);
            aws_raise_error(AWS_ERROR_S3_RDMA_PLUGIN_INIT_FAILED);
            goto error;
        }
    }
    
    *out_provider = provider;
    return AWS_OP_SUCCESS;
    
error:
    if (provider) {
        aws_s3_rdma_provider_release(provider);
    }
    return AWS_OP_ERR;
}

void aws_s3_rdma_provider_release(struct aws_s3_rdma_provider *provider) {
    if (provider) {
        aws_ref_count_release(&provider->ref_count);
    }
}

bool aws_s3_rdma_provider_is_memory_suitable(
    struct aws_s3_rdma_provider *provider,
    const void *ptr,
    size_t size) {
    
    if (!provider || !provider->vtable || !provider->vtable->is_memory_suitable) {
        return false;
    }
    
    return provider->vtable->is_memory_suitable(provider->provider_instance, ptr, size);
}

int aws_s3_rdma_provider_register_memory(
    struct aws_s3_rdma_provider *provider,
    void *ptr,
    size_t size) {
    
    if (!provider || !provider->vtable || !provider->vtable->register_memory) {
        return AWS_OP_ERR;
    }
    
    return provider->vtable->register_memory(provider->provider_instance, ptr, size);
}

int aws_s3_rdma_provider_deregister_memory(
    struct aws_s3_rdma_provider *provider,
    void *ptr) {
    
    if (!provider || !provider->vtable || !provider->vtable->deregister_memory) {
        return AWS_OP_ERR;
    }
    
    return provider->vtable->deregister_memory(provider->provider_instance, ptr);
}

int aws_s3_rdma_provider_pre_register_memory(
    struct aws_s3_rdma_provider *provider,
    void *base,
    size_t size) {

    if (!provider || !provider->vtable ||
        provider->vtable->provider_version < AWS_S3_RDMA_PROVIDER_VERSION_2 ||
        !provider->vtable->pre_register_memory) {
        return aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
    }

    return provider->vtable->pre_register_memory(provider->provider_instance, base, size);
}

int aws_s3_rdma_provider_release_memory(
    struct aws_s3_rdma_provider *provider,
    void *base) {

    if (!provider || !provider->vtable ||
        provider->vtable->provider_version < AWS_S3_RDMA_PROVIDER_VERSION_2 ||
        !provider->vtable->release_memory) {
        return aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
    }

    return provider->vtable->release_memory(provider->provider_instance, base);
}

bool aws_s3_rdma_provider_is_slice_pre_registered(
    struct aws_s3_rdma_provider *provider,
    const void *ptr,
    size_t size) {

    if (!provider || !provider->vtable ||
        provider->vtable->provider_version < AWS_S3_RDMA_PROVIDER_VERSION_2 ||
        !provider->vtable->is_slice_pre_registered) {
        return false;
    }

    return provider->vtable->is_slice_pre_registered(provider->provider_instance, ptr, size);
}

int aws_s3_rdma_provider_prepare_put_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *s3_key,
    const void *buffer,
    size_t size,
    size_t offset,
    struct aws_byte_cursor *out_rdma_token) {
    
    if (!provider || !provider->vtable || !provider->vtable->prepare_put_token) {
        return AWS_OP_ERR;
    }
    
    return provider->vtable->prepare_put_token(
        provider->provider_instance, s3_key, buffer, size, offset, out_rdma_token);
}

int aws_s3_rdma_provider_prepare_get_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *s3_key,
    void *buffer,
    size_t size,
    size_t offset,
    struct aws_byte_cursor *out_rdma_token) {
    
    if (!provider || !provider->vtable || !provider->vtable->prepare_get_token) {
        return AWS_OP_ERR;
    }
    
    return provider->vtable->prepare_get_token(
        provider->provider_instance, s3_key, buffer, size, offset, out_rdma_token);
}

int aws_s3_rdma_provider_process_reply_token(
    struct aws_s3_rdma_provider *provider,
    const struct aws_byte_cursor *rdma_reply_token,
    void *user_data,
    aws_s3_rdma_completion_fn *completion_callback) {
    
    if (!provider || !provider->vtable || !provider->vtable->process_reply_token) {
        return AWS_OP_ERR;
    }
    
    return provider->vtable->process_reply_token(
        provider->provider_instance, rdma_reply_token, user_data, completion_callback);
}

struct aws_byte_cursor aws_s3_rdma_provider_get_rdma_token_header_name(
    struct aws_s3_rdma_provider *provider) {
    
    if (!provider || !provider->vtable || !provider->vtable->get_rdma_token_header_name) {
        /* Fallback to old header name if provider doesn't implement new interface */
        return aws_byte_cursor_from_c_str("x-amz-rdma-token");
    }
    
    return provider->vtable->get_rdma_token_header_name(provider->provider_instance);
}

struct aws_byte_cursor aws_s3_rdma_provider_get_rdma_reply_header_name(
    struct aws_s3_rdma_provider *provider) {
    
    if (!provider || !provider->vtable || !provider->vtable->get_rdma_reply_header_name) {
        /* Fallback to old header name if provider doesn't implement new interface */
        return aws_byte_cursor_from_c_str("x-amz-rdma-reply");
    }
    
    return provider->vtable->get_rdma_reply_header_name(provider->provider_instance);
}

struct aws_byte_cursor aws_s3_rdma_provider_get_rdma_bytes_header_name(
    struct aws_s3_rdma_provider *provider) {
    
    if (!provider || !provider->vtable || !provider->vtable->get_rdma_bytes_header_name) {
        /* Fallback to old header name if provider doesn't implement new interface */
        return aws_byte_cursor_from_c_str("x-amz-rdma-bytes");
    }
    
    return provider->vtable->get_rdma_bytes_header_name(provider->provider_instance);
}
