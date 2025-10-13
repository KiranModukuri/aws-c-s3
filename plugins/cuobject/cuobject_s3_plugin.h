#ifndef CUOBJECT_S3_PLUGIN_H
#define CUOBJECT_S3_PLUGIN_H

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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file cuobject_s3_plugin.h
 * @brief NVIDIA cuObject RDMA provider plugin for AWS S3 CRT client
 */

/**
 * cuObject plugin configuration structure
 */
struct cuobject_plugin_config {
    /* Maximum buffer size */
    size_t max_buffer_size;
    
    /* RDMA protocol version */
    uint32_t protocol_version;
    
    /* Enable debug logging */
    bool debug_logging;
};

/**
 * Plugin entry point - must be exported by the plugin
 */
const struct aws_s3_rdma_provider_vtable *aws_s3_rdma_provider_get_vtable(void);

#ifdef __cplusplus
}
#endif

#endif /* CUOBJECT_S3_PLUGIN_H */ 
