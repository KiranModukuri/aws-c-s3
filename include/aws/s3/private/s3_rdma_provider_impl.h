#ifndef AWS_S3_RDMA_PROVIDER_IMPL_H
#define AWS_S3_RDMA_PROVIDER_IMPL_H

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
#include <aws/common/ref_count.h>
#include <aws/common/allocator.h>

AWS_PUSH_SANE_WARNING_LEVEL

AWS_EXTERN_C_BEGIN

/**
 * @brief Internal RDMA provider structure
 * 
 * This structure is private to the RDMA provider implementation
 * and should only be accessed by internal S3 RDMA components.
 */
struct aws_s3_rdma_provider {
    struct aws_allocator *allocator;
    struct aws_ref_count ref_count;
    
    /* Plugin handle */
    void *plugin_handle;
    
    /* Plugin vtable */
    const struct aws_s3_rdma_provider_vtable *vtable;
    
    /* Provider instance from plugin */
    void *provider_instance;
    
    /* Configuration */
    struct aws_s3_rdma_provider_config config;
};

AWS_EXTERN_C_END

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_S3_RDMA_PROVIDER_IMPL_H */
