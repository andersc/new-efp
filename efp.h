//
// Elastic Frame Protocol - Main Header
// Copyright 2024-2026
//
// A lightweight, generic data framing protocol for fragmenting and
// reassembling data over unreliable or size-limited transport layers.
//
// Zero-overhead template callbacks for maximum performance.
//

#ifndef EFP_H
#define EFP_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>
#include <map>
#include <bitset>
#include <mutex>
#include <atomic>
#include <thread>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <span>
#include <bit>
#include <concepts>
#include <stop_token>
#include <type_traits>

#include "efp_internal.h"

namespace efp {

// Version info
constexpr uint8_t  VERSION_MAJOR = 1;
constexpr uint8_t  VERSION_MINOR = 1;
constexpr uint16_t VERSION = ((uint16_t)(VERSION_MAJOR) << 8) | VERSION_MINOR;

// Default circular buffer size (must be 2^n - 1 for bitmask operations)
constexpr uint16_t DEFAULT_BUFFER_SIZE = 8191;

// C++20 concept for valid buffer sizes (must be 2^n - 1)
template<uint16_t N>
concept ValidBufferSize = std::has_single_bit(static_cast<unsigned>(N + 1));

// Forward declarations
class SuperFrame;
using SuperFramePtr = std::unique_ptr<SuperFrame>;

// C++20 concepts for callback validation
template<typename T>
concept SendCallbackConcept = std::invocable<T, std::span<const uint8_t>, uint8_t>;

template<typename T>
concept ReceiveCallbackConcept = std::invocable<T, SuperFramePtr>;

// Result codes
enum class Result : int16_t {
    // Errors (negative)
    MEMORY_ALLOCATION_ERROR  = -10,
    BUFFER_OUT_OF_BOUNDS     = -9,
    BUFFER_OUT_OF_RESOURCES  = -8,
    FRAME_SIZE_MISMATCH      = -7,
    TOO_LARGE_FRAME          = -6,
    TOO_LARGE_EMBEDDED_DATA  = -5,
    INVALID_PARAMETER        = -4,
    RECEIVER_NOT_RUNNING     = -3,
    INTERNAL_ERROR           = -2,
    NOT_IMPLEMENTED          = -1,

    // Success
    OK                       = 0,

    // Informational (positive)
    DUPLICATE_FRAGMENT       = 1,
    FRAGMENT_TOO_OLD         = 2,
    FRAME_TIMEOUT            = 3,
};

// Receiver operating modes
enum class ReceiverMode : uint8_t {
    THREADED         = 1,  // Background threads handle assembly and delivery
    RUN_TO_COMPLETION = 2   // Caller-driven, no internal threads
};

//------------------------------------------------------------------------------
// SuperFrame: Assembled data frame delivered by receiver
//------------------------------------------------------------------------------
class SuperFrame {
public:
    uint8_t* mpData        = nullptr;       // Pointer to frame data (32-byte aligned)
    size_t   mSize         = 0;             // Frame size in bytes
    uint8_t  mPayloadType  = 0;             // User-defined payload type
    uint32_t mPayloadCode  = UINT32_MAX;    // User-defined payload code
    uint64_t mPts          = UINT64_MAX;    // Presentation timestamp
    uint64_t mDts          = UINT64_MAX;    // Decode timestamp
    uint8_t  mStreamId     = 0;             // Stream identifier
    uint8_t  mSourceId     = 0;             // Source identifier (passed through)
    uint8_t  mFlags        = 0;             // Flags from the frame
    uint16_t mSuperFrameNo = 0;             // Sequence number
    bool     mBroken       = true;          // True if frame is incomplete

    SuperFrame() = default;
    SuperFrame(const SuperFrame&) = delete;
    SuperFrame& operator=(const SuperFrame&) = delete;

    explicit SuperFrame(size_t aAllocSize) {
        if (aAllocSize > 0) {
#ifdef _WIN64
            mpData = (uint8_t*)(_aligned_malloc(aAllocSize, 32));
#else
            if (posix_memalign((void**)(&mpData), 32, aAllocSize) != 0) {
                mpData = nullptr;
            }
#endif
            if (mpData) {
                mSize = aAllocSize;
            }
        }
    }

    ~SuperFrame() {
        if (mpData) {
#ifdef _WIN64
            _aligned_free(mpData);
#else
            free(mpData);
#endif
        }
    }
};

// SuperFramePtr already forward-declared above

//------------------------------------------------------------------------------
// Sender: Fragments data into EFP packets
// Template callback for zero-overhead invocation (no std::function)
//------------------------------------------------------------------------------
template<typename SendCallbackT, uint16_t BUFFER_SIZE = DEFAULT_BUFFER_SIZE>
    requires ValidBufferSize<BUFFER_SIZE> && SendCallbackConcept<SendCallbackT>
class Sender {
public:
    explicit Sender(uint16_t aMtu, SendCallbackT aCallback)
        : mMtu(aMtu), mCallback(std::move(aCallback)) {
        if (mMtu < 256) mMtu = 256;
        mSendBuffer.resize(mMtu);
    }

    ~Sender() = default;

    // Non-copyable, non-movable
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    Sender(Sender&&) = delete;
    Sender& operator=(Sender&&) = delete;

    // Get version
    [[nodiscard]] static consteval uint16_t version() noexcept { return VERSION; }

    // Pack and send data using span (primary API)
    [[nodiscard]] Result send(std::span<const uint8_t> aData,
                uint8_t aPayloadType, uint64_t aPts, uint64_t aDts,
                uint32_t aPayloadCode, uint8_t aStreamId, uint8_t aFlags = Flags::NONE) {

        if (aData.empty()) [[unlikely]] {
            return Result::INVALID_PARAMETER;
        }

        std::lock_guard<std::mutex> lLock(mMutex);

        auto lType1PayloadSize = mMtu - sizeof(FrameType1);
        auto lType2PayloadSize = mMtu - sizeof(FrameType2);

        // Calculate DTS-PTS difference
        auto lDtsPtsDiff = UINT32_MAX;
        if (aDts != UINT64_MAX && aPts != UINT64_MAX && aPts >= aDts) [[likely]] {
            auto lDiff = aPts - aDts;
            if (lDiff <= UINT32_MAX - 1) {
                lDtsPtsDiff = (uint32_t)(lDiff);
            }
        }

        // Single small frame? Use Type2 only
        if (aData.size() <= lType2PayloadSize) [[likely]] {
            return sendType2Only(aData.data(), aData.size(), aPayloadType, aPts, lDtsPtsDiff,
                                 aPayloadCode, aStreamId, aFlags);
        }

        // Multiple fragments needed
        return sendFragmented(aData.data(), aData.size(), aPayloadType, aPts, lDtsPtsDiff,
                              aPayloadCode, aStreamId, aFlags, lType1PayloadSize);
    }

    // Convenience overload for vector
    [[nodiscard]] Result send(const std::vector<uint8_t>& aData,
                uint8_t aPayloadType, uint64_t aPts, uint64_t aDts,
                uint32_t aPayloadCode, uint8_t aStreamId, uint8_t aFlags = Flags::NONE) {
        return send(std::span<const uint8_t>(aData), aPayloadType, aPts, aDts,
                    aPayloadCode, aStreamId, aFlags);
    }

private:
    void invokeCallback(size_t aSize, uint8_t aStreamId) {
        mCallback(std::span<const uint8_t>(mSendBuffer.data(), aSize), aStreamId);
    }

    Result sendType2Only(const uint8_t* apData, size_t aSize,
                         uint8_t aPayloadType, uint64_t aPts, uint32_t aDtsPtsDiff,
                         uint32_t aPayloadCode, uint8_t aStreamId, uint8_t aFlags) {

        FrameType2 lHeader;
        lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE2, aFlags);
        lHeader.mStreamId = aStreamId;
        lHeader.mPayloadType = aPayloadType;
        lHeader.mSizeOfData = (uint16_t)(aSize);
        lHeader.mSuperFrameNo = mSuperFrameNo++;
        lHeader.mOfFragmentNo = 0;
        lHeader.mType1PacketSize = 0;
        lHeader.mPts = aPts;
        lHeader.mDtsPtsDiff = aDtsPtsDiff;
        lHeader.mPayloadCode = aPayloadCode;

        std::memcpy(mSendBuffer.data(), &lHeader, sizeof(lHeader));
        std::memcpy(mSendBuffer.data() + sizeof(lHeader), apData, aSize);

        invokeCallback(sizeof(lHeader) + aSize, aStreamId);

        return Result::OK;
    }

    Result sendFragmented(const uint8_t* apData, size_t aSize,
                          uint8_t aPayloadType, uint64_t aPts, uint32_t aDtsPtsDiff,
                          uint32_t aPayloadCode, uint8_t aStreamId, uint8_t aFlags,
                          size_t aType1PayloadSize) {

        auto lType2HeaderSize = sizeof(FrameType2);

        // Calculate fragment count
        // Last fragment uses Type2, may need Type3 for penultimate
        auto lRemainingAfterType1s = aSize % aType1PayloadSize;
        auto lNumType1Fragments = aSize / aType1PayloadSize;

        uint16_t lTotalFragments;
        auto lNeedsType3 = false;
        size_t lType2DataSize;
        size_t lType3DataSize = 0;

        if (lRemainingAfterType1s == 0) {
            // Perfect fit into Type1s, last one becomes Type2 with full payload
            lTotalFragments = (uint16_t)(lNumType1Fragments);
            lType2DataSize = aType1PayloadSize;
            lNumType1Fragments--;
        } else if (lRemainingAfterType1s <= (mMtu - lType2HeaderSize)) {
            // Remainder fits in Type2
            lTotalFragments = (uint16_t)(lNumType1Fragments + 1);
            lType2DataSize = lRemainingAfterType1s;
        } else {
            // Need Type3 for overflow
            lNeedsType3 = true;
            lTotalFragments = (uint16_t)(lNumType1Fragments + 2);
            lType3DataSize = aType1PayloadSize;  // Type3 carries full fragment
            lType2DataSize = lRemainingAfterType1s - aType1PayloadSize + (mMtu - lType2HeaderSize);
            // Recalculate: remaining split between Type3 and Type2
            auto lCombinedSpace = aType1PayloadSize + (mMtu - lType2HeaderSize);
            if (lRemainingAfterType1s <= lCombinedSpace) {
                lType3DataSize = lRemainingAfterType1s - (mMtu - lType2HeaderSize);
                if (lType3DataSize > aType1PayloadSize) {
                    lType3DataSize = aType1PayloadSize;
                }
                lType2DataSize = lRemainingAfterType1s - lType3DataSize;
            }
        }

        auto lSuperFrameNo = mSuperFrameNo++;
        auto lOfFragmentNo = (uint16_t)(lTotalFragments - 1);
        size_t lDataOffset = 0;

        // Send Type1 fragments
        for (size_t lFragNo = 0; lFragNo < lNumType1Fragments; lFragNo++) {
            FrameType1 lHeader;
            lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE1, aFlags);
            lHeader.mStreamId = aStreamId;
            lHeader.mSuperFrameNo = lSuperFrameNo;
            lHeader.mFragmentNo = (uint16_t)(lFragNo);
            lHeader.mOfFragmentNo = lOfFragmentNo;

            std::memcpy(mSendBuffer.data(), &lHeader, sizeof(lHeader));
            std::memcpy(mSendBuffer.data() + sizeof(lHeader), apData + lDataOffset, aType1PayloadSize);
            lDataOffset += aType1PayloadSize;

            invokeCallback(mMtu, aStreamId);
        }

        // Send Type3 if needed (penultimate fragment)
        if (lNeedsType3) [[unlikely]] {
            FrameType3 lHeader;
            lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE3, aFlags);
            lHeader.mStreamId = aStreamId;
            lHeader.mSuperFrameNo = lSuperFrameNo;
            lHeader.mType1PacketSize = (uint16_t)(aType1PayloadSize);
            lHeader.mOfFragmentNo = lOfFragmentNo;

            std::memcpy(mSendBuffer.data(), &lHeader, sizeof(lHeader));
            std::memcpy(mSendBuffer.data() + sizeof(lHeader), apData + lDataOffset, lType3DataSize);
            lDataOffset += lType3DataSize;

            invokeCallback(sizeof(lHeader) + lType3DataSize, aStreamId);
        }

        // Send Type2 (final fragment)
        FrameType2 lHeader;
        lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE2, aFlags);
        lHeader.mStreamId = aStreamId;
        lHeader.mPayloadType = aPayloadType;
        lHeader.mSizeOfData = (uint16_t)(lType2DataSize);
        lHeader.mSuperFrameNo = lSuperFrameNo;
        lHeader.mOfFragmentNo = lOfFragmentNo;
        lHeader.mType1PacketSize = (uint16_t)(aType1PayloadSize);
        lHeader.mPts = aPts;
        lHeader.mDtsPtsDiff = aDtsPtsDiff;
        lHeader.mPayloadCode = aPayloadCode;

        std::memcpy(mSendBuffer.data(), &lHeader, sizeof(lHeader));
        std::memcpy(mSendBuffer.data() + sizeof(lHeader), apData + lDataOffset, lType2DataSize);

        invokeCallback(sizeof(lHeader) + lType2DataSize, aStreamId);

        return Result::OK;
    }

    uint16_t mMtu;
    uint16_t mSuperFrameNo = 0;
    std::mutex mMutex;
    std::vector<uint8_t> mSendBuffer;
    [[no_unique_address]] SendCallbackT mCallback;
};

// Deduction guide for Sender
template<typename SendCallbackT>
Sender(uint16_t, SendCallbackT) -> Sender<SendCallbackT>;

// Factory function for easier instantiation
template<typename SendCallbackT, uint16_t BUFFER_SIZE = DEFAULT_BUFFER_SIZE>
    requires SendCallbackConcept<SendCallbackT>
[[nodiscard]] auto makeSender(uint16_t aMtu, SendCallbackT aCallback) {
    return Sender<SendCallbackT, BUFFER_SIZE>(aMtu, std::move(aCallback));
}

//------------------------------------------------------------------------------
// Receiver: Reassembles EFP fragments into SuperFrames
// Template callback for zero-overhead invocation (no std::function)
//------------------------------------------------------------------------------
template<typename ReceiveCallbackT, uint16_t BUFFER_SIZE = DEFAULT_BUFFER_SIZE>
    requires ValidBufferSize<BUFFER_SIZE> && ReceiveCallbackConcept<ReceiveCallbackT>
class Receiver {
public:
    explicit Receiver(ReceiveCallbackT aCallback, uint32_t aTimeoutMs = 100,
                      uint32_t aHolTimeoutMs = 0,
                      ReceiverMode aMode = ReceiverMode::THREADED)
        : mCallback(std::move(aCallback)), mTimeoutMs(aTimeoutMs),
          mHolTimeoutMs(aHolTimeoutMs), mMode(aMode) {

        mpBuckets = new Bucket[BUFFER_SIZE + 1];

        if (mMode == ReceiverMode::THREADED) {
            mRunning = true;
            mWorkerThread = std::jthread([this](std::stop_token aStopToken) {
                workerLoop(aStopToken);
            });
            mDeliveryThread = std::jthread([this](std::stop_token aStopToken) {
                deliveryLoop(aStopToken);
            });
        }
    }

    ~Receiver() {
        stop();
        delete[] mpBuckets;
    }

    // Non-copyable, non-movable
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    Receiver(Receiver&&) = delete;
    Receiver& operator=(Receiver&&) = delete;

    // Get version
    [[nodiscard]] static consteval uint16_t version() noexcept { return VERSION; }

    // Receive a fragment using span (primary API)
    [[nodiscard]] Result receive(std::span<const uint8_t> aData, uint8_t aSourceId = 0) {
        if (aData.empty()) [[unlikely]] {
            return Result::INVALID_PARAMETER;
        }

        auto lType = getFrameType(aData[0]);
        Result lResult;

        switch (lType) {
            case FrameType::TYPE0:
                lResult = handleType0(aData.data(), aData.size(), aSourceId);
                break;
            case FrameType::TYPE1:
                lResult = handleType1(aData.data(), aData.size(), aSourceId);
                break;
            case FrameType::TYPE2:
                lResult = handleType2(aData.data(), aData.size(), aSourceId);
                break;
            case FrameType::TYPE3:
                lResult = handleType3(aData.data(), aData.size(), aSourceId);
                break;
            default:
                return Result::INVALID_PARAMETER;
        }

        // In RUN_TO_COMPLETION mode, automatically process completed frames
        if (mMode == ReceiverMode::RUN_TO_COMPLETION) [[unlikely]] {
            std::lock_guard<std::recursive_mutex> lLock(mNetMutex);
            processTimeouts();
        }

        return lResult;
    }

    // Convenience overload for vector
    [[nodiscard]] Result receive(const std::vector<uint8_t>& aData, uint8_t aSourceId = 0) {
        return receive(std::span<const uint8_t>(aData), aSourceId);
    }

    // For RUN_TO_COMPLETION mode: process timeouts and deliver frames
    void poll() {
        if (mMode != ReceiverMode::RUN_TO_COMPLETION) [[likely]] return;

        std::lock_guard<std::recursive_mutex> lLock(mNetMutex);
        processTimeouts();
    }

    // Get number of pending (incomplete) frames in the bucket map
    [[nodiscard]] size_t pendingCount() const {
        return mBucketMap.size();
    }

    // Stop receiver threads (jthreads auto-join on destruction but this allows early stop)
    void stop() {
        auto lExpected = true;
        if (!mRunning.compare_exchange_strong(lExpected, false)) {
            return;  // Already stopped or not started
        }

        // Request stop on jthreads
        mWorkerThread.request_stop();
        mDeliveryThread.request_stop();
        mDeliveryCondition.notify_all();

        // jthreads auto-join, but we can explicitly join for deterministic behavior
        if (mWorkerThread.joinable()) {
            mWorkerThread.join();
        }
        if (mDeliveryThread.joinable()) {
            mDeliveryThread.join();
        }
    }

private:
    struct Stream {
        uint8_t  mPayloadType = 0;
        uint32_t mPayloadCode = UINT32_MAX;
    };

    struct Bucket {
        bool     mActive         = false;
        uint8_t  mPayloadType    = 0;
        uint32_t mPayloadCode    = UINT32_MAX;
        uint16_t mSavedFrameNo   = 0;
        int64_t  mTimeoutUs      = 0;
        uint16_t mFragmentCount  = 0;
        uint16_t mOfFragmentNo   = 0;
        uint64_t mDeliveryOrder  = UINT64_MAX;
        size_t   mFragmentSize   = 0;
        size_t   mType3Size      = 0;  // Size of Type3 payload (0 if no Type3)
        size_t   mType2Size      = 0;  // Size of Type2 payload (for relocation if Type3 arrives after)
        uint64_t mPts            = UINT64_MAX;
        uint64_t mDts            = UINT64_MAX;
        uint8_t  mStreamId       = 0;
        uint8_t  mSourceId       = 0;
        uint8_t  mFlags          = 0;
        std::bitset<8192> mReceivedFragments;  // Max fragments per superframe
        SuperFramePtr mpFrame;
    };

    [[nodiscard]] int64_t nowUs() const noexcept {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    [[nodiscard]] uint64_t recalculateSuperFrameNo(uint16_t aFrameNo) noexcept {
        if (mFirstFrame) [[unlikely]] {
            mOldFrameNo = aFrameNo;
            mFrameNoRecalc = aFrameNo;
            mFirstFrame = false;
            return mFrameNoRecalc;
        }
        // Calculate delta using unsigned subtraction then interpret as signed
        // This correctly handles the 16-bit wrap-around without overflow
        auto lDelta = (int16_t)(uint16_t)(aFrameNo - mOldFrameNo);
        mOldFrameNo = aFrameNo;
        mFrameNoRecalc += lDelta;
        return mFrameNoRecalc;
    }

    Result handleType0(const uint8_t* /*apData*/, size_t /*aSize*/, uint8_t /*aSourceId*/) {
        // Type0 signaling - pass through or handle separately
        return Result::OK;
    }

    Result handleType1(const uint8_t* apData, size_t aSize, uint8_t aSourceId) {
        if (aSize < sizeof(FrameType1)) [[unlikely]] {
            return Result::FRAME_SIZE_MISMATCH;
        }

        std::lock_guard<std::recursive_mutex> lLock(mNetMutex);

        auto* lpHeader = (const FrameType1*)(apData);

        // Bounds check: fragmentNo and ofFragmentNo must fit in bitset (8192 max)
        if (lpHeader->mFragmentNo >= 8192 || lpHeader->mOfFragmentNo >= 8192) [[unlikely]] {
            return Result::BUFFER_OUT_OF_BOUNDS;
        }

        // Check fragmentNo doesn't exceed ofFragmentNo
        if (lpHeader->mFragmentNo > lpHeader->mOfFragmentNo) [[unlikely]] {
            return Result::BUFFER_OUT_OF_BOUNDS;
        }

        auto* lpBucket = &mpBuckets[lpHeader->mSuperFrameNo & BUFFER_SIZE];

        auto lPayloadSize = aSize - sizeof(FrameType1);

        if (!lpBucket->mActive) [[unlikely]] {
            auto lOrder = recalculateSuperFrameNo(lpHeader->mSuperFrameNo);
            if (lOrder == lpBucket->mDeliveryOrder) [[unlikely]] {
                return Result::FRAGMENT_TOO_OLD;
            }

            // Sanity check on total size to prevent huge allocations
            auto lTotalSize = lPayloadSize * ((size_t)(lpHeader->mOfFragmentNo) + 1);
            if (lTotalSize > 100 * 1024 * 1024) [[unlikely]] {  // 100MB max
                return Result::TOO_LARGE_FRAME;
            }

            lpBucket->mDeliveryOrder = lOrder;
            mBucketMap[lOrder] = lpBucket;
            lpBucket->mActive = true;
            lpBucket->mSourceId = aSourceId;
            lpBucket->mFlags = getFlags(lpHeader->mFrameType);
            lpBucket->mStreamId = lpHeader->mStreamId;
            lpBucket->mSavedFrameNo = lpHeader->mSuperFrameNo;
            lpBucket->mReceivedFragments.reset();
            lpBucket->mReceivedFragments[lpHeader->mFragmentNo] = true;
            lpBucket->mTimeoutUs = nowUs() + (mTimeoutMs * 1000);
            lpBucket->mFragmentCount = 1;  // First fragment received
            lpBucket->mOfFragmentNo = lpHeader->mOfFragmentNo;
            lpBucket->mFragmentSize = lPayloadSize;
            lpBucket->mPts = UINT64_MAX;
            lpBucket->mDts = UINT64_MAX;

            // Get cached stream info
            auto* lpStream = &mStreams[lpHeader->mStreamId];
            lpBucket->mPayloadType = lpStream->mPayloadType;
            lpBucket->mPayloadCode = lpStream->mPayloadCode;

            lpBucket->mpFrame = std::make_unique<SuperFrame>(lTotalSize);
            if (!lpBucket->mpFrame->mpData) [[unlikely]] {
                mBucketMap.erase(lOrder);
                lpBucket->mActive = false;
                return Result::MEMORY_ALLOCATION_ERROR;
            }
            lpBucket->mpFrame->mSize = lTotalSize;  // Set expected full frame size

            auto lOffset = lPayloadSize * lpHeader->mFragmentNo;
            std::memcpy(lpBucket->mpFrame->mpData + lOffset, apData + sizeof(FrameType1), lPayloadSize);

            return Result::OK;
        }

        // Existing bucket
        if (lpHeader->mSuperFrameNo != lpBucket->mSavedFrameNo) [[unlikely]] {
            return Result::BUFFER_OUT_OF_RESOURCES;
        }

        if (lpHeader->mOfFragmentNo != lpBucket->mOfFragmentNo ||
            lpHeader->mFragmentNo > lpBucket->mOfFragmentNo) [[unlikely]] {
            mBucketMap.erase(lpBucket->mDeliveryOrder);
            lpBucket->mActive = false;
            return Result::BUFFER_OUT_OF_BOUNDS;
        }

        if (lpBucket->mReceivedFragments[lpHeader->mFragmentNo]) [[unlikely]] {
            return Result::DUPLICATE_FRAGMENT;
        }

        lpBucket->mReceivedFragments[lpHeader->mFragmentNo] = true;
        lpBucket->mFragmentCount++;

        auto lOffset = lpBucket->mFragmentSize * lpHeader->mFragmentNo;
        std::memcpy(lpBucket->mpFrame->mpData + lOffset, apData + sizeof(FrameType1), lPayloadSize);

        return Result::OK;
    }

    Result handleType2(const uint8_t* apData, size_t aSize, uint8_t aSourceId) {
        if (aSize < sizeof(FrameType2)) [[unlikely]] {
            return Result::FRAME_SIZE_MISMATCH;
        }

        std::lock_guard<std::recursive_mutex> lLock(mNetMutex);

        auto* lpHeader = (const FrameType2*)(apData);

        if (aSize < sizeof(FrameType2) + lpHeader->mSizeOfData) [[unlikely]] {
            return Result::FRAME_SIZE_MISMATCH;
        }

        // Bounds check: ofFragmentNo must fit in bitset (8192 max)
        if (lpHeader->mOfFragmentNo >= 8192) [[unlikely]] {
            return Result::BUFFER_OUT_OF_BOUNDS;
        }

        // Sanity check on total size
        auto lTotalSize = ((size_t)(lpHeader->mType1PacketSize) * lpHeader->mOfFragmentNo) +
                          lpHeader->mSizeOfData;
        if (lTotalSize > 100 * 1024 * 1024) [[unlikely]] {  // 100MB max
            return Result::TOO_LARGE_FRAME;
        }

        auto* lpBucket = &mpBuckets[lpHeader->mSuperFrameNo & BUFFER_SIZE];

        if (!lpBucket->mActive) [[unlikely]] {
            auto lOrder = recalculateSuperFrameNo(lpHeader->mSuperFrameNo);
            if (lOrder == lpBucket->mDeliveryOrder) [[unlikely]] {
                return Result::FRAGMENT_TOO_OLD;
            }

            lpBucket->mDeliveryOrder = lOrder;
            mBucketMap[lOrder] = lpBucket;
            lpBucket->mActive = true;
            lpBucket->mSourceId = aSourceId;
            lpBucket->mFlags = getFlags(lpHeader->mFrameType);
            lpBucket->mStreamId = lpHeader->mStreamId;
            lpBucket->mSavedFrameNo = lpHeader->mSuperFrameNo;
            lpBucket->mReceivedFragments.reset();
            lpBucket->mReceivedFragments[lpHeader->mOfFragmentNo] = true;
            lpBucket->mTimeoutUs = nowUs() + (mTimeoutMs * 1000);
            lpBucket->mFragmentCount = 1;  // First fragment received
            lpBucket->mOfFragmentNo = lpHeader->mOfFragmentNo;
            lpBucket->mFragmentSize = lpHeader->mType1PacketSize;
            lpBucket->mType2Size = lpHeader->mSizeOfData;  // Store for potential relocation
            lpBucket->mPts = lpHeader->mPts;
            lpBucket->mPayloadType = lpHeader->mPayloadType;
            lpBucket->mPayloadCode = lpHeader->mPayloadCode;

            if (lpHeader->mDtsPtsDiff == UINT32_MAX) [[unlikely]] {
                lpBucket->mDts = UINT64_MAX;
            } else {
                lpBucket->mDts = lpHeader->mPts - lpHeader->mDtsPtsDiff;
            }

            // Update stream cache
            auto* lpStream = &mStreams[lpHeader->mStreamId];
            lpStream->mPayloadType = lpHeader->mPayloadType;
            lpStream->mPayloadCode = lpHeader->mPayloadCode;

            lpBucket->mpFrame = std::make_unique<SuperFrame>(lTotalSize);
            if (!lpBucket->mpFrame->mpData) [[unlikely]] {
                mBucketMap.erase(lOrder);
                lpBucket->mActive = false;
                return Result::MEMORY_ALLOCATION_ERROR;
            }
            lpBucket->mpFrame->mSize = lTotalSize;  // Explicitly set frame size

            auto lOffset = (size_t)(lpHeader->mType1PacketSize) * lpHeader->mOfFragmentNo;
            std::memcpy(lpBucket->mpFrame->mpData + lOffset, apData + sizeof(FrameType2), lpHeader->mSizeOfData);

            return Result::OK;
        }

        // Existing bucket
        if (lpHeader->mSuperFrameNo != lpBucket->mSavedFrameNo) [[unlikely]] {
            return Result::BUFFER_OUT_OF_RESOURCES;
        }

        if (lpHeader->mOfFragmentNo != lpBucket->mOfFragmentNo) [[unlikely]] {
            mBucketMap.erase(lpBucket->mDeliveryOrder);
            lpBucket->mActive = false;
            return Result::BUFFER_OUT_OF_BOUNDS;
        }

        if (lpBucket->mReceivedFragments[lpHeader->mOfFragmentNo]) [[unlikely]] {
            return Result::DUPLICATE_FRAGMENT;
        }

        lpBucket->mReceivedFragments[lpHeader->mOfFragmentNo] = true;
        lpBucket->mFragmentCount++;
        lpBucket->mPts = lpHeader->mPts;
        lpBucket->mPayloadType = lpHeader->mPayloadType;
        lpBucket->mPayloadCode = lpHeader->mPayloadCode;
        lpBucket->mFlags = getFlags(lpHeader->mFrameType);
        lpBucket->mType2Size = lpHeader->mSizeOfData;  // Store for potential relocation by Type3

        if (lpHeader->mDtsPtsDiff == UINT32_MAX) [[unlikely]] {
            lpBucket->mDts = UINT64_MAX;
        } else {
            lpBucket->mDts = lpHeader->mPts - lpHeader->mDtsPtsDiff;
        }

        // Update stream cache
        auto* lpStream = &mStreams[lpHeader->mStreamId];
        lpStream->mPayloadType = lpHeader->mPayloadType;
        lpStream->mPayloadCode = lpHeader->mPayloadCode;

        // Set actual frame size, accounting for Type3 if present
        size_t lOffset;
        if (lpBucket->mType3Size > 0) [[unlikely]] {
            // Type3 exists: size = type1 fragments + type3 + type2
            lpBucket->mpFrame->mSize = (lpBucket->mFragmentSize * (lpHeader->mOfFragmentNo - 1)) +
                                       lpBucket->mType3Size + lpHeader->mSizeOfData;
            // Type2 data follows Type3 data
            lOffset = (lpBucket->mFragmentSize * (lpHeader->mOfFragmentNo - 1)) + lpBucket->mType3Size;
        } else {
            // No Type3: size = type1 fragments + type2
            lpBucket->mpFrame->mSize = (lpBucket->mFragmentSize * lpHeader->mOfFragmentNo) + lpHeader->mSizeOfData;
            lOffset = (size_t)(lpHeader->mType1PacketSize) * lpHeader->mOfFragmentNo;
        }

        std::memcpy(lpBucket->mpFrame->mpData + lOffset, apData + sizeof(FrameType2), lpHeader->mSizeOfData);

        return Result::OK;
    }

    Result handleType3(const uint8_t* apData, size_t aSize, uint8_t aSourceId) {
        if (aSize < sizeof(FrameType3)) [[unlikely]] {
            return Result::FRAME_SIZE_MISMATCH;
        }

        std::lock_guard<std::recursive_mutex> lLock(mNetMutex);

        auto* lpHeader = (const FrameType3*)(apData);

        // Bounds check: ofFragmentNo must fit in bitset and be > 0 for Type3
        if (lpHeader->mOfFragmentNo >= 8192 || lpHeader->mOfFragmentNo == 0) [[unlikely]] {
            return Result::BUFFER_OUT_OF_BOUNDS;
        }

        auto* lpBucket = &mpBuckets[lpHeader->mSuperFrameNo & BUFFER_SIZE];

        auto lFragmentNo = (uint16_t)(lpHeader->mOfFragmentNo - 1);  // Type3 is always penultimate
        auto lPayloadSize = aSize - sizeof(FrameType3);

        // Sanity check on total size
        auto lTotalSize = ((size_t)(lpHeader->mType1PacketSize) * (lpHeader->mOfFragmentNo - 1)) +
                          lPayloadSize;
        if (lTotalSize > 100 * 1024 * 1024) [[unlikely]] {  // 100MB max
            return Result::TOO_LARGE_FRAME;
        }

        if (!lpBucket->mActive) [[unlikely]] {
            auto lOrder = recalculateSuperFrameNo(lpHeader->mSuperFrameNo);
            if (lOrder == lpBucket->mDeliveryOrder) [[unlikely]] {
                return Result::FRAGMENT_TOO_OLD;
            }

            lpBucket->mDeliveryOrder = lOrder;
            mBucketMap[lOrder] = lpBucket;
            lpBucket->mActive = true;
            lpBucket->mSourceId = aSourceId;
            lpBucket->mFlags = getFlags(lpHeader->mFrameType);
            lpBucket->mStreamId = lpHeader->mStreamId;
            lpBucket->mSavedFrameNo = lpHeader->mSuperFrameNo;
            lpBucket->mReceivedFragments.reset();
            lpBucket->mReceivedFragments[lFragmentNo] = true;
            lpBucket->mTimeoutUs = nowUs() + (mTimeoutMs * 1000);
            lpBucket->mFragmentCount = 1;  // First fragment received
            lpBucket->mOfFragmentNo = lpHeader->mOfFragmentNo;
            lpBucket->mFragmentSize = lpHeader->mType1PacketSize;
            lpBucket->mType3Size = lPayloadSize;  // Store Type3 payload size
            lpBucket->mPts = UINT64_MAX;
            lpBucket->mDts = UINT64_MAX;

            // Get cached stream info
            auto* lpStream = &mStreams[lpHeader->mStreamId];
            lpBucket->mPayloadType = lpStream->mPayloadType;
            lpBucket->mPayloadCode = lpStream->mPayloadCode;

            lpBucket->mpFrame = std::make_unique<SuperFrame>(lTotalSize);
            if (!lpBucket->mpFrame->mpData) [[unlikely]] {
                mBucketMap.erase(lOrder);
                lpBucket->mActive = false;
                return Result::MEMORY_ALLOCATION_ERROR;
            }
            lpBucket->mpFrame->mSize = lTotalSize;  // Set expected frame size

            auto lOffset = lpHeader->mType1PacketSize * lFragmentNo;
            std::memcpy(lpBucket->mpFrame->mpData + lOffset, apData + sizeof(FrameType3), lPayloadSize);

            return Result::OK;
        }

        // Existing bucket
        if (lpHeader->mSuperFrameNo != lpBucket->mSavedFrameNo) [[unlikely]] {
            return Result::BUFFER_OUT_OF_RESOURCES;
        }

        if (lpHeader->mOfFragmentNo != lpBucket->mOfFragmentNo || lFragmentNo > lpBucket->mOfFragmentNo) [[unlikely]] {
            mBucketMap.erase(lpBucket->mDeliveryOrder);
            lpBucket->mActive = false;
            return Result::BUFFER_OUT_OF_BOUNDS;
        }

        if (lpBucket->mReceivedFragments[lFragmentNo]) [[unlikely]] {
            return Result::DUPLICATE_FRAGMENT;
        }

        lpBucket->mReceivedFragments[lFragmentNo] = true;
        lpBucket->mFragmentCount++;
        lpBucket->mType3Size = lPayloadSize;  // Store Type3 payload size for Type2's calculation

        // If Type2 was received before Type3, we need to relocate Type2's data
        // Type2 was placed at fragmentSize * ofFragmentNo, but should be at
        // fragmentSize * (ofFragmentNo - 1) + type3Size
        if (lpBucket->mReceivedFragments[lpHeader->mOfFragmentNo] && lpBucket->mType2Size > 0) [[unlikely]] {
            auto lOldOffset = lpBucket->mFragmentSize * lpHeader->mOfFragmentNo;
            auto lNewOffset = (lpBucket->mFragmentSize * (lpHeader->mOfFragmentNo - 1)) + lPayloadSize;
            // Use memmove because regions may overlap
            std::memmove(lpBucket->mpFrame->mpData + lNewOffset,
                         lpBucket->mpFrame->mpData + lOldOffset,
                         lpBucket->mType2Size);
            // Update frame size now that we know type3Size
            lpBucket->mpFrame->mSize = (lpBucket->mFragmentSize * (lpHeader->mOfFragmentNo - 1)) +
                                       lPayloadSize + lpBucket->mType2Size;
        } else if (!lpBucket->mReceivedFragments[lpHeader->mOfFragmentNo]) {
            // Only update frame size if Type2 hasn't been received yet
            lpBucket->mpFrame->mSize = (lpBucket->mFragmentSize * (lpHeader->mOfFragmentNo - 1)) + lPayloadSize;
        }

        auto lOffset = lpBucket->mFragmentSize * lFragmentNo;
        std::memcpy(lpBucket->mpFrame->mpData + lOffset, apData + sizeof(FrameType3), lPayloadSize);

        return Result::OK;
    }

    void deliverFrame(Bucket* apBucket) {
        apBucket->mpFrame->mPayloadType = apBucket->mPayloadType;
        apBucket->mpFrame->mPayloadCode = apBucket->mPayloadCode;
        apBucket->mpFrame->mPts = apBucket->mPts;
        apBucket->mpFrame->mDts = apBucket->mDts;
        apBucket->mpFrame->mStreamId = apBucket->mStreamId;
        apBucket->mpFrame->mSourceId = apBucket->mSourceId;
        apBucket->mpFrame->mFlags = apBucket->mFlags;
        apBucket->mpFrame->mSuperFrameNo = apBucket->mSavedFrameNo;
        apBucket->mpFrame->mBroken = (apBucket->mFragmentCount != apBucket->mOfFragmentNo + 1);

        if (mMode == ReceiverMode::THREADED) [[likely]] {
            std::lock_guard<std::mutex> lLock(mDeliveryMutex);
            mDeliveryQueue.push_back(std::move(apBucket->mpFrame));
            mDeliveryReady = true;
            mDeliveryCondition.notify_one();
        } else {
            mCallback(std::move(apBucket->mpFrame));
        }

        mBucketMap.erase(apBucket->mDeliveryOrder);
        apBucket->mActive = false;
        apBucket->mpFrame = nullptr;
    }

    void processTimeouts() {
        auto lNow = nowUs();
        std::vector<Bucket*> lToDeliver;
        lToDeliver.reserve(16);  // Preallocate for typical case

        for (auto& [lOrder, lpBucket] : mBucketMap) {
            // Total fragments = ofFragmentNo + 1 (since ofFragmentNo is 0-based index of last fragment)
            auto lComplete = (lpBucket->mFragmentCount == lpBucket->mOfFragmentNo + 1);
            auto lTimedOut = (lpBucket->mTimeoutUs <= lNow);

            if (lComplete || lTimedOut) [[unlikely]] {
                lToDeliver.push_back(lpBucket);
            }
        }

        for (auto* lpBucket : lToDeliver) {
            deliverFrame(lpBucket);
        }
    }

    void workerLoop(std::stop_token aStopToken) {
        constexpr auto SLEEP_DURATION = std::chrono::microseconds(10000);  // 10ms

        while (!aStopToken.stop_requested() && mRunning.load()) {
            {
                std::lock_guard<std::recursive_mutex> lLock(mNetMutex);
                processTimeouts();
            }
            std::this_thread::sleep_for(SLEEP_DURATION);
        }
    }

    void deliveryLoop(std::stop_token aStopToken) {
        while (!aStopToken.stop_requested() && mRunning.load()) {
            SuperFramePtr lpFrame;
            {
                std::unique_lock<std::mutex> lLock(mDeliveryMutex);
                mDeliveryCondition.wait(lLock, [this, &aStopToken] {
                    return mDeliveryReady || !mRunning.load() || aStopToken.stop_requested();
                });

                if (aStopToken.stop_requested() || (!mRunning.load() && mDeliveryQueue.empty())) break;

                if (!mDeliveryQueue.empty()) [[likely]] {
                    lpFrame = std::move(mDeliveryQueue.front());
                    mDeliveryQueue.pop_front();
                }

                if (mDeliveryQueue.empty()) {
                    mDeliveryReady = false;
                }
            }

            if (lpFrame) [[likely]] {
                mCallback(std::move(lpFrame));
            }
        }
    }

    [[no_unique_address]] ReceiveCallbackT mCallback;
    uint32_t mTimeoutMs;
    uint32_t mHolTimeoutMs;
    ReceiverMode mMode;

    Bucket* mpBuckets;
    std::map<uint64_t, Bucket*> mBucketMap;
    Stream mStreams[256];  // All 256 stream IDs (0-255)
    std::recursive_mutex mNetMutex;

    uint16_t mOldFrameNo = 0;
    uint64_t mFrameNoRecalc = 0;
    bool mFirstFrame = true;

    std::atomic<bool> mRunning{false};
    std::jthread mWorkerThread;
    std::jthread mDeliveryThread;

    std::mutex mDeliveryMutex;
    std::deque<SuperFramePtr> mDeliveryQueue;
    std::condition_variable mDeliveryCondition;
    bool mDeliveryReady = false;
};

// Deduction guide for Receiver
template<typename ReceiveCallbackT>
Receiver(ReceiveCallbackT, uint32_t = 100, uint32_t = 0, ReceiverMode = ReceiverMode::THREADED)
    -> Receiver<ReceiveCallbackT>;

// Factory function for easier instantiation
template<typename ReceiveCallbackT, uint16_t BUFFER_SIZE = DEFAULT_BUFFER_SIZE>
    requires ReceiveCallbackConcept<ReceiveCallbackT>
[[nodiscard]] auto makeReceiver(ReceiveCallbackT aCallback, uint32_t aTimeoutMs = 100,
                                 uint32_t aHolTimeoutMs = 0,
                                 ReceiverMode aMode = ReceiverMode::THREADED) {
    return Receiver<ReceiveCallbackT, BUFFER_SIZE>(std::move(aCallback), aTimeoutMs,
                                                    aHolTimeoutMs, aMode);
}

} // namespace efp

//------------------------------------------------------------------------------
// C API - include efp_c_api.h for full C API support
//------------------------------------------------------------------------------

// Helper macro for FOURCC codes (also defined in efp_c_api.h)
#ifndef EFP_CODE
#define EFP_CODE(c0, c1, c2, c3) \
    (((uint32_t)(c0) << 24) | ((uint32_t)(c1) << 16) | ((uint32_t)(c2) << 8) | (uint32_t)(c3))
#endif

#endif // EFP_H

