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
    std::unique_ptr<efp::Sender<>> mpSender;
    efp_send_callback_t mCallback = nullptr;
    void* mpCtx = nullptr;
};

struct efp_receiver_s {
    std::unique_ptr<efp::Receiver<>> mpReceiver;
    efp_receive_callback_t mCallback = nullptr;
    efp_embedded_callback_t mEmbeddedCallback = nullptr;
    void* mpCtx = nullptr;
};

// Legacy API handle management
static std::recursive_mutex gHandlesMutex;
static std::map<uint64_t, efp_sender_t> gSenderHandles;
static std::map<uint64_t, efp_receiver_t> gReceiverHandles;
static uint64_t gNextHandle = 1;

//------------------------------------------------------------------------------
// Version
//------------------------------------------------------------------------------

uint16_t efp_version(void) {
    return efp::VERSION;
}

//------------------------------------------------------------------------------
// Sender API
//------------------------------------------------------------------------------

efp_sender_t efp_sender_create(uint16_t aMtu) {
    try {
        auto* lpSender = new efp_sender_s();
        lpSender->mpSender = std::make_unique<efp::Sender<>>(aMtu);
        return lpSender;
    } catch (...) {
        return nullptr;
    }
}

void efp_sender_destroy(efp_sender_t apSender) {
    if (apSender) {
        delete apSender;
    }
}

void efp_sender_set_callback(efp_sender_t apSender, efp_send_callback_t aCallback, void* apCtx) {
    if (!apSender) {
        return;
    }

    apSender->mCallback = aCallback;
    apSender->mpCtx = apCtx;

    apSender->mpSender->setCallback([apSender](const uint8_t* apData, size_t aSize, uint8_t aStreamId) {
        if (apSender->mCallback) {
            apSender->mCallback(apData, aSize, aStreamId, apSender->mpCtx);
        }
    });
}

int16_t efp_sender_send(efp_sender_t apSender,
                        const uint8_t* apData, size_t aSize,
                        uint8_t aPayloadType,
                        uint64_t aPts, uint64_t aDts,
                        uint32_t aPayloadCode,
                        uint8_t aStreamId,
                        uint8_t aFlags) {
    if (!apSender || !apSender->mpSender) {
        return EFP_INVALID_PARAMETER;
    }

    auto lResult = apSender->mpSender->send(apData, aSize, aPayloadType, aPts, aDts,
                                            aPayloadCode, aStreamId, aFlags);
    return (int16_t)(lResult);
}

void efp_sender_set_superframe_no(efp_sender_t apSender, uint16_t aSuperframeNo) {
    // This requires adding a method to the Sender class
    // For now, this is a placeholder
    (void)apSender;
    (void)aSuperframeNo;
}

//------------------------------------------------------------------------------
// Receiver API
//------------------------------------------------------------------------------

efp_receiver_t efp_receiver_create(uint32_t aTimeoutMs, uint32_t aHolTimeoutMs, uint32_t aMode) {
    try {
        auto* lpReceiver = new efp_receiver_s();
        auto lRecvMode = (aMode == EFP_MODE_RUN_TO_COMPLETION)
            ? efp::ReceiverMode::RUN_TO_COMPLETION
            : efp::ReceiverMode::THREADED;

        lpReceiver->mpReceiver = std::make_unique<efp::Receiver<>>(aTimeoutMs, aHolTimeoutMs, lRecvMode);
        return lpReceiver;
    } catch (...) {
        return nullptr;
    }
}

void efp_receiver_destroy(efp_receiver_t apReceiver) {
    if (apReceiver) {
        if (apReceiver->mpReceiver) {
            apReceiver->mpReceiver->stop();
        }
        delete apReceiver;
    }
}

void efp_receiver_set_callback(efp_receiver_t apReceiver,
                                efp_receive_callback_t aCallback, void* apCtx) {
    if (!apReceiver) {
        return;
    }

    apReceiver->mCallback = aCallback;
    apReceiver->mpCtx = apCtx;

    apReceiver->mpReceiver->setCallback([apReceiver](efp::SuperFramePtr apFrame) {
        if (apReceiver->mCallback && apFrame) {
            // Handle embedded data if callback is set
            size_t lPayloadOffset = 0;

            if (apReceiver->mEmbeddedCallback &&
                (apFrame->mFlags & efp::Flags::INLINE_PAYLOAD) &&
                !apFrame->mBroken) {

                // Parse embedded data
                size_t lOffset = 0;
                while (lOffset + 3 <= apFrame->mSize) {
                    auto lType = apFrame->mpData[lOffset];
                    if (lType == 0) {
                        break;
                    }

                    auto lEmbSize = (uint16_t)(apFrame->mpData[lOffset + 1] |
                                              (apFrame->mpData[lOffset + 2] << 8));

                    auto lIsLast = (lType & 0x80) != 0;
                    auto lActualType = (uint8_t)(lType & 0x7F);

                    if (lOffset + 3 + lEmbSize <= apFrame->mSize) {
                        apReceiver->mEmbeddedCallback(
                            apFrame->mpData + lOffset + 3,
                            lEmbSize,
                            lActualType,
                            apFrame->mPts,
                            apReceiver->mpCtx
                        );
                    }

                    lOffset += 3 + lEmbSize;

                    if (lIsLast) {
                        lPayloadOffset = lOffset;
                        break;
                    }
                }
            }

            // Call main callback
            apReceiver->mCallback(
                apFrame->mpData + lPayloadOffset,
                apFrame->mSize - lPayloadOffset,
                apFrame->mPayloadType,
                apFrame->mBroken ? 1 : 0,
                apFrame->mPts,
                apFrame->mDts,
                apFrame->mPayloadCode,
                apFrame->mStreamId,
                apFrame->mSourceId,
                apFrame->mFlags,
                apReceiver->mpCtx
            );
        }
    });
}

void efp_receiver_set_embedded_callback(efp_receiver_t apReceiver,
                                         efp_embedded_callback_t aCallback, void* apCtx) {
    if (!apReceiver) {
        return;
    }
    apReceiver->mEmbeddedCallback = aCallback;
    // Note: ctx is shared with main callback
    (void)apCtx;
}

int16_t efp_receiver_receive(efp_receiver_t apReceiver,
                              const uint8_t* apData, size_t aSize,
                              uint8_t aSourceId) {
    if (!apReceiver || !apReceiver->mpReceiver) {
        return EFP_INVALID_PARAMETER;
    }

    auto lResult = apReceiver->mpReceiver->receive(apData, aSize, aSourceId);
    return (int16_t)(lResult);
}

void efp_receiver_poll(efp_receiver_t apReceiver) {
    if (apReceiver && apReceiver->mpReceiver) {
        apReceiver->mpReceiver->poll();
    }
}

void efp_receiver_stop(efp_receiver_t apReceiver) {
    if (apReceiver && apReceiver->mpReceiver) {
        apReceiver->mpReceiver->stop();
    }
}

//------------------------------------------------------------------------------
// Embedded Data Helpers
//------------------------------------------------------------------------------

size_t efp_embedded_calc_size(size_t aEmbeddedSize, size_t aPayloadSize) {
    return 3 + aEmbeddedSize + aPayloadSize;  // header (3) + embedded + payload
}

size_t efp_add_embedded_data(uint8_t* apDst,
                              const uint8_t* apEmbeddedData,
                              const uint8_t* apPayloadData,
                              size_t aEmbeddedSize,
                              size_t aPayloadSize,
                              uint8_t aType,
                              uint8_t aIsLast) {
    auto lTotalSize = efp_embedded_calc_size(aEmbeddedSize, aPayloadSize);

    if (!apDst) {
        return lTotalSize;  // Return required size
    }

    // Build embedded header
    auto lTypeByte = aType;
    if (aIsLast) {
        lTypeByte |= EFP_EMBEDDED_LAST;
    }

    apDst[0] = lTypeByte;
    apDst[1] = aEmbeddedSize & 0xFF;
    apDst[2] = (aEmbeddedSize >> 8) & 0xFF;

    // Copy embedded data
    if (apEmbeddedData && aEmbeddedSize > 0) {
        std::memcpy(apDst + 3, apEmbeddedData, aEmbeddedSize);
    }

    // Copy payload
    if (apPayloadData && aPayloadSize > 0) {
        std::memcpy(apDst + 3 + aEmbeddedSize, apPayloadData, aPayloadSize);
    }

    return 0;  // Success
}

int16_t efp_extract_embedded_data(const uint8_t* apFrameData, size_t aFrameSize,
                                   uint8_t* apEmbeddedOut, size_t* apEmbeddedSizeOut,
                                   uint8_t* apTypeOut, size_t* apPayloadOffsetOut) {
    if (!apFrameData || aFrameSize < 3) {
        return EFP_INVALID_PARAMETER;
    }

    auto lType = apFrameData[0];
    if (lType == 0) {
        // No embedded data
        if (apPayloadOffsetOut) {
            *apPayloadOffsetOut = 0;
        }
        if (apEmbeddedSizeOut) {
            *apEmbeddedSizeOut = 0;
        }
        return EFP_OK;
    }

    auto lEmbSize = (uint16_t)(apFrameData[1] | (apFrameData[2] << 8));

    if (3 + lEmbSize > aFrameSize) {
        return EFP_BUFFER_OUT_OF_BOUNDS;
    }

    if (apTypeOut) {
        *apTypeOut = lType & 0x7F;  // Remove last flag
    }

    if (apEmbeddedSizeOut) {
        *apEmbeddedSizeOut = lEmbSize;
    }

    if (apEmbeddedOut && lEmbSize > 0) {
        std::memcpy(apEmbeddedOut, apFrameData + 3, lEmbSize);
    }

    if (apPayloadOffsetOut) {
        *apPayloadOffsetOut = 3 + lEmbSize;
    }

    return EFP_OK;
}

//------------------------------------------------------------------------------
// Legacy API Implementation
//------------------------------------------------------------------------------

uint64_t efp_init_send(uint64_t aMtu,
                       void (*aCallback)(const uint8_t*, size_t, uint8_t, void*),
                       void* apCtx) {
    auto lpSender = efp_sender_create((uint16_t)(aMtu));
    if (!lpSender) {
        return 0;
    }

    efp_sender_set_callback(lpSender, aCallback, apCtx);

    const std::lock_guard<std::recursive_mutex> lLock(gHandlesMutex);
    auto lHandle = gNextHandle++;
    gSenderHandles[lHandle] = lpSender;
    return lHandle;
}

uint64_t efp_init_receive(uint32_t aBucketTimeout, uint32_t aHolTimeout,
                          void (*aCallback)(uint8_t*, size_t, uint8_t, uint8_t, uint64_t,
                                   uint64_t, uint32_t, uint8_t, uint8_t, uint8_t, void*),
                          void (*aEmbeddedCallback)(uint8_t*, size_t, uint8_t, uint64_t, void*),
                          void* apCtx,
                          uint32_t aMode) {
    auto lpReceiver = efp_receiver_create(aBucketTimeout, aHolTimeout, aMode);
    if (!lpReceiver) {
        return 0;
    }

    efp_receiver_set_callback(lpReceiver, aCallback, apCtx);
    efp_receiver_set_embedded_callback(lpReceiver, aEmbeddedCallback, apCtx);

    const std::lock_guard<std::recursive_mutex> lLock(gHandlesMutex);
    auto lHandle = gNextHandle++;
    gReceiverHandles[lHandle] = lpReceiver;
    return lHandle;
}

int16_t efp_end_send(uint64_t aEfpObject) {
    const std::lock_guard<std::recursive_mutex> lLock(gHandlesMutex);

    auto lIt = gSenderHandles.find(aEfpObject);
    if (lIt == gSenderHandles.end()) {
        return EFP_INVALID_PARAMETER;
    }

    efp_sender_destroy(lIt->second);
    gSenderHandles.erase(lIt);
    return EFP_OK;
}

int16_t efp_end_receive(uint64_t aEfpObject) {
    const std::lock_guard<std::recursive_mutex> lLock(gHandlesMutex);

    auto lIt = gReceiverHandles.find(aEfpObject);
    if (lIt == gReceiverHandles.end()) {
        return EFP_INVALID_PARAMETER;
    }

    efp_receiver_destroy(lIt->second);
    gReceiverHandles.erase(lIt);
    return EFP_OK;
}

int16_t efp_send_data(uint64_t aEfpObject,
                      const uint8_t* apData, size_t aSize,
                      uint8_t aDataContent,
                      uint64_t aPts, uint64_t aDts,
                      uint32_t aCode,
                      uint8_t aStreamId,
                      uint8_t aFlags) {
    const std::lock_guard<std::recursive_mutex> lLock(gHandlesMutex);

    auto lIt = gSenderHandles.find(aEfpObject);
    if (lIt == gSenderHandles.end()) {
        return EFP_INVALID_PARAMETER;
    }

    return efp_sender_send(lIt->second, apData, aSize, aDataContent, aPts, aDts,
                          aCode, aStreamId, aFlags);
}

int16_t efp_receive_fragment(uint64_t aEfpObject,
                             const uint8_t* apFragment, size_t aSize,
                             uint8_t aFromSource) {
    const std::lock_guard<std::recursive_mutex> lLock(gHandlesMutex);

    auto lIt = gReceiverHandles.find(aEfpObject);
    if (lIt == gReceiverHandles.end()) {
        return EFP_INVALID_PARAMETER;
    }

    return efp_receiver_receive(lIt->second, apFragment, aSize, aFromSource);
}

