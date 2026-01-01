//
// Elastic Frame Protocol - C API Header
// Copyright 2024-2025
//
// Complete C API for EFP - enables use from C, Go, Rust, Python, etc.
//

#ifndef EFP_C_API_H
#define EFP_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//------------------------------------------------------------------------------
// Version
//------------------------------------------------------------------------------

/**
 * Get EFP version
 * @return Version as 0xMMmm where MM=major, mm=minor
 */
uint16_t efp_version(void);

//------------------------------------------------------------------------------
// Constants
//------------------------------------------------------------------------------

// Receiver modes
#define EFP_MODE_THREADED         1
#define EFP_MODE_RUN_TO_COMPLETION 2

// Flags
#define EFP_FLAG_NONE             0x00
#define EFP_FLAG_INLINE_PAYLOAD   0x10
#define EFP_FLAG_PRIORITY_0       0x00
#define EFP_FLAG_PRIORITY_1       0x20
#define EFP_FLAG_PRIORITY_2       0x40
#define EFP_FLAG_PRIORITY_3       0x60

// Result codes (matching efp::Result)
#define EFP_OK                     0
#define EFP_DUPLICATE_FRAGMENT     1
#define EFP_FRAGMENT_TOO_OLD       2
#define EFP_FRAME_TIMEOUT          3
#define EFP_NOT_IMPLEMENTED       -1
#define EFP_INTERNAL_ERROR        -2
#define EFP_RECEIVER_NOT_RUNNING  -3
#define EFP_INVALID_PARAMETER     -4
#define EFP_TOO_LARGE_EMBEDDED    -5
#define EFP_TOO_LARGE_FRAME       -6
#define EFP_FRAME_SIZE_MISMATCH   -7
#define EFP_BUFFER_OUT_OF_RESOURCES -8
#define EFP_BUFFER_OUT_OF_BOUNDS  (-9)
#define EFP_MEMORY_ALLOCATION_ERROR (-10)

// Embedded data types
#define EFP_EMBEDDED_ILLEGAL            0x00
#define EFP_EMBEDDED_PRIVATE_DATA       0x01
#define EFP_EMBEDDED_H222_PMT           0x02
#define EFP_EMBEDDED_MP4_FRAG_BOX       0x03
#define EFP_EMBEDDED_LAST               0x80  // OR with type to mark as last

// Generate FOURCC code from 4 characters
#define EFP_CODE(c0, c1, c2, c3) \
    (((uint32_t)(c0) << 24) | ((uint32_t)(c1) << 16) | ((uint32_t)(c2) << 8) | (uint32_t)(c3))

//------------------------------------------------------------------------------
// Opaque handles
//------------------------------------------------------------------------------

typedef struct efp_sender_s* efp_sender_t;
typedef struct efp_receiver_s* efp_receiver_t;

//------------------------------------------------------------------------------
// Callback types
//------------------------------------------------------------------------------

/**
 * Send callback - called for each fragment to transmit
 * @param data Fragment data
 * @param size Fragment size in bytes
 * @param stream_id Stream identifier
 * @param ctx User context
 */
typedef void (*efp_send_callback_t)(const uint8_t* data, size_t size,
                                     uint8_t stream_id, void* ctx);

/**
 * Receive callback - called for each assembled superframe
 * @param data Frame data
 * @param size Frame size in bytes
 * @param payload_type User-defined payload type
 * @param broken Non-zero if frame is incomplete
 * @param pts Presentation timestamp
 * @param dts Decode timestamp
 * @param payload_code User-defined payload code
 * @param stream_id Stream identifier
 * @param source_id Source identifier
 * @param flags Frame flags
 * @param ctx User context
 */
typedef void (*efp_receive_callback_t)(uint8_t* data, size_t size,
                                        uint8_t payload_type, uint8_t broken,
                                        uint64_t pts, uint64_t dts,
                                        uint32_t payload_code, uint8_t stream_id,
                                        uint8_t source_id, uint8_t flags,
                                        void* ctx);

/**
 * Embedded data callback - called for each embedded data section
 * @param data Embedded data
 * @param size Embedded data size
 * @param data_type Embedded data type
 * @param pts PTS of containing frame
 * @param ctx User context
 */
typedef void (*efp_embedded_callback_t)(uint8_t* data, size_t size,
                                         uint8_t data_type, uint64_t pts,
                                         void* ctx);

//------------------------------------------------------------------------------
// Sender API
//------------------------------------------------------------------------------

/**
 * Create a sender instance
 * @param mtu Maximum transmission unit (minimum 256)
 * @return Sender handle, or NULL on failure
 */
efp_sender_t efp_sender_create(uint16_t mtu);

/**
 * Destroy a sender instance
 * @param sender Sender handle
 */
void efp_sender_destroy(efp_sender_t sender);

/**
 * Set send callback
 * @param sender Sender handle
 * @param callback Callback function
 * @param ctx User context passed to callback
 */
void efp_sender_set_callback(efp_sender_t sender, efp_send_callback_t callback, void* ctx);

/**
 * Send data
 * @param sender Sender handle
 * @param data Data to send
 * @param size Data size in bytes
 * @param payload_type User-defined payload type
 * @param pts Presentation timestamp
 * @param dts Decode timestamp (UINT64_MAX if not used)
 * @param payload_code User-defined payload code
 * @param stream_id Stream identifier (1-255, 0 reserved)
 * @param flags Flags (EFP_FLAG_*)
 * @return Result code (EFP_OK on success)
 */
int16_t efp_sender_send(efp_sender_t sender,
                        const uint8_t* data, size_t size,
                        uint8_t payload_type,
                        uint64_t pts, uint64_t dts,
                        uint32_t payload_code,
                        uint8_t stream_id,
                        uint8_t flags);

/**
 * Set superframe number (for testing/sync)
 * @param sender Sender handle
 * @param superframe_no New superframe number
 */
void efp_sender_set_superframe_no(efp_sender_t sender, uint16_t superframe_no);

//------------------------------------------------------------------------------
// Receiver API
//------------------------------------------------------------------------------

/**
 * Create a receiver instance
 * @param timeout_ms Bucket timeout in milliseconds
 * @param hol_timeout_ms Head-of-line blocking timeout (0 to disable)
 * @param mode EFP_MODE_THREADED or EFP_MODE_RUN_TO_COMPLETION
 * @return Receiver handle, or NULL on failure
 */
efp_receiver_t efp_receiver_create(uint32_t timeout_ms, uint32_t hol_timeout_ms, uint32_t mode);

/**
 * Destroy a receiver instance
 * @param receiver Receiver handle
 */
void efp_receiver_destroy(efp_receiver_t receiver);

/**
 * Set receive callback
 * @param receiver Receiver handle
 * @param callback Callback function
 * @param ctx User context passed to callback
 */
void efp_receiver_set_callback(efp_receiver_t receiver,
                                efp_receive_callback_t callback, void* ctx);

/**
 * Set embedded data callback
 * @param receiver Receiver handle
 * @param callback Callback function (NULL to disable)
 * @param ctx User context passed to callback
 */
void efp_receiver_set_embedded_callback(efp_receiver_t receiver,
                                         efp_embedded_callback_t callback, void* ctx);

/**
 * Receive a fragment
 * @param receiver Receiver handle
 * @param data Fragment data
 * @param size Fragment size
 * @param source_id Source identifier
 * @return Result code (EFP_OK on success)
 */
int16_t efp_receiver_receive(efp_receiver_t receiver,
                              const uint8_t* data, size_t size,
                              uint8_t source_id);

/**
 * Poll for completed frames (run-to-completion mode only)
 * @param receiver Receiver handle
 */
void efp_receiver_poll(efp_receiver_t receiver);

/**
 * Stop receiver
 * @param receiver Receiver handle
 */
void efp_receiver_stop(efp_receiver_t receiver);

//------------------------------------------------------------------------------
// Embedded Data Helpers
//------------------------------------------------------------------------------

/**
 * Calculate size needed for embedded data
 * @param embedded_size Size of embedded data
 * @param payload_size Size of main payload
 * @return Total buffer size needed
 */
size_t efp_embedded_calc_size(size_t embedded_size, size_t payload_size);

/**
 * Add embedded data to a buffer
 *
 * Usage:
 *   1. Call with dst=NULL to get required size
 *   2. Allocate buffer
 *   3. Call again with allocated buffer
 *
 * @param dst Destination buffer (NULL to calculate size only)
 * @param embedded_data Embedded data to add
 * @param payload_data Main payload data
 * @param embedded_size Size of embedded data
 * @param payload_size Size of main payload
 * @param type Embedded data type (EFP_EMBEDDED_*)
 * @param is_last Non-zero if this is the last embedded section
 * @return Total size if dst is NULL, otherwise 0 on success
 */
size_t efp_add_embedded_data(uint8_t* dst,
                              const uint8_t* embedded_data,
                              const uint8_t* payload_data,
                              size_t embedded_size,
                              size_t payload_size,
                              uint8_t type,
                              uint8_t is_last);

/**
 * Extract embedded data from a received frame
 * @param frame_data Frame data
 * @param frame_size Frame size
 * @param embedded_out Output buffer for embedded data (can be NULL)
 * @param embedded_size_out Size of embedded data found
 * @param type_out Type of embedded data
 * @param payload_offset_out Offset where payload starts
 * @return 0 on success, negative on error
 */
int16_t efp_extract_embedded_data(const uint8_t* frame_data, size_t frame_size,
                                   uint8_t* embedded_out, size_t* embedded_size_out,
                                   uint8_t* type_out, size_t* payload_offset_out);

//------------------------------------------------------------------------------
// Legacy compatibility (matches old C API)
//------------------------------------------------------------------------------

// Old-style handle-based API for compatibility
uint64_t efp_init_send(uint64_t mtu,
                       void (*f)(const uint8_t*, size_t, uint8_t, void*),
                       void* ctx);

uint64_t efp_init_receive(uint32_t bucket_timeout, uint32_t hol_timeout,
                          void (*f)(uint8_t*, size_t, uint8_t, uint8_t, uint64_t,
                                   uint64_t, uint32_t, uint8_t, uint8_t, uint8_t, void*),
                          void (*g)(uint8_t*, size_t, uint8_t, uint64_t, void*),
                          void* ctx,
                          uint32_t mode);

int16_t efp_end_send(uint64_t efp_object);
int16_t efp_end_receive(uint64_t efp_object);

int16_t efp_send_data(uint64_t efp_object,
                      const uint8_t* data, size_t size,
                      uint8_t data_content,
                      uint64_t pts, uint64_t dts,
                      uint32_t code,
                      uint8_t stream_id,
                      uint8_t flags);

int16_t efp_receive_fragment(uint64_t efp_object,
                             const uint8_t* fragment, size_t size,
                             uint8_t from_source);

#ifdef __cplusplus
}
#endif

#endif // EFP_C_API_H

