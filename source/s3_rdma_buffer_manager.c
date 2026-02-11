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
#include <aws/s3/private/s3_request.h>
#include <aws/common/allocator.h>
#include <aws/common/logging.h>
#include <aws/common/string.h>
#include <aws/common/byte_buf.h>
#include <aws/common/ref_count.h>

struct aws_s3_rdma_buffer_manager {
    struct aws_allocator *allocator;
    struct aws_ref_count ref_count;
    
    /* Associated RDMA provider */
    struct aws_s3_rdma_provider *rdma_provider;
    
    /* Vtable for buffer management operations */
    const struct aws_s3_rdma_buffer_manager_vtable *vtable;
};

static void s_aws_s3_rdma_buffer_manager_destroy(void *user_data) {
    struct aws_s3_rdma_buffer_manager *manager = user_data;
    
    if (!manager) {
        return;
    }
    
    AWS_LOGF_DEBUG(AWS_LS_S3_RDMA, "id=%p: Destroying RDMA buffer manager", (void *)manager);
    
    /* Release RDMA provider reference */
    if (manager->rdma_provider) {
        aws_s3_rdma_provider_release(manager->rdma_provider);
        manager->rdma_provider = NULL;
    }
    
    aws_mem_release(manager->allocator, manager);
}

/* Default implementation for buffer preparation */
static int s_default_prepare_buffer_for_rdma(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request,
    void *buffer,
    size_t buffer_size) {
    
    AWS_PRECONDITION(manager);
    AWS_PRECONDITION(request);
    
    if (!buffer || buffer_size == 0) {
        AWS_LOGF_DEBUG(AWS_LS_S3_RDMA, "id=%p Buffer is null or zero size", (void *)manager);
        return AWS_OP_SUCCESS;
    }
    
    /* Skip if already registered */
    if (request->rdma_buffer_registered) {
        AWS_LOGF_DEBUG(AWS_LS_S3_RDMA, "id=%p Buffer already registered for request", (void *)manager);
        return AWS_OP_SUCCESS;
    }
    
    /* Check if memory is suitable for RDMA */
    if (!aws_s3_rdma_provider_is_memory_suitable(manager->rdma_provider, buffer, buffer_size)) {
        AWS_LOGF_DEBUG(AWS_LS_S3_RDMA, "id=%p Buffer not suitable for RDMA", (void *)manager);
        return AWS_OP_SUCCESS;
    }

    /* Run before register callback */
    struct aws_s3_meta_request *meta_request = request->meta_request;
    if (meta_request->user_buffer_options.before_register_callback) {
        int before_result = meta_request->user_buffer_options.before_register_callback(meta_request->user_data);
        AWS_LOGF_DEBUG(AWS_LS_S3_RDMA, "id=%p Run before register function", (void *)manager);
        if (before_result != AWS_OP_SUCCESS) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_META_REQUEST,
                "id=%p Part %u: Before register function failed (result=%d)",
                (void *)meta_request,
                request->part_number,
                before_result);
            return before_result;
        }
    }
    
    /* Register buffer with RDMA provider */
    int register_result = aws_s3_rdma_provider_register_memory(manager->rdma_provider, buffer, buffer_size);
    if (register_result == AWS_OP_SUCCESS) {
        request->rdma_buffer_registered = 1;
        AWS_LOGF_DEBUG(
            AWS_LS_S3_RDMA,
            "id=%p Successfully registered buffer for RDMA: %p (size: %zu)",
            (void *)manager,
            buffer,
            buffer_size);
    } else {
        AWS_LOGF_WARN(
            AWS_LS_S3_RDMA,
            "id=%p Failed to register buffer for RDMA: %p (size: %zu)",
            (void *)manager,
            buffer,
            buffer_size);
    }
    
    return register_result;
}

/* Default implementation for buffer finalization */
static int s_default_finalize_buffer(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request) {
    
    AWS_PRECONDITION(manager);
    AWS_PRECONDITION(request);
    
    if (!request->rdma_buffer_registered) {
        AWS_LOGF_DEBUG(AWS_LS_S3_RDMA, "id=%p No buffer registered for request", (void *)manager);
        return AWS_OP_SUCCESS;
    }
    
    /* Get buffer from request */
    void *buffer = NULL;
    if (request->request_type == AWS_S3_REQUEST_TYPE_GET_OBJECT && 
        request->send_data.response_body.buffer) {
        buffer = request->send_data.response_body.buffer;
    } else if ((request->request_type == AWS_S3_REQUEST_TYPE_PUT_OBJECT ||
                request->request_type == AWS_S3_REQUEST_TYPE_UPLOAD_PART)) {
        /* For PUT operations, use request_body.buffer directly (no longer cleared for RDMA) */
        if (request->request_body.buffer) {
            buffer = request->request_body.buffer;
        }
    }
    
    if (!buffer) {
        AWS_LOGF_WARN(AWS_LS_S3_RDMA, "id=%p No buffer found for finalization (request_type=%d, request_body_buffer=%p)", 
                     (void *)manager, request->request_type, request->request_body.buffer);
        request->rdma_buffer_registered = 0; /* Clear flag anyway */
        return AWS_OP_SUCCESS;
    }
    
    /* Deregister buffer */
    int result = aws_s3_rdma_provider_deregister_memory(manager->rdma_provider, buffer);
    request->rdma_buffer_registered = 0; /* Clear flag regardless of result */
    
    if (result == AWS_OP_SUCCESS) {
        AWS_LOGF_DEBUG(
            AWS_LS_S3_RDMA,
            "id=%p Successfully deregistered buffer: %p",
            (void *)manager,
            buffer);
    } else {
        AWS_LOGF_WARN(
            AWS_LS_S3_RDMA,
            "id=%p Failed to deregister buffer: %p",
            (void *)manager,
            buffer);
    }
    
    return result;
}

/* Default implementation for buffer readiness check */
static bool s_default_is_buffer_rdma_ready(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request) {
    
    AWS_PRECONDITION(manager);
    AWS_PRECONDITION(request);
    
    (void)manager; /* Unused parameter */
    
    /* Check if buffer is registered and RDMA is not disabled */
    return request->rdma_buffer_registered && !request->disable_rdma_on_retry;
}

/* Default implementation for RDMA error handling */
static int s_default_handle_rdma_error(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request) {
    
    AWS_PRECONDITION(manager);
    AWS_PRECONDITION(request);
    
    AWS_LOGF_WARN(
        AWS_LS_S3_RDMA,
        "id=%p Handling RDMA error for request",
        (void *)manager);
    
    /* Mark request to disable RDMA on retry */
    request->disable_rdma_on_retry = 1;
    
    /* Finalize buffer to clean up RDMA registration */
    return s_default_finalize_buffer(manager, request);
}

/* Default vtable implementation */
static const struct aws_s3_rdma_buffer_manager_vtable s_default_rdma_buffer_manager_vtable = {
    .manager_name = "DefaultRDMABufferManager",
    .init = NULL, /* Not needed for default implementation */
    .cleanup = NULL, /* Not needed for default implementation */
    .prepare_buffer_for_rdma = s_default_prepare_buffer_for_rdma,
    .finalize_buffer = s_default_finalize_buffer,
    .is_buffer_rdma_ready = s_default_is_buffer_rdma_ready,
    .handle_rdma_error = s_default_handle_rdma_error,
};

int aws_s3_rdma_buffer_manager_new(
    struct aws_allocator *allocator,
    struct aws_s3_rdma_provider *rdma_provider,
    struct aws_s3_rdma_buffer_manager **out_manager) {
    
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(rdma_provider);
    AWS_PRECONDITION(out_manager);
    
    struct aws_s3_rdma_buffer_manager *manager = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_rdma_buffer_manager));
    if (!manager) {
        return AWS_OP_ERR;
    }
    
    manager->allocator = allocator;
    aws_ref_count_init(&manager->ref_count, manager, s_aws_s3_rdma_buffer_manager_destroy);
    
    /* Hold reference to RDMA provider */
    manager->rdma_provider = rdma_provider;
    aws_ref_count_acquire(&rdma_provider->ref_count);
    
    /* Use default vtable implementation */
    manager->vtable = &s_default_rdma_buffer_manager_vtable;
    
    *out_manager = manager;
    return AWS_OP_SUCCESS;
}

void aws_s3_rdma_buffer_manager_release(struct aws_s3_rdma_buffer_manager *manager) {
    if (manager) {
        aws_ref_count_release(&manager->ref_count);
    }
}

int aws_s3_rdma_buffer_manager_prepare_buffer_for_rdma(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request,
    void *buffer,
    size_t buffer_size) {
    
    if (!manager || !manager->vtable || !manager->vtable->prepare_buffer_for_rdma) {
        return AWS_OP_ERR;
    }
    
    return manager->vtable->prepare_buffer_for_rdma(manager, request, buffer, buffer_size);
}

int aws_s3_rdma_buffer_manager_finalize_buffer(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request) {
    
    if (!manager || !manager->vtable || !manager->vtable->finalize_buffer) {
        return AWS_OP_ERR;
    }
    
    return manager->vtable->finalize_buffer(manager, request);
}

bool aws_s3_rdma_buffer_manager_is_buffer_rdma_ready(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request) {
    
    if (!manager || !manager->vtable || !manager->vtable->is_buffer_rdma_ready) {
        return false;
    }
    
    return manager->vtable->is_buffer_rdma_ready(manager, request);
}

int aws_s3_rdma_buffer_manager_handle_rdma_error(
    struct aws_s3_rdma_buffer_manager *manager,
    struct aws_s3_request *request) {
    
    if (!manager || !manager->vtable || !manager->vtable->handle_rdma_error) {
        return AWS_OP_ERR;
    }
    
    return manager->vtable->handle_rdma_error(manager, request);
}
