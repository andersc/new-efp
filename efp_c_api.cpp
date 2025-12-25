//
// Elastic Frame Protocol - C API Implementation
// Copyright 2024-2025
//

#include "efp_c_api.h"
#include "efp.h"

#include <map>
#include <mutex>
#include <memory>
#include <cstring>

//------------------------------------------------------------------------------
// Internal structures
//------------------------------------------------------------------------------

struct efp_sender_s {
    std::unique_ptr<efp::Sender<>> sender;
    efp_send_callback_t callback = nullptr;
    void* ctx = nullptr;
};

struct efp_receiver_s {
    std::unique_ptr<efp::Receiver<>> receiver;
    efp_receive_callback_t callback = nullptr;
    efp_embedded_callback_t embedded_callback = nullptr;
    void* ctx = nullptr;
};

// Legacy API handle management
static std::recursive_mutex g_handles_mutex;
static std::map<uint64_t, efp_sender_t> g_sender_handles;
static std::map<uint64_t, efp_receiver_t> g_receiver_handles;
static uint64_t g_next_handle = 1;

//------------------------------------------------------------------------------
// Version
//------------------------------------------------------------------------------

uint16_t efp_version(void) {
    return efp::VERSION;
}

//------------------------------------------------------------------------------
// Sender API
//------------------------------------------------------------------------------

efp_sender_t efp_sender_create(uint16_t mtu) {
    try {
        auto* s = new efp_sender_s();
        s->sender = std::make_unique<efp::Sender<>>(mtu);
        return s;
    } catch (...) {
        return nullptr;
    }
}

void efp_sender_destroy(efp_sender_t sender) {
    if (sender) {
        delete sender;
    }
}

void efp_sender_set_callback(efp_sender_t sender, efp_send_callback_t callback, void* ctx) {
    if (!sender) return;

    sender->callback = callback;
    sender->ctx = ctx;

    sender->sender->setCallback([sender](const uint8_t* data, size_t size, uint8_t stream_id) {
        if (sender->callback) {
            sender->callback(data, size, stream_id, sender->ctx);
        }
    });
}

int16_t efp_sender_send(efp_sender_t sender,
                        const uint8_t* data, size_t size,
                        uint8_t payload_type,
                        uint64_t pts, uint64_t dts,
                        uint32_t payload_code,
                        uint8_t stream_id,
                        uint8_t flags) {
    if (!sender || !sender->sender) {
        return EFP_INVALID_PARAMETER;
    }

    auto result = sender->sender->send(data, size, payload_type, pts, dts,
                                       payload_code, stream_id, flags);
    return static_cast<int16_t>(result);
}

void efp_sender_set_superframe_no(efp_sender_t sender, uint16_t superframe_no) {
    // This requires adding a method to the Sender class
    // For now, this is a placeholder
    (void)sender;
    (void)superframe_no;
}

//------------------------------------------------------------------------------
// Receiver API
//------------------------------------------------------------------------------

efp_receiver_t efp_receiver_create(uint32_t timeout_ms, uint32_t hol_timeout_ms, uint32_t mode) {
    try {
        auto* r = new efp_receiver_s();
        efp::ReceiverMode recv_mode = (mode == EFP_MODE_RUN_TO_COMPLETION)
            ? efp::ReceiverMode::RunToCompletion
            : efp::ReceiverMode::Threaded;

        r->receiver = std::make_unique<efp::Receiver<>>(timeout_ms, hol_timeout_ms, recv_mode);
        return r;
    } catch (...) {
        return nullptr;
    }
}

void efp_receiver_destroy(efp_receiver_t receiver) {
    if (receiver) {
        if (receiver->receiver) {
            receiver->receiver->stop();
        }
        delete receiver;
    }
}

void efp_receiver_set_callback(efp_receiver_t receiver,
                                efp_receive_callback_t callback, void* ctx) {
    if (!receiver) return;

    receiver->callback = callback;
    receiver->ctx = ctx;

    receiver->receiver->setCallback([receiver](efp::SuperFramePtr frame) {
        if (receiver->callback && frame) {
            // Handle embedded data if callback is set
            size_t payload_offset = 0;

            if (receiver->embedded_callback &&
                (frame->flags & efp::Flags::InlinePayload) &&
                !frame->broken) {

                // Parse embedded data
                size_t offset = 0;
                while (offset + 3 <= frame->size) {
                    uint8_t type = frame->data[offset];
                    if (type == 0) break;

                    uint16_t emb_size = frame->data[offset + 1] |
                                       (frame->data[offset + 2] << 8);

                    bool is_last = (type & 0x80) != 0;
                    uint8_t actual_type = type & 0x7F;

                    if (offset + 3 + emb_size <= frame->size) {
                        receiver->embedded_callback(
                            frame->data + offset + 3,
                            emb_size,
                            actual_type,
                            frame->pts,
                            receiver->ctx
                        );
                    }

                    offset += 3 + emb_size;

                    if (is_last) {
                        payload_offset = offset;
                        break;
                    }
                }
            }

            // Call main callback
            receiver->callback(
                frame->data + payload_offset,
                frame->size - payload_offset,
                frame->payloadType,
                frame->broken ? 1 : 0,
                frame->pts,
                frame->dts,
                frame->payloadCode,
                frame->streamId,
                frame->sourceId,
                frame->flags,
                receiver->ctx
            );
        }
    });
}

void efp_receiver_set_embedded_callback(efp_receiver_t receiver,
                                         efp_embedded_callback_t callback, void* ctx) {
    if (!receiver) return;
    receiver->embedded_callback = callback;
    // Note: ctx is shared with main callback
    (void)ctx;
}

int16_t efp_receiver_receive(efp_receiver_t receiver,
                              const uint8_t* data, size_t size,
                              uint8_t source_id) {
    if (!receiver || !receiver->receiver) {
        return EFP_INVALID_PARAMETER;
    }

    auto result = receiver->receiver->receive(data, size, source_id);
    return static_cast<int16_t>(result);
}

void efp_receiver_poll(efp_receiver_t receiver) {
    if (receiver && receiver->receiver) {
        receiver->receiver->poll();
    }
}

void efp_receiver_stop(efp_receiver_t receiver) {
    if (receiver && receiver->receiver) {
        receiver->receiver->stop();
    }
}

//------------------------------------------------------------------------------
// Embedded Data Helpers
//------------------------------------------------------------------------------

size_t efp_embedded_calc_size(size_t embedded_size, size_t payload_size) {
    return 3 + embedded_size + payload_size;  // header (3) + embedded + payload
}

size_t efp_add_embedded_data(uint8_t* dst,
                              const uint8_t* embedded_data,
                              const uint8_t* payload_data,
                              size_t embedded_size,
                              size_t payload_size,
                              uint8_t type,
                              uint8_t is_last) {
    size_t total_size = efp_embedded_calc_size(embedded_size, payload_size);

    if (!dst) {
        return total_size;  // Return required size
    }

    // Build embedded header
    uint8_t type_byte = type;
    if (is_last) {
        type_byte |= EFP_EMBEDDED_LAST;
    }

    dst[0] = type_byte;
    dst[1] = embedded_size & 0xFF;
    dst[2] = (embedded_size >> 8) & 0xFF;

    // Copy embedded data
    if (embedded_data && embedded_size > 0) {
        std::memcpy(dst + 3, embedded_data, embedded_size);
    }

    // Copy payload
    if (payload_data && payload_size > 0) {
        std::memcpy(dst + 3 + embedded_size, payload_data, payload_size);
    }

    return 0;  // Success
}

int16_t efp_extract_embedded_data(const uint8_t* frame_data, size_t frame_size,
                                   uint8_t* embedded_out, size_t* embedded_size_out,
                                   uint8_t* type_out, size_t* payload_offset_out) {
    if (!frame_data || frame_size < 3) {
        return EFP_INVALID_PARAMETER;
    }

    uint8_t type = frame_data[0];
    if (type == 0) {
        // No embedded data
        if (payload_offset_out) *payload_offset_out = 0;
        if (embedded_size_out) *embedded_size_out = 0;
        return EFP_OK;
    }

    uint16_t emb_size = frame_data[1] | (frame_data[2] << 8);

    if (3 + emb_size > frame_size) {
        return EFP_BUFFER_OUT_OF_BOUNDS;
    }

    if (type_out) {
        *type_out = type & 0x7F;  // Remove last flag
    }

    if (embedded_size_out) {
        *embedded_size_out = emb_size;
    }

    if (embedded_out && emb_size > 0) {
        std::memcpy(embedded_out, frame_data + 3, emb_size);
    }

    if (payload_offset_out) {
        *payload_offset_out = 3 + emb_size;
    }

    return EFP_OK;
}

//------------------------------------------------------------------------------
// Legacy API Implementation
//------------------------------------------------------------------------------

uint64_t efp_init_send(uint64_t mtu,
                       void (*f)(const uint8_t*, size_t, uint8_t, void*),
                       void* ctx) {
    efp_sender_t sender = efp_sender_create(static_cast<uint16_t>(mtu));
    if (!sender) {
        return 0;
    }

    efp_sender_set_callback(sender, f, ctx);

    std::lock_guard<std::recursive_mutex> lock(g_handles_mutex);
    uint64_t handle = g_next_handle++;
    g_sender_handles[handle] = sender;
    return handle;
}

uint64_t efp_init_receive(uint32_t bucket_timeout, uint32_t hol_timeout,
                          void (*f)(uint8_t*, size_t, uint8_t, uint8_t, uint64_t,
                                   uint64_t, uint32_t, uint8_t, uint8_t, uint8_t, void*),
                          void (*g)(uint8_t*, size_t, uint8_t, uint64_t, void*),
                          void* ctx,
                          uint32_t mode) {
    efp_receiver_t receiver = efp_receiver_create(bucket_timeout, hol_timeout, mode);
    if (!receiver) {
        return 0;
    }

    efp_receiver_set_callback(receiver, f, ctx);
    efp_receiver_set_embedded_callback(receiver, g, ctx);

    std::lock_guard<std::recursive_mutex> lock(g_handles_mutex);
    uint64_t handle = g_next_handle++;
    g_receiver_handles[handle] = receiver;
    return handle;
}

int16_t efp_end_send(uint64_t efp_object) {
    std::lock_guard<std::recursive_mutex> lock(g_handles_mutex);

    auto it = g_sender_handles.find(efp_object);
    if (it == g_sender_handles.end()) {
        return EFP_INVALID_PARAMETER;
    }

    efp_sender_destroy(it->second);
    g_sender_handles.erase(it);
    return EFP_OK;
}

int16_t efp_end_receive(uint64_t efp_object) {
    std::lock_guard<std::recursive_mutex> lock(g_handles_mutex);

    auto it = g_receiver_handles.find(efp_object);
    if (it == g_receiver_handles.end()) {
        return EFP_INVALID_PARAMETER;
    }

    efp_receiver_destroy(it->second);
    g_receiver_handles.erase(it);
    return EFP_OK;
}

int16_t efp_send_data(uint64_t efp_object,
                      const uint8_t* data, size_t size,
                      uint8_t data_content,
                      uint64_t pts, uint64_t dts,
                      uint32_t code,
                      uint8_t stream_id,
                      uint8_t flags) {
    std::lock_guard<std::recursive_mutex> lock(g_handles_mutex);

    auto it = g_sender_handles.find(efp_object);
    if (it == g_sender_handles.end()) {
        return EFP_INVALID_PARAMETER;
    }

    return efp_sender_send(it->second, data, size, data_content, pts, dts,
                          code, stream_id, flags);
}

int16_t efp_receive_fragment(uint64_t efp_object,
                             const uint8_t* fragment, size_t size,
                             uint8_t from_source) {
    std::lock_guard<std::recursive_mutex> lock(g_handles_mutex);

    auto it = g_receiver_handles.find(efp_object);
    if (it == g_receiver_handles.end()) {
        return EFP_INVALID_PARAMETER;
    }

    return efp_receiver_receive(it->second, fragment, size, from_source);
}

