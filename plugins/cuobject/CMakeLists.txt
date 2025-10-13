# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.15)
project(cuobject_s3_plugin C CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)

# Find CUDA (optional - plugin can work without it for testing)
find_package(CUDA QUIET)
if(CUDA_FOUND)
    # Do not enable the CUDA language; we only optionally link to CUDA runtime
    add_definitions(-DUSE_CUDA=1)
    message(STATUS "CUDA found (runtime linking only): ${CUDA_VERSION}")
else()
    add_definitions(-DUSE_CUDA=0)
    message(STATUS "CUDA not found - building without CUDA support")
endif()

# Find cuObject library
set(CUOBJECT_ROOT_DIR "/home/cuObject" 
    CACHE PATH "Path to cuObject root directory")

if(EXISTS "${CUOBJECT_ROOT_DIR}/include/cuobjclient.h")
    message(STATUS "Found cuObject headers at: ${CUOBJECT_ROOT_DIR}")
    set(CUOBJECT_INCLUDE_DIR "${CUOBJECT_ROOT_DIR}/include")
    
    # Force use of packaged cuObject library (has all required symbols)
    set(CUOBJECT_PACKAGED_LIB "${CUOBJECT_ROOT_DIR}/build_x86/_CPack_Packages/Linux/TGZ/cuObject-0.8.5-Linux/src/lib/libcuobjclient.so")
    if(EXISTS "${CUOBJECT_PACKAGED_LIB}")
        set(CUOBJECT_LIBRARY "${CUOBJECT_PACKAGED_LIB}")
        message(STATUS "Using packaged cuObject library: ${CUOBJECT_LIBRARY}")
    else()
        # Fallback to lib_src version
        find_library(CUOBJECT_LIBRARY
            NAMES cuobjclient cuobject
            PATHS "${CUOBJECT_ROOT_DIR}/build_x86/lib_src" 
                  "${CUOBJECT_ROOT_DIR}/build_aarch64/lib_src"
                  "${CUOBJECT_ROOT_DIR}/lib"
            NO_DEFAULT_PATH)
        message(STATUS "Using fallback cuObject library: ${CUOBJECT_LIBRARY}")
    endif()
    
    if(CUOBJECT_LIBRARY)
        message(STATUS "Found cuObject library: ${CUOBJECT_LIBRARY}")
    else()
        message(WARNING "cuObject library not found - building with stub implementation")
    endif()
else()
    message(WARNING "cuObject headers not found at ${CUOBJECT_ROOT_DIR} - building with stub implementation")
endif()

# AWS CRT includes - monorepo layout under crt/*
if(DEFINED AWS_CRT_ROOT_DIR)
    # Use provided AWS_CRT_ROOT_DIR
    set(AWS_COMMON_ROOT_DIR "${AWS_CRT_ROOT_DIR}/crt/aws-c-common")
    set(AWS_CRT_BUILD_DIR "${AWS_CRT_ROOT_DIR}/build")
else()
    # Default to relative paths
set(AWS_CRT_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(AWS_COMMON_ROOT_DIR "${AWS_CRT_ROOT_DIR}/crt/aws-c-common")
set(AWS_CRT_BUILD_DIR "${AWS_CRT_ROOT_DIR}/build")
endif()

# aws-c-common generates headers (e.g. aws/common/config.h) under its own build tree
set(AWS_COMMON_BUILD_DIR "${AWS_COMMON_ROOT_DIR}/build")

# Plugin source files
if(CUOBJECT_LIBRARY)
    set(PLUGIN_SOURCES cuobject_s3_plugin.cpp)
    message(STATUS "Building real cuObject plugin with library support")
else()
    set(PLUGIN_SOURCES cuobject_s3_plugin_mock.cpp)
    message(STATUS "Building cuObject plugin with mock implementation (cuObject library not found)")
endif()

# Create shared library (plugin)
add_library(cuobject_s3_plugin SHARED ${PLUGIN_SOURCES})

# Set plugin properties
set_target_properties(cuobject_s3_plugin PROPERTIES
    PREFIX "lib"
    SUFFIX ".so"
    VERSION "1.0.0"
    SOVERSION "1"
    CXX_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
)

# Include directories
find_package(aws-c-s3 CONFIG QUIET)
if (aws-c-s3_FOUND)
    target_include_directories(cuobject_s3_plugin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        $<TARGET_PROPERTY:AWS::aws-c-s3,INTERFACE_INCLUDE_DIRECTORIES>
    )
else()
    target_include_directories(cuobject_s3_plugin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${AWS_CRT_ROOT_DIR}/include
        ${AWS_COMMON_ROOT_DIR}/include
        ${AWS_COMMON_BUILD_DIR}/generated/include
        ${AWS_CRT_ROOT_DIR}/crt/aws-c-io/include
        ${AWS_CRT_ROOT_DIR}/crt/aws-c-http/include
        ${AWS_CRT_ROOT_DIR}/crt/aws-c-auth/include
        ${AWS_CRT_ROOT_DIR}/crt/aws-c-cal/include
        ${AWS_CRT_ROOT_DIR}/crt/aws-c-compression/include
        ${AWS_CRT_ROOT_DIR}/crt/aws-c-event-stream/include
        ${AWS_CRT_ROOT_DIR}/crt/aws-c-mqtt/include
        ${AWS_CRT_ROOT_DIR}/crt/aws-checksums/include
        ${AWS_CRT_ROOT_DIR}/crt/aws-c-sdkutils/include
    )
endif()

# Add cuObject includes if available
if(CUOBJECT_INCLUDE_DIR)
    target_include_directories(cuobject_s3_plugin PRIVATE ${CUOBJECT_INCLUDE_DIR})
    # Also add cufile include path (needed by cuobjclient.h)
    target_include_directories(cuobject_s3_plugin PRIVATE 
        "${CUOBJECT_ROOT_DIR}/build_x86/_CPack_Packages/Linux/TGZ/cuObject-0.8.3-Linux/src/include"
        "${CUOBJECT_ROOT_DIR}/build_aarch64/_CPack_Packages/Linux/TGZ/cuObject-0.8.3-Linux/src/include"
    )
    # Try to find cufile headers from common locations or env override
    find_path(CUFILE_INCLUDE_DIR
        NAMES cufile.h
        PATHS
            $ENV{CUFILE_INCLUDE_DIR}
            ${CUOBJECT_ROOT_DIR}/build_x86/_CPack_Packages/Linux/TGZ/cuObject-0.8.3-Linux/src/include
            ${CUOBJECT_ROOT_DIR}/build_aarch64/_CPack_Packages/Linux/TGZ/cuObject-0.8.3-Linux/src/include
            /usr/local/cuda/include
            /opt/nvidia/gds/include
            /usr/include
        NO_DEFAULT_PATH)
    if (CUFILE_INCLUDE_DIR)
        message(STATUS "Found cufile headers at: ${CUFILE_INCLUDE_DIR}")
        target_include_directories(cuobject_s3_plugin PRIVATE ${CUFILE_INCLUDE_DIR})
    else()
        message(WARNING "cufile.h not found; ensure CUFILE_INCLUDE_DIR is set if build fails")
    endif()
endif()

# Compiler definitions
target_compile_definitions(cuobject_s3_plugin PRIVATE
    AWS_UNSTABLE_TESTING_API=1
)

# Link libraries
find_package(aws-c-common CONFIG QUIET)
if (aws-c-common_FOUND)
    target_link_libraries(cuobject_s3_plugin PRIVATE
        AWS::aws-c-common
        ${CMAKE_DL_LIBS}
        pthread
    )
else()
    # Fallback: try to find the library manually
    find_library(AWS_COMMON_LIBRARY
        NAMES aws-c-common
        PATHS ${AWS_CRT_BUILD_DIR}/aws-c-common 
              ${AWS_CRT_ROOT_DIR}/build/aws-c-common
        NO_DEFAULT_PATH)
    
    target_link_libraries(cuobject_s3_plugin PRIVATE
        ${AWS_COMMON_LIBRARY}
        ${CMAKE_DL_LIBS}
        pthread
    )
endif()

# Link cuObject library if available
if(CUOBJECT_LIBRARY)
    target_link_libraries(cuobject_s3_plugin PRIVATE ${CUOBJECT_LIBRARY})
    
    # Also link against the cuObject packaged cufile library
    find_library(CUOBJECT_CUFILE_LIBRARY
        NAMES cufile
        PATHS
            "${CUOBJECT_ROOT_DIR}/build_x86/_CPack_Packages/Linux/TGZ/cuObject-0.8.3-Linux/src/lib"
            $ENV{CUFILE_LIB_DIR}
            /opt/nvidia/gds/lib
            /usr/local/cuda/lib64
            /usr/lib/x86_64-linux-gnu
            /usr/lib64
        NO_DEFAULT_PATH)
    
    if(CUOBJECT_CUFILE_LIBRARY)
        target_link_libraries(cuobject_s3_plugin PRIVATE ${CUOBJECT_CUFILE_LIBRARY})
        message(STATUS "Linking against cuObject cufile library: ${CUOBJECT_CUFILE_LIBRARY}")
    endif()
    
    # Set RPATH to prefer cuObject packaged libraries
    set(CUOBJECT_LIB_DIR "${CUOBJECT_ROOT_DIR}/build_x86/_CPack_Packages/Linux/TGZ/cuObject-0.8.3-Linux/src/lib")
    set_target_properties(cuobject_s3_plugin PROPERTIES
        BUILD_RPATH "${CUOBJECT_LIB_DIR}"
        INSTALL_RPATH "${CUOBJECT_LIB_DIR}"
        BUILD_RPATH_USE_ORIGIN TRUE
        INSTALL_RPATH_USE_LINK_PATH FALSE
    )
    
    target_compile_definitions(cuobject_s3_plugin PRIVATE HAVE_CUOBJECT=1)
else()
    target_compile_definitions(cuobject_s3_plugin PRIVATE HAVE_CUOBJECT=0)
endif()

# Link CUDA libraries if available
if(CUDA_FOUND)
    target_link_libraries(cuobject_s3_plugin PRIVATE
        ${CUDA_LIBRARIES}
        ${CUDA_CUDART_LIBRARY}
    )
    target_include_directories(cuobject_s3_plugin PRIVATE ${CUDA_INCLUDE_DIRS})
endif()

# Compiler flags
target_compile_options(cuobject_s3_plugin PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fPIC
)

# Debug/Release specific flags
target_compile_options(cuobject_s3_plugin PRIVATE
    $<$<CONFIG:Debug>:-g -O0 -DDEBUG>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# Install plugin
install(TARGETS cuobject_s3_plugin
    LIBRARY DESTINATION lib/aws-s3-plugins
    RUNTIME DESTINATION bin
)

# Install headers for development
install(FILES cuobject_s3_plugin.h
    DESTINATION include/aws/s3/plugins
)

# Test programs for development and validation
if(BUILD_TESTING)
    # Test for mock plugin implementation (individual functions)
    add_executable(test_mock_plugin test_mock_plugin.c)
    set_target_properties(test_mock_plugin PROPERTIES
        LINKER_LANGUAGE C
    )
    target_link_libraries(test_mock_plugin PRIVATE
        ${CMAKE_DL_LIBS}
    )
    
    # Test for AWS CRT integration with mock plugin (vtable interface)
    add_executable(test_aws_integration_mock test_aws_integration_mock.c)
    set_target_properties(test_aws_integration_mock PROPERTIES
        LINKER_LANGUAGE C
    )
    target_link_libraries(test_aws_integration_mock PRIVATE
        ${CMAKE_DL_LIBS}
    )
    
    # Test for real cuObject plugin (vtable interface)
    add_executable(test_real_plugin test_real_plugin.c)
    set_target_properties(test_real_plugin PROPERTIES
        LINKER_LANGUAGE C
    )
    target_include_directories(test_real_plugin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${AWS_CRT_ROOT_DIR}/include
        ${AWS_COMMON_ROOT_DIR}/include
        ${AWS_CRT_BUILD_DIR}/aws-c-common/generated/include
    )
    target_link_libraries(test_real_plugin PRIVATE
        ${CMAKE_DL_LIBS}
    )
    
    # Test for AWS CRT S3 client RDMA integration
    add_executable(test_crt_rdma_integration test_crt_rdma_integration.c)
    set_target_properties(test_crt_rdma_integration PROPERTIES
        LINKER_LANGUAGE C
    )
    target_include_directories(test_crt_rdma_integration PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        # AWS CRT includes from source
        ${AWS_CRT_ROOT_DIR}/include
        ${AWS_CRT_ROOT_DIR}/../aws-c-common/include
        ${AWS_CRT_ROOT_DIR}/../aws-c-io/include
        ${AWS_CRT_ROOT_DIR}/../aws-c-http/include
        ${AWS_CRT_ROOT_DIR}/../aws-c-auth/include
        ${AWS_CRT_ROOT_DIR}/../aws-c-cal/include
        ${AWS_CRT_ROOT_DIR}/../aws-c-compression/include
        ${AWS_CRT_ROOT_DIR}/../aws-c-event-stream/include
        ${AWS_CRT_ROOT_DIR}/../aws-c-mqtt/include
        ${AWS_CRT_ROOT_DIR}/../aws-checksums/include
        ${AWS_CRT_ROOT_DIR}/../aws-c-sdkutils/include
        # Generated includes
        ${AWS_CRT_BUILD_DIR}/aws-c-common/generated/include
        # Third party includes
        ${AWS_CRT_ROOT_DIR}/../build/deps/S2N/install/include
        ${AWS_CRT_ROOT_DIR}/../build/deps/AWSLC/install/include
    )
    target_link_libraries(test_crt_rdma_integration PRIVATE
        # Link against AWS CRT libraries (using the correct build paths)
        ${AWS_CRT_ROOT_DIR}/../build/aws-c-s3/libaws-c-s3.a
        ${AWS_CRT_ROOT_DIR}/../build/aws-c-auth/libaws-c-auth.a
        ${AWS_CRT_ROOT_DIR}/../build/aws-c-http/libaws-c-http.a
        ${AWS_CRT_ROOT_DIR}/../build/aws-c-io/libaws-c-io.a
        ${AWS_CRT_ROOT_DIR}/../build/aws-c-cal/libaws-c-cal.a
        ${AWS_CRT_ROOT_DIR}/../build/aws-c-compression/libaws-c-compression.a
        ${AWS_CRT_ROOT_DIR}/../build/aws-c-event-stream/libaws-c-event-stream.a
        ${AWS_CRT_ROOT_DIR}/../build/aws-c-common/libaws-c-common.a
        ${AWS_CRT_ROOT_DIR}/../build/aws-c-sdkutils/libaws-c-sdkutils.a
        ${AWS_CRT_ROOT_DIR}/../build/aws-checksums/libaws-checksums.a
        ${AWS_CRT_ROOT_DIR}/../build/deps/S2N/lib/libs2n.a
        ${AWS_CRT_ROOT_DIR}/../build/deps/AWSLC/crypto/libcrypto.a
        # System libraries (order matters for static linking)
        ${CMAKE_DL_LIBS}
        pthread
        rt
        m
    )

    # RDMA PUT functional test using installed AWS CRT packages
    # This target links against installed CRT CMake packages via CMAKE_PREFIX_PATH
    find_package(aws-c-s3 CONFIG REQUIRED)
    find_package(aws-c-http CONFIG REQUIRED)
    find_package(aws-c-io CONFIG REQUIRED)
    find_package(aws-c-auth CONFIG REQUIRED)
    find_package(aws-c-cal CONFIG REQUIRED)
    find_package(aws-c-compression CONFIG REQUIRED)
    find_package(aws-checksums CONFIG REQUIRED)
    find_package(aws-c-sdkutils CONFIG REQUIRED)

    add_executable(test_rdma_put test_rdma_put.c)
    set_target_properties(test_rdma_put PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(test_rdma_put PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(test_rdma_put PRIVATE
        AWS::aws-c-s3
        AWS::aws-c-http
        AWS::aws-c-io
        AWS::aws-c-auth
        AWS::aws-c-cal
        AWS::aws-c-compression
        AWS::aws-checksums
        AWS::aws-c-sdkutils
        ${CMAKE_DL_LIBS}
        pthread
        rt
        m
    )

    # Optional CUDA for GPU buffer allocation (if available)
    find_package(CUDA QUIET)
    if(CUDA_FOUND)
        target_compile_definitions(test_rdma_put PRIVATE USE_CUDA=1)
        target_include_directories(test_rdma_put PRIVATE ${CUDA_INCLUDE_DIRS})
        target_link_libraries(test_rdma_put PRIVATE ${CUDA_CUDART_LIBRARY})
    endif()

    # (removed) test_rdma_get: consolidated into test_rdma_io

    # Combined RDMA PUT/GET verify test
    add_executable(test_rdma_io test_rdma_io.c)
    set_target_properties(test_rdma_io PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(test_rdma_io PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(test_rdma_io PRIVATE
        AWS::aws-c-s3
        AWS::aws-c-http
        AWS::aws-c-io
        AWS::aws-c-auth
        AWS::aws-c-cal
        AWS::aws-c-compression
        AWS::aws-checksums
        AWS::aws-c-sdkutils
        ${CMAKE_DL_LIBS}
        pthread
        rt
        m
    )
    if(CUDA_FOUND)
        target_compile_definitions(test_rdma_io PRIVATE USE_CUDA=1)
        target_include_directories(test_rdma_io PRIVATE ${CUDA_INCLUDE_DIRS})
        target_link_libraries(test_rdma_io PRIVATE ${CUDA_CUDART_LIBRARY})
    endif()


    # New test: user buffer integration GET
    add_executable(test_user_buffer_integration test_user_buffer_integration.c)
    set_target_properties(test_user_buffer_integration PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(test_user_buffer_integration PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(test_user_buffer_integration PRIVATE
        AWS::aws-c-s3
        AWS::aws-c-http
        AWS::aws-c-io
        AWS::aws-c-auth
        AWS::aws-c-cal
        AWS::aws-c-compression
        AWS::aws-checksums
        AWS::aws-c-sdkutils
        ${CMAKE_DL_LIBS}
        pthread
        rt
        m
    )
endif()

# Print configuration summary
message(STATUS "cuObject S3 Plugin Configuration:")
message(STATUS "  CUDA Support: ${CUDA_FOUND}")
message(STATUS "  cuObject Library: ${CUOBJECT_LIBRARY}")
message(STATUS "  cuObject Include: ${CUOBJECT_INCLUDE_DIR}")
message(STATUS "  Install Directory: ${CMAKE_INSTALL_PREFIX}/lib/aws-s3-plugins") 
