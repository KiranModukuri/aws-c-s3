# S3 RDMA Protocol Specification

**Version:** 1.0  
**Date:** October 2025  
**Status:** Draft

## Abstract

This document specifies the RDMA (Remote Direct Memory Access) protocol extension for AWS S3 HTTP API. This extension enables high-performance data transfers by moving object data out-of-band from the HTTP control channel, using direct memory-to-memory transfers between client and server via RDMA hardware.

## 1. Introduction

### 1.1 Purpose

This specification defines the wire protocol for RDMA-enabled S3 operations, intended for server implementers who wish to support RDMA transfers. Client implementations can reference this as the authoritative protocol behavior.

### 1.2 Terminology

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED", "MAY", and "OPTIONAL" in this document are to be interpreted as described in RFC 2119.

**Protocol Terms:**

- **RDMA Token**: Opaque identifier provided by client in the `x-amz-rdma-token` request header. Encodes buffer location, size, access keys, and context needed for the server to perform RDMA operations. Token format is provider-specific.

- **Control Channel**: The HTTP request/response that coordinates the transfer. Contains headers, authentication, metadata, but not object data when RDMA is used.

- **Data Channel**: The RDMA connection through which object data is transferred out-of-band from HTTP.

- **RDMA Success**: Server successfully transferred data via RDMA, indicated by `x-amz-rdma-reply` header with value 200, 204, or 206.

- **RDMA Decline**: Server chose not to use RDMA (indicated by `x-amz-rdma-reply: 501`) or did not include the header at all.

### 1.3 Protocol Overview

The RDMA extension operates as a **hybrid protocol**:

1. **Client Proposal**: Client includes `x-amz-rdma-token` header in HTTP request
2. **Negotiation**: Server decides whether to use RDMA for this specific request
3. **Data Transfer**: 
   - If RDMA accepted: Data transfers out-of-band via RDMA
   - If RDMA declined: Data transfers via standard HTTP body
4. **Completion**: Server responds with headers indicating RDMA status

**Key Characteristics:**

- RDMA support is OPTIONAL for servers
- Per-request negotiation (each request is independent)
- Graceful fallback to HTTP when RDMA unavailable
- Full backward compatibility with standard S3 protocol
- Works with existing S3 authentication and authorization

## 2. Protocol Negotiation

### 2.1 Client Proposal

A client proposes RDMA for a request by including the `x-amz-rdma-token` header:

```http
PUT /bucket/object HTTP/1.1
Host: s3.amazonaws.com
x-amz-rdma-token: <opaque-token>
Content-Length: 0
x-amz-checksum-crc32c: <checksum>
Authorization: <signature>

[Empty HTTP body]
```

**Requirements:**

- Client MUST include `x-amz-rdma-token` Opaque header specific to the plugin
- Client MUST set `Content-Length: 0` for PUT operations
- Client MUST send empty HTTP body
- Client MUST calculate checksums (`x-amz-checksum-*`) on the raw data that will be transferred via RDMA
- Client MUST sign the request with empty body (payload hash is SHA256 of empty string)

### 2.2 Server Decision

Upon receiving a request with `x-amz-rdma-token`, the server MUST choose one of:

**Option A: Accept RDMA**
- Parse and validate the RDMA token
- Perform RDMA read (PUT) or write (GET) using token information
- Include `x-amz-rdma-reply` header in response with success code (200/204/206)

**Option B: Decline RDMA**
- Ignore the RDMA token
- Either:
  - Return `x-amz-rdma-reply: 501` and serve via HTTP body (GET) or return error (PUT)
  - Omit `x-amz-rdma-reply` header entirely and serve via standard HTTP

### 2.3 Server Capabilities

Servers MAY decline RDMA for any reason including but not limited to:

- RDMA not supported on this endpoint
- Client token is invalid or cannot be parsed
- Requested object size doesn't meet RDMA thresholds
- Server-side resource constraints
- Network path doesn't support RDMA

## 3. PUT Operations (Upload)

### 3.1 Standard PUT Request with RDMA

**Request:**

```http
PUT /bucket/object HTTP/1.1
Host: s3.amazonaws.com
x-amz-rdma-token: <opaque-token>
Content-Length: 0
Content-Type: application/octet-stream
x-amz-checksum-crc32c: i9aeUg==
x-amz-content-sha256: UNSIGNED-PAYLOAD
Authorization: AWS4-HMAC-SHA256 ...

[Empty HTTP body - data will be read by server via RDMA]
```

**Server Behavior:**

1. Authenticate and authorize the request using standard S3 mechanisms
2. Parse the `x-amz-rdma-token` to extract buffer information
3. Use RDMA READ operation to fetch data from client's memory
4. Validate checksum against received data
5. Store the object

### 3.2 PUT Response - RDMA Success

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
x-amz-request-id: ABC123
ETag: "abc123def456"
Content-Length: 0

[Empty HTTP body]
```

**Requirements:**

- Server MUST include `x-amz-rdma-reply: 200` header
- Server MUST set `Content-Length: 0`
- Server MUST NOT include response body
- Server MUST include standard S3 response headers (ETag, etc.)
- HTTP status code MUST be 200 OK

### 3.3 PUT Response - RDMA Declined (Server Fallback)

When server cannot or chooses not to use RDMA for a PUT:

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 501
Content-Length: 87
Content-Type: application/xml

<Error>
  <Code>RDMANotSupported</Code>
  <Message>RDMA not available</Message>
</Error>
```

**Server Behavior:**

- Server MUST include `x-amz-rdma-reply: 501`
- Server MAY include HTTP body with error details
- HTTP status code MUST be 200 (operation did not succeed)
- Client is expected to retry with data in HTTP body

**Rationale**: For PUT operations with RDMA decline, the server received no data (empty HTTP body), so the upload did not complete. Client must retry the entire operation with data in the HTTP body.

### 3.4 Multipart Upload with RDMA

Each part of a multipart upload independently negotiates RDMA:

```http
PUT /bucket/object?partNumber=1&uploadId=xyz HTTP/1.1
Host: s3.amazonaws.com
x-amz-rdma-token: <opaque-token-for-part1>
Content-Length: 0
x-amz-checksum-crc32c: <part-1-checksum>

[Empty HTTP body]
```

**Response:**

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
ETag: "part1-etag"
Content-Length: 0
```

**Requirements:**

- Each part MUST have its own unique RDMA token
- Each part independently succeeds or fails with RDMA
- Parts can mix RDMA and HTTP within same multipart upload
- CompleteMultipartUpload operates normally (no RDMA involvement)

## 4. GET Operations (Download)

### 4.1 Standard GET Request with RDMA

**Request:**

```http
GET /bucket/object HTTP/1.1
Host: s3.amazonaws.com
x-amz-rdma-token: <opaque-token>
x-amz-checksum-mode: ENABLED
```

**Server Behavior:**

1. Authenticate and authorize the request
2. Parse the `x-amz-rdma-token` to extract buffer information
3. Retrieve the object data
4. Use RDMA WRITE operation to place data in client's memory
5. Return response with RDMA status

### 4.2 GET Response - RDMA Success

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
x-amz-rdma-bytes-transferred: 10485760
Content-Length: 0
Content-Type: application/octet-stream
ETag: "abc123"
x-amz-checksum-crc32c: i9aeUg==
Last-Modified: Mon, 13 Oct 2025 09:00:00 GMT

[Empty HTTP body - data transferred via RDMA]
```

**Requirements:**

- Server MUST include `x-amz-rdma-reply: 200` header
- Server MUST include `x-amz-rdma-bytes-transferred` header with actual byte count
- Server MUST set `Content-Length: 0`
- Server MUST NOT include data in HTTP body
- HTTP status code MUST be 200 OK
- All standard object metadata headers MUST be present

**x-amz-rdma-bytes-transferred format:**

- MUST be decimal integer representing bytes transferred via RDMA
- Example: `x-amz-rdma-bytes-transferred: 10485760`

### 4.3 GET Response - RDMA Declined (Server Fallback)

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 501
Content-Length: 10485760
Content-Type: application/octet-stream
ETag: "abc123"

[HTTP body contains object data - 10485760 bytes]
```

**Server Behavior:**

- Server MUST include `x-amz-rdma-reply: 501`
- Server MUST include object data in HTTP body
- `Content-Length` MUST reflect actual object size
- HTTP status code MUST be 200 OK (operation succeeded via HTTP)
- Client should process HTTP body normally

**Rationale**: For GET operations with RDMA decline, the server can still serve the data via HTTP body. The operation completes successfully, client just receives data via HTTP instead of RDMA.

### 4.4 Ranged GET with RDMA

```http
GET /bucket/object HTTP/1.1
Host: s3.amazonaws.com
Range: bytes=0-10485759
x-amz-rdma-token: <opaque-token>
```

**Response:**

```http
HTTP/1.1 206 Partial Content
x-amz-rdma-reply: 206
x-amz-rdma-bytes-transferred: 10485760
Content-Range: bytes 0-10485759/104857600
Content-Length: 0
ETag: "abc123"
```

**Requirements:**

- Server MUST use `x-amz-rdma-reply: 206` for successful partial content
- Server MUST include `Content-Range` header
- Bytes transferred MUST match range size: `(end - start + 1) == bytes_transferred`
- All other RDMA success requirements apply

## 5. Checksum Handling

### 5.1 PUT Checksum Requirements

**With RDMA:**

- Client calculates checksum on raw buffer data (NOT on HTTP body encoding)
- Client includes `x-amz-checksum-*` header in request
- Client MUST NOT include `Content-MD5` (applies to HTTP body only)
- Server validates checksum against RDMA-received data

**Example:**

```http
PUT /bucket/object HTTP/1.1
x-amz-rdma-token: <opaque-token>
x-amz-checksum-crc32c: i9aeUg==
Content-Length: 0
```

**Standard HTTP (for comparison):**

```http
PUT /bucket/object HTTP/1.1
Content-Length: 10485760
Content-MD5: 1B2M2Y8AsgTpgAmY7PhCfg==
x-amz-checksum-crc32c: <checksum-of-encoded-stream>

[HTTP body with possible aws-chunked encoding]
```

### 5.2 GET Checksum Validation

Server includes checksum in response headers:

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
x-amz-rdma-bytes-transferred: 10485760
x-amz-checksum-crc32c: i9aeUg==
```

Client validates checksum against data received via RDMA.

### 5.3 Supported Algorithms

The following S3 flexible checksum algorithms are supported with RDMA:

- `x-amz-checksum-crc32`
- `x-amz-checksum-crc32c`
- `x-amz-checksum-sha1`
- `x-amz-checksum-sha256`

All checksums are calculated on raw data, not HTTP-encoded streams.

## 6. Authentication and Signing

### 6.1 SigV4 Signing with RDMA

**PUT Request Signing:**

- Client signs request with empty HTTP body
- `x-amz-content-sha256` header MUST be `UNSIGNED-PAYLOAD`
- `x-amz-rdma-token` MUST be included in signed headers list
- Payload hash in signature is SHA256 of empty string: `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`

**Canonical Request Example:**

```
PUT
/bucket/object

content-length:0
host:s3.amazonaws.com
x-amz-checksum-crc32c:i9aeUg==
x-amz-content-sha256:UNSIGNED-PAYLOAD
x-amz-date:20251013T120000Z
x-amz-rdma-token:<opaque-token>

content-length;host;x-amz-checksum-crc32c;x-amz-content-sha256;x-amz-date;x-amz-rdma-token
UNSIGNED-PAYLOAD
```

**GET Request Signing:**

- Standard SigV4 signing applies
- `x-amz-rdma-token` included in signed headers

## 7. Error Handling

### 7.1 Invalid RDMA Token

If server cannot parse or validate the RDMA token:

```http
HTTP/1.1 400 Bad Request
Content-Type: application/xml

<Error>
  <Code>InvalidRDMAToken</Code>
  <Message>The provided RDMA token is invalid</Message>
</Error>
```

**Server Behavior:**

- Return HTTP error response (400, 403, etc.)
- Do NOT include `x-amz-rdma-reply` header
- Client handles as standard S3 error

### 7.2 RDMA Operation Failure

If RDMA transfer fails after being accepted:

**For PUT:**

```http
HTTP/1.1 500 Internal Server Error
Content-Type: application/xml

<Error>
  <Code>RDMATransferFailed</Code>
  <Message>RDMA data transfer failed</Message>
</Error>
```

**For GET with recovery:**

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 501
Content-Length: 10485760

[Object data in HTTP body as fallback]
```

### 7.3 Server Does Not Support RDMA

If server doesn't support RDMA at all, it MAY:

**Option A: Omit x-amz-rdma-reply header**

```http
HTTP/1.1 200 OK
Content-Length: <size>

[Data in HTTP body]
```

**Option B: Return 501 code**

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 501
Content-Length: <size>

[Data in HTTP body]
```

Both are valid. Client MUST handle both cases.

## 8. Protocol Validation Rules

### 8.1 Content-Length Validation

**Rule:** When `x-amz-rdma-reply` is 200, 204, or 206, `Content-Length` MUST be 0.

**Rationale:** These codes indicate data was transferred via RDMA, not HTTP body.

**Valid Response:**

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
Content-Length: 0
```

**Invalid Response (Protocol Violation):**

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
Content-Length: 10485760  ← ERROR
```

### 8.2 Status Code Consistency

**Rule:** HTTP status code SHOULD match `x-amz-rdma-reply` value in success cases.

| x-amz-rdma-reply | Expected HTTP Status | Meaning |
|------------------|---------------------|---------|
| 200 | 200 OK | Full object transferred via RDMA |
| 204 | 204 No Content | Success with no content |
| 206 | 206 Partial Content | Range request via RDMA |
| 501 | 200 (usually) | RDMA not used, HTTP body contains data/error |

### 8.3 Required Headers

**For RDMA Success (200/204/206):**

- `x-amz-rdma-reply` - MUST be present
- `Content-Length` - MUST be 0
- `x-amz-rdma-bytes-transferred` - MUST be present for GET operations

**For RDMA Decline (501):**

- `x-amz-rdma-reply` - MUST be present
- For GET: `Content-Length` > 0, body contains object data
- For PUT: May include error details in body

## 9. Summary Table

| Aspect | Standard HTTP | RDMA Protocol |
|--------|---------------|---------------|
| **Request (PUT)** |
| Header: x-amz-rdma-token | Not present | MUST be present (opaque token) |
| Content-Length | Object size | 0 |
| HTTP Body | Contains data | Empty |
| Content-MD5 | May be present | MUST NOT be present |
| Checksum calculation | On encoded stream | On raw buffer |
| **Response (PUT Success)** |
| Header: x-amz-rdma-reply | Not present | MUST be 200 |
| Content-Length | 0 | 0 |
| HTTP Body | Empty | Empty |
| **Request (GET)** |
| Header: x-amz-rdma-token | Not present | MUST be present |
| **Response (GET Success)** |
| Header: x-amz-rdma-reply | Not present | MUST be 200 or 206 |
| Header: x-amz-rdma-bytes-transferred | Not present | MUST be present |
| Content-Length | Object size | 0 |
| HTTP Body | Contains data | Empty |
| **Fallback (501)** |
| For GET | N/A | Server sends data in HTTP body, operation succeeds |
| For PUT | N/A | Client must retry with data in HTTP body |

## 10. Implementation Notes

### 10.1 Token Format

RDMA token format is provider-specific and opaque to the protocol. Typical contents:

- Memory address or handle
- Buffer size
- Access permissions/keys
- Connection context
- Timeout/expiration

Tokens are typically 100-200 bytes.

### 10.2 Backward Compatibility

- Servers without RDMA support simply ignore `x-amz-rdma-token` header
- Clients MUST handle responses without `x-amz-rdma-reply` header
- All existing S3 API semantics preserved
- Authentication, authorization unchanged

### 10.3 Security Considerations

- RDMA tokens MUST be signed when using signing of headers
- Tokens MUST have limited lifetime/scope
- Standard S3 authentication applies to control channel
- RDMA data channel security is implementation-specific
- Token reuse across requests is permitted

## 11. Examples

### 11.1 Complete PUT Sequence

**Client Request:**

```http
PUT /my-bucket/large-file.dat HTTP/1.1
Host: s3.amazonaws.com
x-amz-rdma-token: <opaque-token>
Content-Length: 0
Content-Type: application/octet-stream
x-amz-checksum-crc32c: i9aeUg==
x-amz-content-sha256: UNSIGNED-PAYLOAD
x-amz-date: 20251013T120000Z
Authorization: AWS4-HMAC-SHA256 Credential=.../20251013/us-east-1/s3/aws4_request, SignedHeaders=content-length;content-type;host;x-amz-checksum-crc32c;x-amz-content-sha256;x-amz-date;x-amz-rdma-token, Signature=...

[Empty HTTP body]
```

**Server Response:**

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
x-amz-request-id: TX123456789ABC
ETag: "5d41402abc4b2a76b9719d911017c592"
x-amz-server-side-encryption: AES256
Content-Length: 0
Date: Mon, 13 Oct 2025 12:00:00 GMT

[Empty HTTP body]
```

### 11.2 Complete GET Sequence

**Client Request:**

```http
GET /my-bucket/large-file.dat HTTP/1.1
Host: s3.amazonaws.com
x-amz-rdma-token: <opaque-token>
x-amz-checksum-mode: ENABLED
x-amz-date: 20251013T120000Z
Authorization: AWS4-HMAC-SHA256 ...
```

**Server Response:**

```http
HTTP/1.1 200 OK
x-amz-rdma-reply: 200
x-amz-rdma-bytes-transferred: 10485760
Content-Length: 0
Content-Type: application/octet-stream
ETag: "5d41402abc4b2a76b9719d911017c592"
x-amz-checksum-crc32c: i9aeUg==
Last-Modified: Mon, 13 Oct 2025 10:00:00 GMT
Accept-Ranges: bytes
x-amz-request-id: RX987654321XYZ
Date: Mon, 13 Oct 2025 12:00:00 GMT

[Empty HTTP body - data transferred via RDMA]
```

---

## Appendix A: References

- RFC 2119: Key words for use in RFCs to Indicate Requirement Levels
- AWS S3 API Reference: https://docs.aws.amazon.com/s3/
- RDMA Consortium: https://www.rdmaconsortium.org/

## Appendix B: Revision History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | October 2025 | Initial protocol specification |

