/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/**
 * @file cuobject_s3_plugin_mock.cpp
 * @brief Mock implementation of cuObject S3 RDMA plugin for testing and development
 * 
 * This file provides a mock implementation of the cuObject S3 RDMA plugin that simulates
 * cuObject behavior without requiring the actual cuObject library. It's used when the
 * real cuObject library is not available during build time.
 * 
 * The mock implementation provides:
 * - Simulated cuObject client connection
 * - Mock GPU memory detection
 * - RDMA token generation and processing
 * - All plugin interfaces (individual functions + AWS CRT vtable)
 */

#include "cuobject_s3_plugin.h"
#include <iostream>
#include <memory>
#include <string>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>

// Mock cuObject client implementation
// This simulates the behavior of a real cuObject client without requiring the actual library

struct CuObjectClient {
    std::string server_host;
    int server_port;
    bool connected;
    
    CuObjectClient(const std::string& host, int port) 
        : server_host(host), server_port(port), connected(false) {}
    
    bool connect() {
        // In real implementation, this would connect to cuObject server
        std::cout << "[cuObject] Connecting to " << server_host << ":" << server_port << std::endl;
        connected = true;
        return true;
    }
    
    void disconnect() {
        std::cout << "[cuObject] Disconnecting from server" << std::endl;
        connected = false;
    }
    
    std::string register_memory(void* ptr, size_t size) {
        if (!connected) return "";
        
        // Generate a realistic-looking RDMA token
        char token[64];
        snprintf(token, sizeof(token), "rdma_token_%p_%zu", ptr, size);
        std::cout << "[cuObject] Registered memory region: " << token << " (ptr=" << ptr << ", size=" << size << ")" << std::endl;
        return std::string(token);
    }
    
    bool unregister_memory(const std::string& token) {
        if (!connected) return false;
        
        std::cout << "[cuObject] Unregistered memory region: " << token << std::endl;
        return true;
    }
    
    bool is_gpu_memory(void* ptr) {
        // In real implementation, this would check if pointer is GPU memory
        // For now, we'll do a simple heuristic check
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        
        // Assume GPU memory has high addresses (this is just for demo)
        return addr > 0x7f0000000000ULL;
    }
};

// Global plugin state
static CuObjectClient* g_cuobject_client = nullptr;
static std::string g_provider_name = "cuObject RDMA Provider (Mock)";
static std::string g_provider_version = "1.0.0-mock";

extern "C" {

// Plugin initialization
__attribute__((visibility("default")))
int cuobject_s3_plugin_init(const char* config_json) {
    std::cout << "[cuObject Plugin] Initializing with config: " << (config_json ? config_json : "null") << std::endl;
    
    // Parse configuration (simplified JSON parsing)
    std::string host = "localhost";
    int port = 8080;
    
    if (config_json) {
        // Simple JSON parsing for demo (real implementation would use proper JSON parser)
        std::string config(config_json);
        
        size_t host_pos = config.find("\"host\":");
        if (host_pos != std::string::npos) {
            size_t start = config.find("\"", host_pos + 7);
            size_t end = config.find("\"", start + 1);
            if (start != std::string::npos && end != std::string::npos) {
                host = config.substr(start + 1, end - start - 1);
            }
        }
        
        size_t port_pos = config.find("\"port\":");
        if (port_pos != std::string::npos) {
            size_t start = port_pos + 7;
            while (start < config.length() && (config[start] == ' ' || config[start] == ':')) start++;
            size_t end = start;
            while (end < config.length() && isdigit(config[end])) end++;
            if (end > start) {
                port = std::stoi(config.substr(start, end - start));
            }
        }
    }
    
    try {
        g_cuobject_client = new CuObjectClient(host, port);
        if (!g_cuobject_client->connect()) {
            delete g_cuobject_client;
            g_cuobject_client = nullptr;
            return -1;
        }
        
        std::cout << "[cuObject Plugin] Successfully initialized and connected to " << host << ":" << port << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[cuObject Plugin] Initialization failed: " << e.what() << std::endl;
        return -1;
    }
}

// Plugin cleanup
__attribute__((visibility("default")))
void cuobject_s3_plugin_cleanup() {
    std::cout << "[cuObject Plugin] Cleaning up" << std::endl;
    
    if (g_cuobject_client) {
        g_cuobject_client->disconnect();
        delete g_cuobject_client;
        g_cuobject_client = nullptr;
    }
}

// Get provider information
__attribute__((visibility("default")))
const char* cuobject_s3_plugin_get_name() {
    return g_provider_name.c_str();
}

__attribute__((visibility("default")))
const char* cuobject_s3_plugin_get_version() {
    return g_provider_version.c_str();
}

// Check if memory is suitable for RDMA
__attribute__((visibility("default")))
int cuobject_s3_plugin_is_memory_suitable(void* ptr, size_t size) {
    if (!g_cuobject_client || !g_cuobject_client->connected) {
        return 0;
    }
    
    // Check if this is GPU memory or large enough for RDMA
    bool is_gpu = g_cuobject_client->is_gpu_memory(ptr);
    bool is_large_enough = size >= (1024 * 1024); // 1MB threshold
    
    int suitable = (is_gpu || is_large_enough) ? 1 : 0;
    
    std::cout << "[cuObject Plugin] Memory suitability check: ptr=" << ptr 
              << ", size=" << size << ", gpu=" << is_gpu 
              << ", large=" << is_large_enough << ", suitable=" << suitable << std::endl;
    
    return suitable;
}

// Register memory for RDMA
__attribute__((visibility("default")))
int cuobject_s3_plugin_register_memory(void* ptr, size_t size, char* token_out, size_t token_size) {
    if (!g_cuobject_client || !g_cuobject_client->connected) {
        return -1;
    }
    
    std::string token = g_cuobject_client->register_memory(ptr, size);
    if (token.empty()) {
        return -1;
    }
    
    if (token.length() >= token_size) {
        std::cerr << "[cuObject Plugin] Token buffer too small" << std::endl;
        return -1;
    }
    
    strcpy(token_out, token.c_str());
    return 0;
}

// Unregister memory
__attribute__((visibility("default")))
int cuobject_s3_plugin_unregister_memory(const char* token) {
    if (!g_cuobject_client || !g_cuobject_client->connected) {
        return -1;
    }
    
    return g_cuobject_client->unregister_memory(std::string(token)) ? 0 : -1;
}

// Generate RDMA token for GET operation
__attribute__((visibility("default")))
int cuobject_s3_plugin_generate_get_token(void* buffer, size_t size, const char* memory_token, 
                                          char* rdma_token_out, size_t token_size) {
    if (!g_cuobject_client || !g_cuobject_client->connected) {
        return -1;
    }
    
    // Generate GET token based on memory token
    char get_token[256];
    snprintf(get_token, sizeof(get_token), "GET_%s_%zu", memory_token, size);
    
    if (strlen(get_token) >= token_size) {
        return -1;
    }
    
    strcpy(rdma_token_out, get_token);
    
    std::cout << "[cuObject Plugin] Generated GET token: " << get_token << std::endl;
    return 0;
}

// Generate RDMA token for PUT operation
__attribute__((visibility("default")))
int cuobject_s3_plugin_generate_put_token(void* buffer, size_t size, const char* memory_token,
                                          char* rdma_token_out, size_t token_size) {
    if (!g_cuobject_client || !g_cuobject_client->connected) {
        return -1;
    }
    
    // Generate PUT token based on memory token
    char put_token[256];
    snprintf(put_token, sizeof(put_token), "PUT_%s_%zu", memory_token, size);
    
    if (strlen(put_token) >= token_size) {
        return -1;
    }
    
    strcpy(rdma_token_out, put_token);
    
    std::cout << "[cuObject Plugin] Generated PUT token: " << put_token << std::endl;
    return 0;
}

// Process RDMA reply token
__attribute__((visibility("default")))
int cuobject_s3_plugin_process_reply_token(const char* reply_token, void* buffer, size_t size) {
    if (!g_cuobject_client || !g_cuobject_client->connected) {
        return -1;
    }
    
    std::cout << "[cuObject Plugin] Processing reply token: " << reply_token 
              << " for buffer=" << buffer << ", size=" << size << std::endl;
    
    // In real implementation, this would handle the RDMA completion
    // For now, just validate the token format
    if (strncmp(reply_token, "REPLY_", 6) == 0) {
        std::cout << "[cuObject Plugin] Reply token processed successfully" << std::endl;
        return 0;
    }
    
    std::cerr << "[cuObject Plugin] Invalid reply token format" << std::endl;
    return -1;
}

// Get statistics
int cuobject_s3_plugin_get_stats(char* stats_json_out, size_t stats_size) {
    if (!g_cuobject_client) {
        return -1;
    }
    
    const char* stats = "{"
        "\"status\": \"connected\","
        "\"transfers_completed\": 42,"
        "\"bytes_transferred\": 1073741824,"
        "\"average_bandwidth_mbps\": 2500.5"
        "}";
    
    if (strlen(stats) >= stats_size) {
        return -1;
    }
    
    strcpy(stats_json_out, stats);
    return 0;
}

// Reset statistics
int cuobject_s3_plugin_reset_stats() {
    std::cout << "[cuObject Plugin] Statistics reset" << std::endl;
    return 0;
}

// Set configuration
int cuobject_s3_plugin_set_config(const char* config_json) {
    std::cout << "[cuObject Plugin] Configuration updated: " << (config_json ? config_json : "null") << std::endl;
    
    // In real implementation, this would update runtime configuration
    return 0;
}

// Header name functions
const char* cuobject_s3_plugin_get_rdma_token_header_name() {
    return "x-amz-rdma-token";
}

const char* cuobject_s3_plugin_get_rdma_reply_header_name() {
    return "x-amz-rdma-reply";
}

const char* cuobject_s3_plugin_get_rdma_bytes_header_name() {
    return "x-amz-rdma-bytes";
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
        std::cout << "[cuObject Plugin Mock] Using custom RDMA token header: " << token_header << std::endl;
    }
    
    const char* reply_header = std::getenv("CUOBJECT_RDMA_REPLY_HEADER_NAME");
    if (reply_header && strlen(reply_header) > 0) {
        g_rdma_reply_header_name = reply_header;
        std::cout << "[cuObject Plugin Mock] Using custom RDMA reply header: " << reply_header << std::endl;
    }
    
    const char* bytes_header = std::getenv("CUOBJECT_RDMA_BYTES_HEADER_NAME");
    if (bytes_header && strlen(bytes_header) > 0) {
        g_rdma_bytes_header_name = bytes_header;
        std::cout << "[cuObject Plugin Mock] Using custom RDMA bytes header: " << bytes_header << std::endl;
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

// Header name functions for vtable
static struct aws_byte_cursor mock_get_rdma_token_header_name(struct aws_s3_rdma_provider *provider) {
    (void)provider;
    init_header_names_from_env();
    return byte_cursor_from_c_str(g_rdma_token_header_name.c_str());
}

static struct aws_byte_cursor mock_get_rdma_reply_header_name(struct aws_s3_rdma_provider *provider) {
    (void)provider;
    init_header_names_from_env();
    return byte_cursor_from_c_str(g_rdma_reply_header_name.c_str());
}

static struct aws_byte_cursor mock_get_rdma_bytes_header_name(struct aws_s3_rdma_provider *provider) {
    (void)provider;
    init_header_names_from_env();
    return byte_cursor_from_c_str(g_rdma_bytes_header_name.c_str());
}

// AWS CRT integration entry point
__attribute__((visibility("default")))
const struct aws_s3_rdma_provider_vtable* aws_s3_rdma_provider_get_vtable() {
    static struct aws_s3_rdma_provider_vtable vtable = {
        .provider_name = "cuObject RDMA Provider (Mock)",
        .provider_version = 1,
        .init = nullptr,           // We use individual init function
        .cleanup = nullptr,        // We use individual cleanup function  
        .register_memory = nullptr,
        .deregister_memory = nullptr,
        .is_memory_suitable = nullptr,
        .get_max_transfer_size = nullptr,
        .prepare_put_token = nullptr,
        .prepare_get_token = nullptr,
        .process_reply_token = nullptr,
        .get_rdma_token_header_name = mock_get_rdma_token_header_name,
        .get_rdma_reply_header_name = mock_get_rdma_reply_header_name,
        .get_rdma_bytes_header_name = mock_get_rdma_bytes_header_name,
    };
    
    return &vtable;
}

} // extern "C" 