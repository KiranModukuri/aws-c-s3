##  Overview 

This commit introduces comprehensive RDMA (Remote Direct Memory Access) support for the AWS S3 client library, enabling high-performance, low-latency data transfers between S3 and client applications. The implementation provides a plugin-based architecture that allows RDMA operations to coexist with standard HTTP transfers.

##  Key Objectives

1. **Enable RDMA transfers** for S3 GET and PUT operations
2. **Maintain backward compatibility** with existing HTTP-based transfers
3. **Support automatic fallback** from RDMA to HTTP when needed
4. **Integrate RDMA buffer management** with existing memory lifecycle
5. **Handle RDMA-specific checksums** and validation

## Terminology

**Key Terms Used in This Document:**

- **RDMA (Remote Direct Memory Access)**: Hardware capability for direct memory-to-memory data transfer between systems without CPU involvement, bypassing the operating system.

- **Zero-Copy**: Data transfer technique where data moves directly from source to destination memory without intermediate copying through CPU or system buffers.

- **Out-of-Band Transfer**: Data transferred through a separate channel (RDMA) while control information flows through the main channel (HTTP).

- **RDMA Token**: Opaque, provider-specific identifier encoding buffer location, size, and access permissions for RDMA transfers. Typically 80-120 bytes.

- **Memory Registration**: Process of making memory accessible to RDMA hardware by pinning pages and obtaining hardware memory keys.

- **Pinnable Memory**: Memory that can be locked in physical RAM (not swapped to disk) to ensure stable addresses for RDMA operations.

- **Meta Request**: High-level S3 operation (PUT/GET) that may be split into multiple HTTP requests (e.g., multipart uploads, ranged GETs).

- **Part Size**: Size of individual chunks in multipart uploads or ranged downloads (default: 8 MB).

- **aws-chunked Encoding**: AWS-specific HTTP transfer encoding that wraps data with chunk metadata and trailing checksums. Incompatible with RDMA.

- **SigV4 (Signature Version 4)**: AWS authentication protocol that signs HTTP requests including headers and payload.

- **GPUDirect**: NVIDIA technology enabling direct RDMA transfers to/from GPU memory, bypassing CPU and system RAM.

- **Vtable (Virtual Function Table)**: C design pattern for polymorphism using function pointers, enabling pluggable RDMA provider implementations.

**S3-Specific Terms:**

- **ETag**: Entity tag - unique identifier for a specific version of an S3 object, used for caching and validation.

- **Content-MD5**: Base64-encoded MD5 hash of the HTTP request body, used for integrity verification. Not applicable to RDMA (empty body).

- **If-Match**: HTTP conditional header containing an ETag, ensuring operations only proceed if the object hasn't changed.

- **Content-Range**: HTTP header indicating which portion of an object is being transferred (e.g., `bytes 0-1023/2048`).

- **Multipart Upload**: S3 feature for uploading large objects in multiple parts (each 5 MB - 5 GB), assembled on S3 after all parts complete.

- **UploadId**: Unique identifier for a multipart upload session.

- **CompleteMultipartUpload**: Final API call that assembles uploaded parts into a single S3 object.

- **Ranged GET**: S3 GET request with `Range` header specifying byte range to retrieve (e.g., `Range: bytes=0-1023`). Used for parallel downloads and large object streaming.

- **Auto-Ranged GET/PUT**: Client library feature that automatically splits large transfers into multiple parallel requests for better performance.

**Checksum-Related Terms:**

- **Flexible Checksums**: S3 feature supporting multiple checksum algorithms (CRC32, CRC32C, SHA-1, SHA-256) for data integrity validation.

- **CRC32C**: Cyclic Redundancy Check algorithm optimized for error detection, often hardware-accelerated. Common choice for RDMA transfers.

- **Trailing Checksums**: Checksums sent at the end of chunked transfer, after all data. Not compatible with RDMA (data is out-of-band).

## Files Changed

### New Files (3)
- `include/aws/s3/s3_rdma_provider.h` - Public RDMA provider API
- `include/aws/s3/private/s3_rdma_provider_impl.h` - Internal RDMA structures
- `source/s3_rdma_provider.c` - RDMA provider implementation
- `source/s3_rdma_buffer_manager.c` - RDMA buffer lifecycle management
- `source/s3_rdma_request_handler.c` - RDMA request/response handling

### Modified Core Files (13)
- `source/s3_client.c` - Client-level RDMA initialization
- `source/s3_auto_ranged_get.c` - RDMA support for ranged GETs
- `source/s3_auto_ranged_put.c` - RDMA support for multipart uploads
- `source/s3_meta_request.c`  - Meta request RDMA integration
- `source/s3_default_meta_request.c` - RDMA for single-request operations
- Plus headers and supporting files

##  Architecture Overview

### RDMA Component Stack

```
+------------------------------------------+
|        S3 Client Application             |
+------------------+-----------------------+
                   |
+------------------v-----------------------+
|      aws_s3_client (enhanced)            |
|  * enable_rdma flag                      |
|  * rdma_min_transfer_size                |
|  * rdma_provider instance                |
+------------------+-----------------------+
                   |
    +--------------+---------------+
    |              |               |
+---v-------+ +----v------+ +------v-----------+
|  RDMA     | |  RDMA     | |  RDMA Request    |
| Provider  | |  Buffer   | |  Handler         |
|           | |  Manager  | |                  |
+-----------+ +-----------+ +------------------+
```

### Key Components

#### 1. **RDMA Provider** (`aws_s3_rdma_provider`)
- **Purpose**: Abstracts hardware-specific RDMA operations
- **Responsibilities**:
  - Memory registration/deregistration with RDMA hardware
  - Token generation: Creates opaque tokens encoding buffer address, size, memory keys, and transfer context
  - Token processing: Decodes tokens to perform RDMA read/write operations
  - Memory suitability checks: Validates if memory can be used for RDMA (pinnable, aligned, accessible)
- **Design**: Vtable-based plugin architecture for extensibility
- **Token Format**: Provider-specific, opaque format (typically 80-120 bytes)

#### 2. **RDMA Buffer Manager** (`aws_s3_rdma_buffer_manager`)
- **Purpose**: Manages RDMA buffer lifecycle
- **Responsibilities**:
  - RDMA readiness checks
  - Buffer preparation for RDMA
  - Buffer finalization/teardown after transfers
  - Error handling and cleanup
- **Integration**: Tied to request lifecycle

#### 3. **RDMA Request Handler** (`aws_s3_rdma_request_handler`)
- **Purpose**: Handles RDMA-specific request/response logic
- **Responsibilities**:
  - Request token insertion
  - Response header processing
  - RDMA checksum calculation
  - Content/Object size validation
  - HTTP/RDMA fallback logic

##  Operation Flow

**Note:** The flows below reference configuration options like `user_buffer_options` and internal flags like `rdma_buffer_registered`. These are detailed in the "Implementation Details" section below.

### RDMA PUT Operation

```
1. User initiates PUT with user_buffer_options (user-provided memory buffer)
   |-> Client checks: enable_rdma && buffer_size >= rdma_min_transfer_size
   |
2. Request Preparation (s3_default_meta_request.c)
   |-> Zero-copy buffer assignment (user buffer -> request_body)
   |-> Skip body-read for RDMA-eligible requests
   |
3. Buffer Registration (s3_rdma_buffer_manager.c)
   |-> Check memory suitability
   |-> Register with RDMA provider
   |-> Set rdma_buffer_registered = 1
   |
4. Token Generation (s3_rdma_request_handler.c)
   |-> Generate RDMA request token
   |-> Add x-amz-rdma-token header
   |-> Calculate RDMA checksum (actual buffer content)
   |-> Skip HTTP body assignment (keep request_body intact)
   |
5. Request Signing (SigV4)
   |-> Skip STREAMING-UNSIGNED-PAYLOAD-TRAILER (AWS signature for chunked uploads, incompatible with RDMA)
   |-> Use UNSIGNED-PAYLOAD for empty HTTP body (standard unsigned signature)
   |
6. HTTP Request Sent
   |-> Headers include RDMA token
   |-> HTTP body empty (data transferred via RDMA)
   |
7. Response Processing
   |-> Check for x-amz-rdma-reply header
   |-> Process reply token
   |-> Handle RDMA errors -> fallback to HTTP retry by setting `disable_rdma_on_retry`
   |
8. Cleanup
   |-> Deregister buffer
   \-> Clear rdma_buffer_registered flag
```

### RDMA GET Operation

```
1. User initiates GET with user_buffer_options
   |-> Pre-map user buffer to response_body
   |
2. Request Preparation
   |-> Register buffer for RDMA
   |-> Generate RDMA request token
   |-> Add x-amz-rdma-token header
   |
3. HTTP Request Sent
   |
4. Response Header Processing
   |-> Check for x-amz-rdma-reply header
   |-> Check for x-amz-rdma-bytes-transferred header
   |-> Parse RDMA bytes value (format: single number "10485760" or fraction "10485760/104857600")
   |   |-> Extract actual bytes transferred from RDMA
   |   |-> Update Content-Length to reflect RDMA bytes
   |   \-> Update response_body.len
   |
5. Response Body Handling
   |-> If RDMA succeeded: HTTP body should be empty
   |-> If RDMA failed: Process HTTP body normally (fallback)
   |
6. Checksum Validation
   |-> For RDMA transfers: update checksum with RDMA data
   |-> Use actual transferred bytes from x-amz-rdma-bytes-transferred
   |
7. Progress Reporting
   |-> Report RDMA bytes transferred (not HTTP body length)
   |
8. Cleanup
   \-> Deregister buffer
```

##  Implementation Details

### Client Configuration

**New Configuration Options:**
```c
struct aws_s3_client_config {
    // ... existing fields ...
    
    bool enable_rdma;                    // Enable RDMA globally
    size_t rdma_min_transfer_size;       // Minimum size for RDMA (default: 1MB)
    struct aws_s3_rdma_provider *rdma_provider;  // RDMA provider instance
};
```

### Meta Request Options

**New Options:**
```c
struct aws_s3_meta_request_options {
    // ... existing fields ...
    
    bool use_rdma;                       // Enable RDMA for this request
    struct aws_user_buffer_options *user_buffer_options;  // User-provided buffer
};

struct aws_user_buffer_options {
    void *transfer_buffer;               // Pre-allocated buffer
    size_t transfer_buffer_size;         // Buffer size
};
```

### Request Tracking

**New Request Fields:**
```c
struct aws_s3_request {
    // ... existing fields ...
    
    uint8_t rdma_buffer_registered;      // Buffer registered for RDMA
    uint8_t disable_rdma_on_retry;       // Fallback to HTTP on retry
    uint8_t rdma_transfer_succeeded;     // Server used RDMA
    uint8_t is_user_provided_buffer;     // Skip read step
};
```

##  Key Features

### 1. **Automatic Eligibility Detection**

RDMA is used when ALL conditions are met:
- Client has `enable_rdma = true`
- Meta request has `use_rdma = true`
- Buffer size >= `rdma_min_transfer_size`
- Memory is RDMA-suitable (provider-specific: pinnable, proper alignment, accessible to RDMA hardware)
- Request not marked `disable_rdma_on_retry`

**Note:** Memory suitability is determined by the RDMA provider's `is_memory_suitable()` callback, which checks hardware-specific requirements (e.g., GPU memory for GPUDirect, pinned system memory, proper page alignment).

### 2. **Zero-Copy Buffer Management**

**PUT Operations:**
- User buffer assigned directly to `request_body`
- No allocator set (prevents double-free)
- `is_user_provided_buffer` flag skips file read

**GET Operations:**
- User buffer pre-mapped to `response_body`
- Data written directly to user buffer via RDMA
- HTTP body callback: may receive 0 bytes or not be called (HTTP library dependent)

### 3. **HTTP/RDMA Hybrid Protocol**

**Headers:**
- `x-amz-rdma-token`: Request token from client (opaque, provider-specific)
- `x-amz-rdma-reply`: HTTP status code from server (200/204/206=success, 501=not supported)
- `x-amz-rdma-bytes-transferred`: Actual bytes transferred via RDMA (GET only)
- Standard S3 headers coexist

**Body:**
- HTTP body empty for RDMA transfers
- Data transferred out-of-band via RDMA
- Fallback: for GET operation, server can send data via HTTP body

### 4. **Checksum Handling**

**PUT Operations:**
- Calculate checksum on actual transfer buffer
- Add `x-amz-checksum-*` header with RDMA data checksum
- Skip `Content-MD5` (applies to HTTP body, not RDMA)
- Skip `STREAMING-UNSIGNED-PAYLOAD-TRAILER` (chunked encoding incompatible)

**GET Operations:**
- Update running checksum with RDMA data
- Use `x-amz-rdma-bytes-transferred` for byte count
- Validate against response checksum headers

### 5. **Intelligent Fallback**

**Scenarios:**
- RDMA token generation fails -> HTTP retry
- RDMA reply token invalid -> HTTP retry
- RDMA buffer registration fails -> HTTP transfer
- Server doesn't support RDMA -> HTTP body used
- RDMA transfer error -> HTTP retry

**Mechanism:**
- Set `disable_rdma_on_retry = 1`
- Deregister buffer
- Return error to trigger retry
- Retry uses standard HTTP transfer

### 6. **Environment Variable Override**

```bash
export AWS_S3_FORCE_RDMA=1     # Same as above
```

##  Integration Points

### Auto-Ranged GET (`s3_auto_ranged_get.c`)

- **Buffer Allocation**: Uses user-provided buffer if available
- **Part Distribution**: Each part maps to buffer slice
- **RDMA Token**: Generated per-part
- **Progress Tracking**: Aggregates RDMA bytes from all parts

### Auto-Ranged PUT (`s3_auto_ranged_put.c`)

- **Multipart Upload**: Each part uses RDMA if eligible
- **Part Preparation**: Per-part buffer registration
- **Checksum**: Per-part RDMA checksum calculation
- **Size Validation**: Ensure parts fit in RDMA buffers

### Default Meta Request (`s3_default_meta_request.c`)

- **Single-Request PUT**: Direct user buffer assignment
- **Single-Request GET**: Pre-mapped response buffer
- **Content-MD5**: Skipped for RDMA requests
- **Body Assignment**: Conditional based on RDMA eligibility

##  Transfer Mode Compatibility

### RDMA-Compatible vs Incompatible Modes

RDMA support depends on the transfer mode used for the upload/download:

| Transfer Mode | PUT RDMA Support | GET RDMA Support | Notes |
|---------------|------------------|------------------|-------|
| **User-provided buffer** | Yes | Yes | Optimal - data already in memory, zero-copy |
| **File upload (buffered)** | Yes | N/A | Data read into buffer, then RDMA transfer |
| **File I/O streaming** | No | N/A | Uses `request_body_stream`, incompatible with RDMA |
| **Async writes** | Yes | N/A | Data pre-populated in buffer, RDMA eligible |
| **Standard memory buffer** | Yes | Yes | Memory allocated from pool, RDMA eligible |

### File I/O Streaming (Incompatible with RDMA)

**When active:**
- `fio_opts.should_stream = true` with parallel stream
- Creates `request_body_stream` to read directly from file during HTTP transfer
- Skips buffer allocation and buffering step

**Why incompatible:**
```
Standard buffered flow:
  File -> Read into buffer -> Register buffer -> RDMA transfer

File streaming flow:
  File -> HTTP stream (no buffer available)
          ^
          No buffer to register for RDMA
```

**Code detection (s3_auto_ranged_put.c:1131-1154):**
```c
if (request->fio_streaming) {
    // Create request_body_stream for direct file streaming
    request->request_body_stream = aws_part_streaming_input_stream_new(...);
    // RDMA is automatically skipped - no buffer exists to register
}
```

**Behavior:**
- No RDMA token generated
- Request sent as standard HTTP with body stream
- Data streamed directly from file to HTTP connection
- No error or fallback needed - intentional design

### Async Writes (Compatible with RDMA)

**When active:**
- `request_body_using_async_writes = true`
- Buffer pre-populated via `async_write.buffered_data`

**Code path (s3_auto_ranged_put.c:604-608):**
```c
if (meta_request->synced_data.async_write.ready_to_send) {
    // Async-write already has a buffer
    request->request_body = meta_request->synced_data.async_write.buffered_data;
    request->content_length = request->request_body.len;
}
```

**RDMA flow:**
- Buffer already contains data (no file read needed)
- Buffer registered with RDMA provider
- Token generated and RDMA transfer proceeds normally

##  Error Handling

### RDMA-Specific Errors

**New Error Codes** (defined in `s3.h`):
- `AWS_ERROR_S3_RDMA_INVALID_TOKEN` - Invalid RDMA token
- `AWS_ERROR_S3_RDMA_OPERATION_FAILED` - RDMA transfer failed
- `AWS_ERROR_S3_RDMA_BUFFER_REGISTRATION_FAILED` - Buffer registration failed

### Error Recovery

1. **Buffer Registration Failure** (Early failure during buffer preparation)
   - Log warning (ERROR level for GET, WARN level for PUT)
   - Request continues without RDMA (falls back to standard HTTP immediately)
   - No RDMA token generated or added to headers
   - Data transferred via HTTP body on current attempt
   - No retry needed (current request completes via HTTP)
   - Note: Different from token generation failure which triggers a retry

2. **Token Generation Failure**
   - Abort request preparation and deregister RDMA buffer
   - Set `disable_rdma_on_retry = 1`
   - Return error to trigger automatic retry
   - Retry uses standard HTTP (data in body)

3. **RDMA Transfer Failure (x-amz-rdma-reply indicates failure)**
   - Check HTTP response status and `x-amz-rdma-reply` value
   - GET with 501: Server sent data via HTTP body - process normally (no retry needed)
   - PUT with 501: Server didn't receive data - set `disable_rdma_on_retry = 1` and trigger retry
   - Clean up registered buffers

4. **Content Size Mismatch** (GET operations only)
   - Occurs when `Content-Range` indicates object size exceeds allocated buffer
   - Example: Requested full object, but server returned `Content-Range: bytes 0-X/Y` where Y > buffer_capacity
   - Client validates: actual object size vs. allocated buffer size
   - If mismatch detected: abort current request, switch to ranged GET strategy
   - Subsequent requests use proper range headers to fit buffer constraints

## Performance Considerations

### When RDMA Helps

**Ideal for:**
- Large objects (>= 1 MB default threshold)
- GPU memory transfers (NVIDIA GPUDirect)
- High-throughput workloads
- Low-latency requirements
- Direct memory-to-storage paths

### When to Use HTTP

 **Better with HTTP:**
- Small objects (< 1 MB)
- Network without RDMA support
- S3 endpoints without RDMA capability
- Retry scenarios after RDMA failure

## S3 Wire Protocol Changes

This section documents the exact HTTP header modifications when RDMA is enabled for S3 operations. The changes maintain backward compatibility while enabling out-of-band data transfer via RDMA.

### Overview

When RDMA is active, the HTTP request/response acts as a **control channel** while data flows through **RDMA out-of-band**. The HTTP body is typically empty (Content-Length: 0), and special headers coordinate the RDMA transfer.

**Key RDMA Headers:**
- **Request:** `x-amz-rdma-token` - Opaque token identifying registered RDMA buffer/transfer context
- **Response:** `x-amz-rdma-reply` - HTTP status code indicating RDMA result
  - `200/204/206` = RDMA transfer successful
  - `501` = RDMA not supported - GET: data in HTTP body; PUT: must retry with HTTP
- **Response (GET only):** `x-amz-rdma-bytes-transferred` - Actual number of bytes transferred via RDMA

---

### Quick Reference: `x-amz-rdma-reply` Status Codes

| Status Code | When Present | Meaning |
|-------------|--------------|---------|
| `200` | Successful PUT/GET | RDMA transfer completed successfully |
| `204` | Successful operation | RDMA transfer completed, no content |
| `206` | Successful ranged GET | RDMA partial transfer completed successfully |
| `501` | HTTP 200 + GET | RDMA not supported, server sent data via HTTP body (operation succeeded, no retry) |
| `501` | HTTP 200 + PUT | RDMA transfer failed, body missing (client must retry with HTTP) |
| (absent) | Any | Server doesn't support RDMA protocol, request processed normally via HTTP body |

**Important:** The HTTP response status code and `x-amz-rdma-reply` are evaluated together:
- **HTTP 2xx + `x-amz-rdma-reply: 200/204/206`** -> RDMA success, data in buffer (Content-Length MUST be 0)
- **HTTP 2xx + `x-amz-rdma-reply: 501` + GET** -> Server sent data via HTTP body, process normally (no retry)
- **HTTP 2xx + `x-amz-rdma-reply: 501` + PUT** -> RDMA failed, client must retry with standard HTTP
- **HTTP 2xx + no `x-amz-rdma-reply`** -> Server doesn't support RDMA, process HTTP body normally
- **HTTP 4xx/5xx** -> Standard S3 error (handle normally, `x-amz-rdma-reply` irrelevant)

**Validation Rule:** If `x-amz-rdma-reply` is 200/204/206, then `Content-Length` MUST be 0. Any other value indicates a protocol violation and should be treated as an error.

---

## 1. Single PUT Object (PutObject)

### 1.1 HTTP Request - Standard (No RDMA)

```http
PUT /bucket/object HTTP/1.1
Host: s3.amazonaws.com
Content-Length: 10485760
Content-Type: application/octet-stream
Content-MD5: 1B2M2Y8AsgTpgAmY7PhCfg==
x-amz-checksum-crc32c: AAAAAA==
x-amz-content-sha256: STREAMING-UNSIGNED-PAYLOAD-TRAILER
x-amz-decoded-content-length: 10485760
Content-Encoding: aws-chunked
Transfer-Encoding: chunked

[HTTP body with chunked data: 10485760 bytes]
```

### 1.2 HTTP Request - With RDMA

```http
PUT /bucket/object HTTP/1.1
Host: s3.amazonaws.com
Content-Length: 0
Content-Type: application/octet-stream
x-amz-rdma-token: <opaque-token>
x-amz-checksum-crc32c: AAAAAA==

[Empty HTTP body - data transferred via RDMA: 10485760 bytes]
```

**Headers ADDED:**
- `x-amz-rdma-token`: RDMA request token (provider-generated, typically 80-120 bytes)

**Headers MODIFIED:**
- `Content-Length`: Changed from actual size to `0`

**Headers REMOVED:**
- `Content-MD5`: Removed (applies to HTTP body, not RDMA data)
- `x-amz-content-sha256`: Removed (no streaming payload)
- `x-amz-decoded-content-length`: Removed (no chunked encoding)
- `Content-Encoding`: Removed if it was `aws-chunked` (user-specified encoding like `gzip` is preserved)
- `Transfer-Encoding`: Removed (no chunking)

**Headers PRESERVED:**
- `x-amz-checksum-*`: Kept but **calculated** on RDMA buffer content (not on chunked-encoded stream)
- `Content-Type`: Preserved (object metadata)
- User-specified `Content-Encoding` (e.g., `gzip`): Preserved (object metadata)

### 1.3 HTTP Response - RDMA Success

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
x-amz-request-id: ABC123
ETag: "abc123def456"
Content-Length: 0

[Empty HTTP body]
```

**Headers ADDED (Server):**
- `x-amz-rdma-reply`: HTTP status code indicating RDMA transfer result (200 = success)

**Validation:** When `x-amz-rdma-reply: 200`, Content-Length MUST be 0. Non-zero Content-Length is a protocol error.

### 1.4 HTTP GET Response - RDMA Not Supported (Server Fallback)

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 501
x-amz-request-id: ABC123
ETag: "abc123def456"
Content-Length: 10485760

[HTTP body: 10485760 bytes - server sent data via standard HTTP]
```

**Server Behavior:** Server doesn't support RDMA or declined to use it, sent data via HTTP body instead.

**Client Behavior:**
- Detects `x-amz-rdma-reply: 501` with HTTP 200 for GET operation
- Operation has **already succeeded** - data delivered via HTTP body
- Processes HTTP body normally via body callback
- Sets `disable_rdma_on_retry = 1` (affects this request's retries only, if any subsequent errors occur)
- **No retry needed** - request completes successfully

### 1.5 HTTP PUT Response - RDMA Not Supported (Server Fallback)

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 501
x-amz-request-id: ABC123
ETag: "abc123def456"
Content-Length: 87
Content-Type: application/xml

<?xml version="1.0" encoding="UTF-8"?>
<Error>
  <Code>RdmaTransferFailed</Code>
  <Message>RDMA transfer failed; client must retry with HTTP body</Message>
</Error>
```

**Note:** When RDMA PUT fails, the server returns a 200 status and `x-amz-rdma-reply: 501`, but also includes an HTTP body describing the error (e.g., XML error document), and `Content-Length` reflects the size of this error response.

**Client Behavior:** When `x-amz-rdma-reply: 501` with successful HTTP status (200/204/206):
- For GET operation has **already succeeded** (no retry needed). process the http body
- For PUT: RDMA transfer failed, sets `disable_rdma_on_retry = 1` and request is retried with http

### 1.6 HTTP Response - Server Error

```http
HTTP/1.1 400 Bad Request
Content-Length: 234
Content-Type: application/xml

<?xml version="1.0" encoding="UTF-8"?>
<Error>
  <Code>InvalidRequest</Code>
  <Message>Invalid RDMA token</Message>
</Error>
```

**Client Behavior:** Handle as standard S3 error response (may retry based on error type)

---

## 2. Multipart Upload (UploadPart)

### 2.1 HTTP Request - Standard (No RDMA)

```http
PUT /bucket/object?partNumber=1&uploadId=xyz HTTP/1.1
Host: s3.amazonaws.com
Content-Length: 10485760
Content-MD5: 1B2M2Y8AsgTpgAmY7PhCfg==
x-amz-checksum-crc32c: AAAAAA==
x-amz-content-sha256: STREAMING-UNSIGNED-PAYLOAD-TRAILER
x-amz-decoded-content-length: 10485760
Content-Encoding: aws-chunked
Transfer-Encoding: chunked

[HTTP body: 10485760 bytes]
```

### 2.2 HTTP Request - With RDMA

```http
PUT /bucket/object?partNumber=1&uploadId=xyz HTTP/1.1
Host: s3.amazonaws.com
Content-Length: 0
x-amz-rdma-token: <opaque-token-part1>
x-amz-checksum-crc32c: AAAAAA==

[Empty HTTP body - data transferred via RDMA]
```

**Headers ADDED:**
- `x-amz-rdma-token`: **Unique token per part** (each part gets its own token)

**Headers MODIFIED:**
- `Content-Length`: `10485760` to `0`

**Headers REMOVED:**
- `Content-MD5`: Removed
- `x-amz-content-sha256`: Removed
- `x-amz-decoded-content-length`: Removed
- `Content-Encoding`: `aws-chunked` removed
- `Transfer-Encoding`: Removed

**Headers PRESERVED:**
- `x-amz-checksum-crc32c`: **Calculated per part** on RDMA buffer content (not on chunked stream)
- Query parameters (`partNumber`, `uploadId`): Unchanged

### 2.3 HTTP Response - RDMA Success (Per Part)

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
ETag: "part1-etag-abc123"
Content-Length: 0

[Empty HTTP body]
```

**Note:** Each part receives `x-amz-rdma-reply` status code (200 = success). ETags are used in CompleteMultipartUpload.

### 2.4 CompleteMultipartUpload - Unchanged

```http
POST /bucket/object?uploadId=xyz HTTP/1.1
Host: s3.amazonaws.com
Content-Length: <xml-size>
Content-Type: application/xml

<CompleteMultipartUpload>
  <Part>
    <PartNumber>1</PartNumber>
    <ETag>"part1-etag-abc123"</ETag>
  </Part>
  ...
</CompleteMultipartUpload>
```

**No RDMA-specific changes** - uses ETags from UploadPart responses

---

## 3. Single GET Object (GetObject)

### 3.1 HTTP Request - Standard (No RDMA)

```http
GET /bucket/object HTTP/1.1
Host: s3.amazonaws.com
x-amz-checksum-mode: ENABLED
Accept-Encoding: identity
```

### 3.2 HTTP Request - With RDMA

```http
GET /bucket/object HTTP/1.1
Host: s3.amazonaws.com
x-amz-rdma-token: <opaque-token>
x-amz-checksum-mode: ENABLED
Accept-Encoding: identity
Content-Length: 0
```

**Headers ADDED:**
- `x-amz-rdma-token`: RDMA request token

**Headers MODIFIED:**
- `Content-Length`: May be set to `0` if implementation sends it (GET requests typically have no body)

**Headers REMOVED:**
- `Transfer-Encoding`: Removed if present

**Note:** The `Content-Length: 0` in the request example is for documentation clarity. GET requests typically don't include request bodies, so this header may be omitted.

### 3.3 HTTP Response - Standard (No RDMA)

```http
HTTP/1.1 200 OK
Content-Length: 10485760
Content-Type: application/octet-stream
ETag: "abc123"
x-amz-checksum-crc32c: AAAAAA==
Last-Modified: Mon, 13 Oct 2025 09:00:00 GMT

[HTTP body: 10485760 bytes]
```

### 3.4 HTTP Response - With RDMA

```http
HTTP/1.1 200 OK
Content-Length: 0
Content-Type: application/octet-stream
ETag: "abc123"
x-amz-rdma-reply: 200
x-amz-rdma-bytes-transferred: 10485760
x-amz-checksum-crc32c: AAAAAA==
Last-Modified: Mon, 13 Oct 2025 09:00:00 GMT

[Empty HTTP body - data transferred via RDMA]
```

**Headers ADDED (Server):**
- `x-amz-rdma-reply`: HTTP status code (200 = RDMA success, 501 = RDMA not supported)
- `x-amz-rdma-bytes-transferred`: Actual bytes transferred via RDMA
  - Format: `<transferred_bytes>` or `<transferred_bytes>/<total_bytes>`
  - Example: `10485760` or `10485760/10485760`
  - Client should parse the first number for actual bytes received

**Headers MODIFIED (Server):**
- `Content-Length`: **MUST be 0** when `x-amz-rdma-reply: 200` (HTTP body is empty, data via RDMA)

**Validation:** If `x-amz-rdma-reply: 200`, Content-Length MUST be 0. Any other value is a protocol error.

**Headers PRESERVED:**
- `x-amz-checksum-crc32c`: Checksum of RDMA data (validated by client)
- All object metadata headers unchanged

**Client Processing:**
- HTTP body callback receives **0 bytes** (not called, or called with empty data)
- Client updates checksum with RDMA data directly from registered buffer
- Progress reports use `x-amz-rdma-bytes-transferred` value

### 3.5 HTTP Response - RDMA Not Supported (501)

```http
HTTP/1.1 200 OK
Content-Length: 10485760
Content-Type: application/octet-stream
ETag: "abc123"
x-amz-rdma-reply: 501
x-amz-checksum-crc32c: AAAAAA==

[HTTP body: 10485760 bytes - delivered via standard HTTP]
```

**Server behavior:** Server doesn't support RDMA or RDMA failed, sent data via HTTP body instead

**Client behavior:** 
- Detects `x-amz-rdma-reply: 501` with HTTP 200
- Processes HTTP body via body callback
- Sets `disable_rdma_on_retry = 1` (affects this request's retries only, if any subsequent errors occur)
- No retry needed - data delivered successfully

---

## 4. Ranged GET (Multi-Part GET)

### 4.1 Initial Discovery Request - Option A: HEAD

```http
HEAD /bucket/object HTTP/1.1
Host: s3.amazonaws.com
```

**Response:**
```http
HTTP/1.1 200 OK
Content-Length: 104857600
ETag: "abc123"
x-amz-checksum-crc32c: AAAAAA==
```

**No RDMA impact** - HEAD request unchanged

### 4.2 Initial Discovery Request - Option B: GET with partNumber=1 (Disabled for RDMA)

**NOTE:** This optimization is **disabled when RDMA is enabled** to avoid size mismatch issues.

### 4.3 Ranged GET Request - Standard (No RDMA)

```http
GET /bucket/object HTTP/1.1
Host: s3.amazonaws.com
Range: bytes=0-10485759
x-amz-checksum-mode: ENABLED
```

**Response:**
```http
HTTP/1.1 206 Partial Content
Content-Range: bytes 0-10485759/104857600
Content-Length: 10485760
ETag: "abc123"

[HTTP body: 10485760 bytes for this range]
```

### 4.4 Ranged GET Request - With RDMA (Part 1)

```http
GET /bucket/object HTTP/1.1
Host: s3.amazonaws.com
Range: bytes=0-10485759
x-amz-rdma-token: <opaque-token-part1>
x-amz-checksum-mode: ENABLED
Content-Length: 0
```

**Response:**
```http
HTTP/1.1 206 Partial Content
Content-Range: bytes 0-10485759/104857600
Content-Length: 0
ETag: "abc123"
x-amz-rdma-reply: 206
x-amz-rdma-bytes-transferred: 10485760
x-amz-checksum-crc32c: <part1-checksum>

[Empty HTTP body - data transferred via RDMA]
```

### 4.5 Ranged GET Request - With RDMA (Part 2)

```http
GET /bucket/object HTTP/1.1
Host: s3.amazonaws.com
Range: bytes=10485760-20971519
x-amz-rdma-token: <opaque-token-part2>
x-amz-checksum-mode: ENABLED
Content-Length: 0
If-Match: "abc123"
```

**Headers ADDED:**
- `If-Match`: ETag from first part (ensures object consistency across parts)

**Response:**
```http
HTTP/1.1 206 Partial Content
Content-Range: bytes 10485760-20971519/104857600
Content-Length: 0
ETag: "abc123"
x-amz-rdma-reply: 206
x-amz-rdma-bytes-transferred: 10485760

[Empty HTTP body]
```

**Client Behavior:**
- Each part gets a **unique RDMA token**
- Client maps each part to a **slice of user buffer**
- Checksums validated per-part
- `If-Match` ensures object hasn't changed between parts

---

## 5. Checksum Handling

### 5.1 PUT Operations - Checksum Calculation

**Standard HTTP:**
```
1. Read file into buffer
2. Apply aws-chunked encoding wrapper
3. Calculate checksum on encoded stream
4. Add: x-amz-checksum-crc32c: <checksum-of-encoded-stream>
5. Add: Content-MD5: <md5-of-encoded-stream>
6. Send chunked HTTP body
```

**With RDMA:**
```
1. Buffer already contains data (user buffer or file read)
2. Calculate checksum on RAW buffer (no encoding)
3. Add: x-amz-checksum-crc32c: <checksum-of-raw-buffer>
4. Skip: Content-MD5 (HTTP body is empty)
5. Skip: aws-chunked encoding
6. Set Content-Length: 0
7. Add RDMA token
8. Send empty HTTP body (data via RDMA)
```

**Key Difference:** RDMA checksum is on **raw data**, not encoded data

### 5.2 GET Operations - Checksum Validation

**Standard HTTP:**
```
1. Receive HTTP body chunks
2. Update running checksum per chunk
3. Compare with x-amz-checksum-crc32c header
```

**With RDMA:**
```
1. Receive x-amz-rdma-bytes-transferred header
2. Update running checksum with data from RDMA buffer
3. Use exact byte count from x-amz-rdma-bytes-transferred
4. Compare with x-amz-checksum-crc32c header
5. HTTP body callback: May be called with 0 bytes or not called at all (HTTP library dependent)
   - If called, data->len will be 0
   - Application must handle both cases (not called, or called with 0 bytes)
```

### 5.3 Supported Checksum Algorithms

All S3 flexible checksums supported for RDMA:
- `x-amz-checksum-crc32`
- `x-amz-checksum-crc32c`
- `x-amz-checksum-sha1`
- `x-amz-checksum-sha256`

**Algorithm Selection:** Same as standard S3 (specified in `checksum_config`)

---

## 6. Encoding and Content-Encoding

### 6.1 PUT Operations - Encoding Behavior

**Standard HTTP with Trailing Checksum:**
```http
Content-Encoding: aws-chunked
Transfer-Encoding: chunked
x-amz-content-sha256: STREAMING-UNSIGNED-PAYLOAD-TRAILER
x-amz-decoded-content-length: 10485760
Content-Length: <chunked-size-with-trailers>
```

**RDMA (No Chunked Encoding):**
```http
Content-Length: 0
x-amz-checksum-crc32c: AAAAAA==
```

**RDMA with User Content-Encoding (gzip):**
```http
Content-Length: 0
Content-Encoding: gzip
x-amz-checksum-crc32c: AAAAAA==
```

**Important:** 
- `aws-chunked` is **always removed** for RDMA (it's AWS-specific encoding for trailers)
- User-specified `Content-Encoding` (e.g., `gzip`, `deflate`) is **preserved** (object metadata)

### 6.2 GET Operations - Encoding Behavior

**No special handling** - object's stored `Content-Encoding` is returned in response headers regardless of RDMA usage.

---

## 7. Signing and Authentication

### 7.1 SigV4 Signing - Standard HTTP

```
Canonical Request includes:
- HTTP body content (for non-streaming)
- x-amz-content-sha256: STREAMING-UNSIGNED-PAYLOAD-TRAILER (for streaming)
- Content-Length: <actual-size>
```

### 7.2 SigV4 Signing - RDMA

```
Canonical Request includes:
- HTTP body content: <empty>
- x-amz-content-sha256: UNSIGNED-PAYLOAD (standard unsigned)
- Content-Length: 0
- x-amz-rdma-token: <opaque-token> (included in signed headers)
```

**Key Changes:**
- RDMA uses `UNSIGNED-PAYLOAD` instead of `STREAMING-UNSIGNED-PAYLOAD-TRAILER`
- `x-amz-rdma-token` is included in signed headers list
- HTTP body is empty, so payload hash is SHA256 of empty string: `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
- Actual data transferred via RDMA is NOT included in signature (transferred out-of-band)

### 7.3 Implementation Detail

The flag `require_streaming_unsigned_payload_header` is **cleared** for RDMA requests before signing:

```c
request->send_data.require_streaming_unsigned_payload_header = 0;
```

This prevents the signer from adding `STREAMING-UNSIGNED-PAYLOAD-TRAILER` which is incompatible with RDMA.

---

## 8. Error Handling and Fallback

### 8.0 RDMA Setup and Token Generation Failures

**Pre-Request Failure Scenario:**

When RDMA token generation fails BEFORE the request is sent (e.g., provider error, memory registration issue), the client automatically retries with standard HTTP:

**Flow:**
```
1. Buffer registration may succeed or fail (failure logged as warning, continues)
2. Data is filled into buffer
3. Token generation attempted (prepare_put_token or prepare_get_token)
   \-> If token generation FAILS:
       |-> Deregister buffer if it was registered
       |-> Set disable_rdma_on_retry = 1
       |-> Return AWS_ERROR_S3_INTERNAL_ERROR
       \-> Triggers automatic retry
4. On retry:
   \-> disable_rdma_on_retry flag prevents RDMA
   \-> Request sent as standard HTTP (data in body)
```

**Code behavior (s3_rdma_request_handler.c:600-622):**
```c
if (rdma_result != 0) {
    // Token generation failed
    AWS_LOGF_ERROR("RDMA token generation failed (result=%d), will retry with HTTP", rdma_result);
    
    // Clean up registered buffer
    if (request->rdma_buffer_registered) {
        aws_s3_rdma_provider_deregister_memory(provider, buffer);
        request->rdma_buffer_registered = 0;
    }
    
    // Disable RDMA for retry
    request->disable_rdma_on_retry = 1;
    
    // Return error to trigger retry (next attempt uses HTTP with body)
    return aws_raise_error(AWS_ERROR_S3_INTERNAL_ERROR);
}
```

**Important:** This failure happens BEFORE the HTTP request is sent, so the server never sees the failed RDMA attempt. The retry is transparent to the server.

**User Impact:** 
- Single request may appear as 2 attempts (1 failed RDMA setup + 1 HTTP success)
- Data is safely transferred via HTTP fallback
- No data loss or corruption

---

### 8.1 RDMA Success Response

**Server successfully completed RDMA transfer:**
```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
Content-Length: 0

[Empty HTTP body - data transferred via RDMA]
```

**`x-amz-rdma-reply` values for success:**
- `200` - Successful PUT/GET (Content-Length MUST be 0)
- `204` - Successful operation with no content (Content-Length MUST be 0)
- `206` - Successful partial content (ranged GET) (Content-Length MUST be 0)

**Critical Validation:** For all RDMA success codes (200/204/206), Content-Length MUST be 0. Non-zero Content-Length with these codes indicates a protocol error and the request should fail.

### 8.2 RDMA Transfer Failed (PUT with 501)

**Server indicates RDMA transfer failed:**
```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 501
Content-Length: 0
ETag: "abc123"

[Empty HTTP body]
```

**What happened:** 
- Client sent empty HTTP body (expected RDMA transfer)
- Server returned HTTP 200 but RDMA token indicates failure (501)
- HTTP body was empty, so server has no data

**Client behavior:**
1. Detect `x-amz-rdma-reply: 501` with PUT operation
2. Set `disable_rdma_on_retry = 1`
3. **Retry the request with standard HTTP** (send data in HTTP body)
4. This is NOT a final success - retry is required to actually transfer the data

### 8.3 RDMA Not Supported - Server Fallback (GET with 501)

**Server doesn't support RDMA, sends data via HTTP body:**
```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 501
Content-Length: 10485760

[Server sends data via HTTP body as fallback]
```

**What happened:**
- Client sent RDMA token in request
- Server doesn't support RDMA or declined to use it
- Server sent data via HTTP body instead (fallback)

**Client behavior:**
1. Detect `x-amz-rdma-reply: 501` with GET operation
2. Process HTTP body normally via body callback
3. Set `disable_rdma_on_retry = 1` (affects this request's retries only)
4. **No retry needed** - data delivered successfully via HTTP body
5. Operation completes successfully

### 8.4 Server Error Response

**Server returns error (invalid token, access denied, etc.):**
```http
HTTP/1.1 400 Bad Request
Content-Length: <error-xml-size>
Content-Type: application/xml

<?xml version="1.0" encoding="UTF-8"?>
<Error>
  <Code>InvalidArgument</Code>
  <Message>Invalid RDMA token format</Message>
</Error>
```

**Client behavior:**
1. HTTP status code indicates failure (4xx, 5xx)
2. Handle as standard S3 error response
3. May retry based on error type and retry policy
4. RDMA may be disabled on retry depending on error

### 8.5 Server Doesn't Support RDMA (No Header)

**No RDMA headers in response:**
```http
HTTP/1.1 200 OK
Content-Length: 10485760

[HTTP body with data]
```

**Client behavior:**
- Missing `x-amz-rdma-reply` header is not detected as an error
- Process HTTP body normally
- Request completes successfully
- **Note:** Client does NOT automatically disable RDMA for future requests (implementation choice)

### 8.6 RDMA Reply Status Code Summary

The `x-amz-rdma-reply` header contains an HTTP status code indicating the RDMA operation result.

**Note:** Some validation rules mentioned below are documented requirements but not yet fully implemented. See Section 15 for TODO items.

| `x-amz-rdma-reply` Value | HTTP Status | Meaning | Client Action |
|--------------------------|-------------|---------|---------------|
| `200` | 200 OK | RDMA transfer successful (Content-Length MUST be 0) | Validate Content-Length=0, complete request, deregister buffer |
| `204` | 204 No Content | RDMA transfer successful (Content-Length MUST be 0) | Validate Content-Length=0, complete request, deregister buffer |
| `206` | 206 Partial Content | RDMA partial transfer successful (Content-Length MUST be 0) | Validate Content-Length=0, complete request, deregister buffer |
| `501` | 200 + GET | RDMA not supported, server sent data via HTTP body | Process HTTP body, disable RDMA for future, complete successfully (no retry) |
| `501` | 200 + PUT | RDMA transfer failed, body was missing | Disable RDMA, **retry with standard HTTP** to transfer data |
| (absent) | 200/204/206 | Server doesn't support RDMA at all | Process HTTP body normally, complete request successfully |
| (any) | 4xx/5xx | Server error | Handle as standard S3 error, may retry based on error type |

**Key Decision Logic:**

```
if (HTTP status code is 4xx or 5xx) {
    // Server error - handle as normal S3 error
    handle_s3_error(response);
    // May retry based on error type and retry policy
}
else if (x-amz-rdma-reply == 501) {
    // RDMA not supported or failed
    disable_rdma_on_retry = 1;
    
    if (GET operation) {
        // Server sent data via HTTP body (fallback)
        // Operation succeeded - just process the body
        process_http_body_callback(response);
        complete_request(SUCCESS);  // No retry needed
    }
    else if (PUT operation) {
        // RDMA transfer failed - body was missing
        // HTTP body was empty, server has no data
        // MUST retry with standard HTTP to transfer data
        trigger_retry_with_http();  // Will send data in HTTP body
    }
}
else if (x-amz-rdma-reply == 200 || 204 || 206) {
    // RDMA transfer successful
    // VALIDATION: Content-Length MUST be 0
    if (Content-Length != 0) {
        AWS_LOGF_ERROR("Protocol violation: x-amz-rdma-reply=%d but Content-Length=%llu (must be 0)",
                       rdma_reply, content_length);
        return AWS_ERROR_S3_RDMA_PROTOCOL_VIOLATION;
    }
    // Data is in registered buffer (not HTTP body)
    validate_checksums();
    complete_request(SUCCESS);
}
else if (x-amz-rdma-reply is absent) {
    // Server doesn't support RDMA or ignored the token
    // No error - just process HTTP body normally
    process_http_body_callback(response);
    complete_request(SUCCESS);
    // Note: RDMA NOT automatically disabled for future requests
}
```

---

## 9. Protocol Validation Rules

### 9.1 Critical Validation: Content-Length for RDMA Success

**Rule:** When `x-amz-rdma-reply` is 200, 204, or 206, `Content-Length` **MUST be 0**.

**Rationale:**
- RDMA success codes indicate data was transferred via RDMA, not HTTP body
- HTTP body should be empty
- Non-zero Content-Length contradicts RDMA success status

**Client Behavior on Violation:** (TODO)
```c
if (x_amz_rdma_reply == 200 || x_amz_rdma_reply == 204 || x_amz_rdma_reply == 206) {
    if (content_length != 0) {
        // Protocol violation - fail the request
        AWS_LOGF_ERROR("RDMA protocol violation: x-amz-rdma-reply=%d but Content-Length=%llu (expected 0)",
                       x_amz_rdma_reply, content_length);
        deregister_buffer();
        return AWS_ERROR_S3_RDMA_PROTOCOL_VIOLATION;
    }
    // Valid - proceed with RDMA data
}
```

**Examples:**

**Valid:**
```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
Content-Length: 0
x-amz-rdma-bytes-transferred: 10485760
```

**Invalid (Protocol Violation):**
```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
Content-Length: 10485760    <- ERROR: Must be 0
x-amz-rdma-bytes-transferred: 10485760
```

### 9.2 Additional Validation Rules (Recommended, Not All Implemented)

**Rule: x-amz-rdma-bytes-transferred with RDMA Success (TODO - See Section 15.1)**
- If `x-amz-rdma-reply: 200/206` for GET, `x-amz-rdma-bytes-transferred` should be present
- If absent, behavior is undefined (needs implementation decision: fail request or use buffer capacity as fallback)

**Rule: Consistent Status Codes (TODO - See Section 15.1)**
- HTTP status code should match RDMA reply code in success cases (not currently validated)
- `x-amz-rdma-reply: 200` -> HTTP status should be 200
- `x-amz-rdma-reply: 206` -> HTTP status should be 206
- `x-amz-rdma-reply: 204` -> HTTP status should be 204

**Rule: 501 with Content-Length (Expectation, Not Enforced)**
- `x-amz-rdma-reply: 501` for GET -> Content-Length > 0 expected (server sends data in body)
- `x-amz-rdma-reply: 501` for PUT -> Content-Length may vary (server may send error XML)
- Note: Client handles both cases gracefully, doesn't strictly validate Content-Length value for 501

---

## 10. Protocol State Machine

### 10.1 PUT Operation State Flow

```
+----------------------------------------------------------+
| 1. Client Prepares Request                               |
|    - Buffer ready (file read or user buffer)             |
|    - Check: size >= rdma_min_transfer_size               |
|    - Check: memory RDMA-suitable                         |
+-------------------+--------------------------------------+
                    |
                    v
+----------------------------------------------------------+
| 2. Register Buffer with RDMA Provider                    |
|    - rdma_buffer_registered = 1                          |
|    - Generate RDMA token                                 |
+-------------------+--------------------------------------+
                    |
                    v
+----------------------------------------------------------+
| 3. Prepare HTTP Headers                                  |
|    - Add: x-amz-rdma-token                               |
|    - Set: Content-Length: 0                              |
|    - Remove: Content-MD5, Transfer-Encoding, etc.        |
|    - Calculate: x-amz-checksum-* on raw buffer           |
+-------------------+--------------------------------------+
                    |
                    v
+----------------------------------------------------------+
| 4. Sign Request (SigV4)                                  |
|    - Sign with empty HTTP body                           |
|    - Include x-amz-rdma-token in signed headers          |
+-------------------+--------------------------------------+
                    |
                    v
+----------------------------------------------------------+
| 5. Send HTTP Request (empty body)                        |
|    - RDMA provider transfers data out-of-band            |
+-------------------+--------------------------------------+
                    |
            +-------+--------+
            |                |
            v                v
+-------------------+  +---------------------+
| 6a. RDMA Success  |  | 6b. RDMA Failed     |
| - HTTP: 200       |  | - HTTP: 200         |
| - x-amz-rdma-     |  | - x-amz-rdma-reply: |
|   reply: 200      |  |   501               |
+---------+---------+  +-----------+---------+
          |                        |
          v                        v
+-------------------+  +---------------------+
| 7a. Deregister    |  | 7b. Deregister +    |
|     Complete      |  |     Disable RDMA +  |
+-------------------+  |     RETRY           |
                       | - disable_rdma_on_  |
                       |   retry = 1         |
                       +-----------+---------+
                                   |
                                   v
                       +---------------------+
                       | 8. Retry with HTTP  |
                       | - Send data in body |
                       | - No RDMA token     |
                       +---------------------+
```

### 10.2 GET Operation State Flow

```
+----------------------------------------------------------+
| 1. Client Prepares Request                               |
|    - Allocate response buffer or use user buffer         |
|    - Check: size >= rdma_min_transfer_size               |
+-------------------+--------------------------------------+
                    |
                    v
+----------------------------------------------------------+
| 2. Register Buffer with RDMA Provider                    |
|    - rdma_buffer_registered = 1                          |
|    - Generate RDMA token                                 |
+-------------------+--------------------------------------+
                    |
                    v
+----------------------------------------------------------+
| 3. Add RDMA Token to Request                             |
|    - Add: x-amz-rdma-token                               |
|    - Set: Content-Length: 0 (request body)               |
+-------------------+--------------------------------------+
                    |
                    v
+----------------------------------------------------------+
| 4. Send HTTP Request                                     |
+-------------------+--------------------------------------+
                    |
            +-------+--------+
            |                |
            v                v
+-------------------+  +---------------------+
| 5a. RDMA Success  |  | 5b. RDMA Not Sup.   |
| Headers:          |  | Headers:            |
| - HTTP: 200       |  | - HTTP: 200         |
| - x-amz-rdma-     |  | - x-amz-rdma-reply: |
|   reply: 200      |  |   501               |
| - x-amz-rdma-     |  | - Content-Length: N |
|   bytes: N        |  | Body:               |
| - Content-        |  | - Data via HTTP     |
|   Length: 0       |  +-----------+---------+
| Body: empty       |              |
+---------+---------+              v
          |            +---------------------+
          v            | 6b. Process HTTP    |
+-------------------+  |     Body Callback   |
| 6a. Update        |  | - Disable RDMA for  |
| Checksum with     |  |   future requests   |
| RDMA Data         |  +---------------------+
| - Use rdma_bytes  |
| - From buffer     |
+---------+---------+
          |
          v
+-------------------+
| 7. Validate       |
|    Checksum       |
+---------+---------+
          |
          v
+-------------------+
| 8. Deregister     |
|    Complete       |
+-------------------+
```

---

## 11. Size and Threshold Behavior

### 11.1 Size Decisions

**RDMA Eligibility Check (PUT):**
```
eligible = (buffer_size >= rdma_min_transfer_size) &&
           (buffer_size > 0) &&
           (buffer != NULL) &&
           (memory_is_rdma_suitable)
```

**Default:** `rdma_min_transfer_size = 1048576` (1 MB)

### 11.2 Partial Object Handling

**Scenario:** 500 KB file, threshold = 1 MB

**Request:**
```http
PUT /bucket/small-file HTTP/1.1
Content-Length: 512000
Content-MD5: <md5>

[HTTP body: 512000 bytes - standard HTTP, no RDMA]
```

**No RDMA headers** - falls below threshold

### 11.3 Mixed Mode (Multipart)

**Scenario 1:** 15 MB upload, part_size = 5 MB, threshold = 1 MB

- **Part 1:** 5 MB → RDMA (>= 1 MB)
- **Part 2:** 5 MB → RDMA (>= 1 MB)
- **Part 3:** 5 MB → RDMA (>= 1 MB)

**All parts use RDMA** (each part independently evaluated)

**Scenario 2:** 3 MB upload, part_size = 5 MB, threshold = 1 MB, multipart_upload_threshold = 10 MB

- **Single PUT:** 3 MB → RDMA (>= 1 MB threshold, below multipart threshold)

**Scenario 3:** Mixed RDMA and HTTP - 12.5 MB upload, part_size = 5 MB, threshold = 2 MB

- **Part 1:** 5 MB → RDMA (>= 2 MB threshold)
- **Part 2:** 5 MB → RDMA (>= 2 MB threshold)
- **Part 3:** 2.5 MB → HTTP (< 2 MB threshold)

**Note:** The last part falls below threshold and uses standard HTTP. This is supported - RDMA and HTTP parts can coexist in the same multipart upload.

---

## 12. Content-Range and Partial Content

### 12.1 GET with Range - RDMA Response

```http
HTTP/1.1 206 Partial Content
Content-Range: bytes 0-10485759/104857600
Content-Length: 0
x-amz-rdma-reply: 206
x-amz-rdma-bytes-transferred: 10485760
```

**Client Validation:**
1. Check `x-amz-rdma-reply: 206` (RDMA success for partial content)
2. Parse `Content-Range`: start=0, end=10485759, total=104857600
3. Validate: `(end - start + 1) == rdma_bytes_transferred`
4. Validate: `rdma_bytes_transferred <= user_buffer_capacity`

**Size Mismatch Handling:**
- If `rdma_bytes_transferred > user_buffer_capacity` → Error
- If `rdma_bytes_transferred < expected` → Error + Retry

### 12.2 Initial GET - Size Discovery

**For RDMA, partNumber=1 optimization is DISABLED:**

```c
if (auto_ranged_get->object_size_hint <= meta_request->part_size &&
    !(meta_request->client->enable_rdma && meta_request->use_rdma)) {
    return AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1;
}
```

**Reason:** `partNumber=1` returns only first part of multipart object, causing size mismatch with RDMA buffer allocation.

**RDMA uses instead:**
- HEAD request (preferred for RDMA)
- Range-based GET from the start

---

## 13. Progress and Metrics

### 13.1 Progress Reporting - GET

**Standard HTTP:**
```
bytes_transferred = response_body.len
content_length = response_body.len
```

**RDMA:**
```
bytes_transferred = parsed from x-amz-rdma-bytes-transferred header
content_length = set from x-amz-rdma-bytes-transferred header  
```

**Why different:** RDMA data bypasses response_body and HTTP body is empty, so client must use the `x-amz-rdma-bytes-transferred` header value to report progress.

---

## 14. Wire Protocol Summary Table

| Aspect | Standard HTTP | RDMA |
|--------|---------------|------|
| **Request Content-Length** (PUT) | Actual data size | 0 |
| **Response Content-Length** (GET) | Actual data size | 0 (HTTP body empty) |
| **Request Body** | Contains data | Empty |
| **Response Body** | Contains data | Empty |
| **RDMA Token Header** | None | `x-amz-rdma-token` (request) |
| **RDMA Reply Header** | None | `x-amz-rdma-reply` (HTTP status code: 200/204/206=success, 501=not supported) |
| **RDMA Bytes Header** | None | `x-amz-rdma-bytes-transferred` (GET response) |
| **Content-MD5** | Calculated on HTTP body | Removed |
| **x-amz-checksum-*** | Calculated on encoded stream | Calculated on raw buffer |
| **Content-Encoding** | May include `aws-chunked` | `aws-chunked` removed, user encoding preserved |
| **Transfer-Encoding** | May be `chunked` | Removed |
| **x-amz-content-sha256** | `STREAMING-UNSIGNED-PAYLOAD-TRAILER` | Not added |
| **x-amz-decoded-content-length** | Present for chunked | Removed |
| **Signing** | Signs body or streaming | Signs empty body with token |
| **Data Transfer** | In HTTP body | Out-of-band via RDMA |
| **Fallback (501)** | N/A | GET: Server sends via HTTP body (no retry); PUT: Client retries with HTTP body |

---

## 15. TODO and Future Enhancements

This section documents validation rules and features that are specified in this document but may not yet be fully implemented in the current codebase.

### 15.1 Protocol Validation (Not Yet Implemented)

**Content-Length Validation on RDMA Success (Section 9.1)**
- **Status**: Documented requirement, implementation pending
- **Description**: Client should validate that `Content-Length` is 0 when `x-amz-rdma-reply` is 200/204/206
- **Action on Violation**: Fail request with `AWS_ERROR_S3_RDMA_PROTOCOL_VIOLATION`
- **Code Location**: Needs implementation in `s3_rdma_request_handler.c` response processing
- **Priority**: High - prevents protocol violations

**x-amz-rdma-bytes-transferred Validation (Section 9.2)**
- **Status**: Behavior undefined if absent
- **Description**: When `x-amz-rdma-reply: 200/206` for GET, `x-amz-rdma-bytes-transferred` should be present
- **Action if Absent**: Should fail request or use buffer capacity as fallback (not currently specified)
- **Code Location**: `s3_rdma_request_handler.c:s_default_process_response_headers`
- **Priority**: Medium - handles edge case

**HTTP Status Code Consistency Check (Section 9.2)**
- **Status**: Not validated
- **Description**: HTTP status should match `x-amz-rdma-reply` (e.g., both should be 200, or both 206)
- **Action on Mismatch**: Currently undefined - should log warning or fail
- **Priority**: Low - defensive validation

### 15.2 Error Handling Enhancements

**Specific Error Code for Protocol Violations**
- **Status**: Referenced but may not be defined
- **Error Code**: `AWS_ERROR_S3_RDMA_PROTOCOL_VIOLATION`
- **Usage**: For Content-Length validation failure, token format errors, header inconsistencies
- **Location**: Should be added to `include/aws/s3/s3.h` error definitions

**Fine-Grained disable_rdma_on_retry Logic**
- **Current**: Simple flag set on various failures
- **Enhancement**: Could differentiate between:
  - Transient RDMA failures (retry with RDMA later)
  - Provider/hardware failures (disable permanently for session)
  - Server-side 501 (disable for this endpoint/object)
- **Priority**: Low - current behavior is safe

### 15.3 Performance and Metrics

**RDMA Transfer Statistics**
- **Status**: Not tracked
- **Potential Additions**:
  - Number of successful RDMA vs HTTP transfers
  - Bytes transferred via RDMA vs HTTP
  - Average RDMA setup time
  - RDMA failure rates by error type
- **Use Case**: Performance monitoring, debugging

**Progress Reporting Accuracy**
- **Current**: Progress callback uses `x-amz-rdma-bytes-transferred`
- **Enhancement**: More granular progress during RDMA transfer (if provider supports)
- **Priority**: Low - current approach works

### 15.4 Documentation Clarifications Needed

**"disable_rdma_on_retry" Propagation Scope (Section 8)**

- **Current Limitation**: When an RDMA protocol violation or fallback occurs, the request is failed, but this status is not currently propagated up to the meta_request or to the S3 client. The disable_rdma_on_retry flag and related error do not influence future meta_request logic or client state.
- **Enhancement Needed**: The RDMA disablement/violation should be visibly and programmatically propagated up through the meta_request to the S3 client layer. This would enable client code to react appropriately (e.g., mark the meta_request as failed due to RDMA protocol, automatically switch future requests to HTTP, or alert the user/admin).
- **Clarification**: Clearly define and document whether disablement is meant to last for:
  - The single request/retry only (current behavior)
  - The entire meta_request (all parts of a multipart operation, for example)
  - The full lifetime of the client/session, possibly persisted across restarts
- **Recommended Action**: 
  - Update implementation to propagate RDMA protocol violation/failure status to the meta_request and client API.
  - Document intended disablement scope and error reporting policy in both code and public documentation.
  - Add callbacks or error codes so client applications can detect and respond to RDMA protocol issues.


### 15.5 Provider Interface Enhancements

**Memory Suitability Criteria**
- **Current**: Provider-specific via `is_memory_suitable()` callback
- **Enhancement**: Document common criteria (alignment, page-locking requirements, address range limits)
- **Status**: Mentioned in terminology but provider-specific details not documented

**Token Format Standardization**
- **Current**: Provider-specific opaque format
- **Enhancement**: Consider standard envelope format with provider-specific payload
- **Use Case**: Debugging, logging, cross-provider compatibility
- **Priority**: Low - current approach is flexible

### 15.6 Testing and Validation

**Protocol Violation Test Cases**
- Test Content-Length != 0 with RDMA success codes
- Test mismatched HTTP status and `x-amz-rdma-reply`
- Test missing `x-amz-rdma-bytes-transferred` on GET success
- Test malformed RDMA token handling

**Failure Scenario Coverage**
- RDMA hardware failures mid-transfer
- Network failures during RDMA
- Server-side RDMA failures after client setup
- Mixed RDMA/HTTP multipart uploads

**Performance Benchmarks**
- RDMA vs HTTP latency comparison
- Throughput with various object sizes
- GPU memory (GPUDirect) vs system memory
- Multi-part upload with RDMA

---