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
#include <algorithm>
#include <stdexcept>

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

template<typename T>
concept NackCallbackConcept = std::invocable<T, std::span<const uint8_t>>;

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

// Sub-fragment modes for bundled transmission
enum class SubFragmentMode : uint8_t {
    SINGLE  = 1,  // Normal mode: 1 fragment per UDP packet (default)
    HALF    = 2,  // 2 fragments per UDP packet
    QUARTER = 4,  // 4 fragments per UDP packet
    EIGHTH  = 8   // 8 fragments per UDP packet
};

//------------------------------------------------------------------------------
// Statistics structures for sender and receiver
//------------------------------------------------------------------------------

struct SenderStatistics {
    uint64_t mRetentionBufferBytes      = 0;  // Current bytes in retention buffer
    uint32_t mRetentionBufferFragments  = 0;  // Current fragments in retention buffer
    uint64_t mFragmentsSent             = 0;  // Total fragments sent
    uint64_t mBundlesSent               = 0;  // Total Type4 bundles sent
    uint64_t mNacksReceived             = 0;  // Total NACKs received
    uint64_t mRetransmittedFragments    = 0;  // Total fragments retransmitted
    uint32_t mRetransmitQueueSize       = 0;  // Current retransmit queue depth
    double   mFragmentsPerSecond        = 0;  // Recent send rate (sliding window)
    double   mRetransmitsPerSecond      = 0;  // Recent retransmit rate
};

struct ReceiverStatistics {
    uint64_t mFragmentsReceived     = 0;  // Total fragments received
    uint64_t mBundlesReceived       = 0;  // Total Type4 bundles received
    uint64_t mNacksSent             = 0;  // Total NACKs sent
    uint64_t mCompleteFrames        = 0;  // Total complete SuperFrames delivered
    uint64_t mBrokenFrames          = 0;  // Total broken SuperFrames delivered
    uint64_t mDuplicateFragments    = 0;  // Duplicate fragments received
    uint32_t mPendingBuckets        = 0;  // Current active buckets
    double   mFragmentsPerSecond    = 0;  // Recent receive rate
    double   mNacksPerSecond        = 0;  // Recent NACK rate
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
    explicit Sender(uint16_t aMtu, SendCallbackT aCallback,
                    SubFragmentMode aSubFragmentMode = SubFragmentMode::SINGLE,
                    uint32_t aRetentionMs = 0,
                    size_t aRetentionMaxBytes = 50 * 1024 * 1024)
        : mMtu(aMtu), mCallback(std::move(aCallback)),
          mSubFragmentMode(aSubFragmentMode),
          mRetentionMs(aRetentionMs), mRetentionMaxBytes(aRetentionMaxBytes) {
        if (mMtu < 256) mMtu = 256;
        mSendBuffer.resize(mMtu);
        // For bundled mode, we need a larger buffer for the bundle
        if (mSubFragmentMode != SubFragmentMode::SINGLE) {
            mBundleBuffer.resize(mMtu * (uint8_t)(mSubFragmentMode) + sizeof(FrameType4));
        }
    }

    ~Sender() = default;

    // Non-copyable, non-movable
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    Sender(Sender&&) = delete;
    Sender& operator=(Sender&&) = delete;

    // Get version
    [[nodiscard]] static consteval uint16_t version() noexcept { return VERSION; }

    // Get current statistics
    [[nodiscard]] SenderStatistics getStatistics() const {
        std::lock_guard<std::mutex> lLock(mMutex);
        auto lStats = mStatistics;
        lStats.mRetentionBufferFragments = (uint32_t)(mRetentionBuffer.size());
        lStats.mRetentionBufferBytes = mRetentionBufferBytes;
        lStats.mRetransmitQueueSize = (uint32_t)(mRetransmitQueue.size());
        return lStats;
    }

    // Process incoming NACK from receiver
    [[nodiscard]] Result receiveNack(std::span<const uint8_t> aData) {
        if (aData.size() < sizeof(FrameType0Nack)) [[unlikely]] {
            return Result::FRAME_SIZE_MISMATCH;
        }

        auto* lpHeader = (const FrameType0Nack*)(aData.data());

        // Validate it's a Type0 NACK
        if (getFrameType(lpHeader->mFrameType) != FrameType::TYPE0) [[unlikely]] {
            return Result::INVALID_PARAMETER;
        }
        if (lpHeader->mSubtype != (uint8_t)(Type0Subtype::NACK)) [[unlikely]] {
            return Result::INVALID_PARAMETER;
        }

        auto lExpectedSize = sizeof(FrameType0Nack) + (lpHeader->mNackCount * sizeof(NackEntry));
        if (aData.size() < lExpectedSize) [[unlikely]] {
            return Result::FRAME_SIZE_MISMATCH;
        }

        std::lock_guard<std::mutex> lLock(mMutex);

        auto* lpEntries = (const NackEntry*)(aData.data() + sizeof(FrameType0Nack));

        for (uint8_t lI = 0; lI < lpHeader->mNackCount; lI++) {
            auto& lEntry = lpEntries[lI];

            // Process each fragment in the range
            for (uint8_t lJ = 0; lJ <= lEntry.mFragmentCount; lJ++) {
                auto lFragmentNo = (uint16_t)(lEntry.mFragmentNo + lJ);
                auto lKey = makeRetentionKey(lEntry.mSuperFrameNo, lFragmentNo);

                auto lIt = mRetentionBuffer.find(lKey);
                if (lIt != mRetentionBuffer.end()) {
                    // Add to retransmit queue if not already queued
                    RetransmitRequest lReq;
                    lReq.mSuperFrameNo = lEntry.mSuperFrameNo;
                    lReq.mFragmentNo = lFragmentNo;
                    lReq.mStreamId = lEntry.mStreamId;
                    lReq.mDeadlineUs = 0;  // No deadline
                    lReq.mPriority = 0;
                    lReq.mRetryCount = lIt->second.mRetryCount;
                    mRetransmitQueue.push_back(lReq);
                }
            }
        }

        mStatistics.mNacksReceived++;
        return Result::OK;
    }

    // Pack and send data using span (primary API)
    [[nodiscard]] Result send(std::span<const uint8_t> aData,
                uint8_t aPayloadType, uint64_t aPts, uint64_t aDts,
                uint32_t aPayloadCode, uint8_t aStreamId, uint8_t aFlags = Flags::NONE) {

        if (aData.empty()) [[unlikely]] {
            return Result::INVALID_PARAMETER;
        }

        std::lock_guard<std::mutex> lLock(mMutex);

        // Evict old retention entries
        if (mRetentionMs > 0) {
            evictOldRetention();
        }

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

    // Process pending retransmits from the queue (for SINGLE mode or manual control)
    // Returns number of fragments retransmitted
    [[nodiscard]] size_t processRetransmits(size_t aMaxCount = SIZE_MAX) {
        std::lock_guard<std::mutex> lLock(mMutex);

        size_t lCount = 0;
        while (!mRetransmitQueue.empty() && lCount < aMaxCount) {
            auto& lReq = mRetransmitQueue.front();
            auto lKey = makeRetentionKey(lReq.mSuperFrameNo, lReq.mFragmentNo);

            auto lIt = mRetentionBuffer.find(lKey);
            if (lIt != mRetentionBuffer.end()) {
                // Retransmit the fragment
                mCallback(std::span<const uint8_t>(lIt->second.mData), lIt->second.mStreamId);
                lIt->second.mRetryCount++;
                mStatistics.mRetransmittedFragments++;
                lCount++;
            }

            mRetransmitQueue.erase(mRetransmitQueue.begin());
        }

        return lCount;
    }

private:
    // Retention buffer entry
    struct RetainedFragment {
        uint64_t mTimestampUs = 0;       // When fragment was sent
        uint16_t mSuperFrameNo = 0;      // SuperFrame sequence
        uint16_t mFragmentNo = 0;        // Fragment within SuperFrame
        uint8_t  mStreamId = 0;          // Stream ID
        uint8_t  mRetryCount = 0;        // Times this has been retransmitted
        std::vector<uint8_t> mData;      // Complete fragment data (Type1/2/3 frame)
    };

    // Retransmit request entry
    struct RetransmitRequest {
        uint16_t mSuperFrameNo = 0;
        uint16_t mFragmentNo = 0;
        uint8_t  mStreamId = 0;
        uint64_t mDeadlineUs = 0;        // When this MUST be sent (0 = no deadline)
        uint8_t  mPriority = 0;          // Higher = more urgent
        uint8_t  mRetryCount = 0;        // Times requested
    };

    [[nodiscard]] static uint32_t makeRetentionKey(uint16_t aSuperFrameNo, uint16_t aFragmentNo) noexcept {
        return ((uint32_t)(aSuperFrameNo) << 16) | aFragmentNo;
    }

    [[nodiscard]] int64_t nowUs() const noexcept {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void evictOldRetention() {
        auto lNow = nowUs();
        auto lCutoffUs = lNow - (int64_t)(mRetentionMs) * 1000;

        // Evict by time
        for (auto lIt = mRetentionBuffer.begin(); lIt != mRetentionBuffer.end(); ) {
            if (lIt->second.mTimestampUs < lCutoffUs) {
                mRetentionBufferBytes -= lIt->second.mData.size();
                lIt = mRetentionBuffer.erase(lIt);
            } else {
                ++lIt;
            }
        }

        // Evict by size if still over limit
        while (mRetentionBufferBytes > mRetentionMaxBytes && !mRetentionBuffer.empty()) {
            // Find oldest entry
            auto lOldestIt = mRetentionBuffer.begin();
            for (auto lIt = mRetentionBuffer.begin(); lIt != mRetentionBuffer.end(); ++lIt) {
                if (lIt->second.mTimestampUs < lOldestIt->second.mTimestampUs) {
                    lOldestIt = lIt;
                }
            }
            mRetentionBufferBytes -= lOldestIt->second.mData.size();
            mRetentionBuffer.erase(lOldestIt);
        }
    }

    void retainFragment(uint16_t aSuperFrameNo, uint16_t aFragmentNo, uint8_t aStreamId,
                        const uint8_t* apData, size_t aSize) {
        if (mRetentionMs == 0) return;

        auto lKey = makeRetentionKey(aSuperFrameNo, aFragmentNo);

        RetainedFragment lFrag;
        lFrag.mTimestampUs = nowUs();
        lFrag.mSuperFrameNo = aSuperFrameNo;
        lFrag.mFragmentNo = aFragmentNo;
        lFrag.mStreamId = aStreamId;
        lFrag.mRetryCount = 0;
        lFrag.mData.assign(apData, apData + aSize);

        mRetentionBufferBytes += aSize;
        mRetentionBuffer[lKey] = std::move(lFrag);
    }

    void invokeCallback(size_t aSize, uint8_t aStreamId) {
        mCallback(std::span<const uint8_t>(mSendBuffer.data(), aSize), aStreamId);
        mStatistics.mFragmentsSent++;
    }

    void invokeBundleCallback(size_t aSize, uint8_t aStreamId) {
        mCallback(std::span<const uint8_t>(mBundleBuffer.data(), aSize), aStreamId);
        mStatistics.mBundlesSent++;
    }

    // Check if we should process retransmits and get one if available
    bool getRetransmitFragment(std::vector<uint8_t>& rData) {
        if (mRetransmitQueue.empty()) return false;

        auto& lReq = mRetransmitQueue.front();
        auto lKey = makeRetentionKey(lReq.mSuperFrameNo, lReq.mFragmentNo);

        auto lIt = mRetentionBuffer.find(lKey);
        if (lIt == mRetentionBuffer.end()) {
            // Fragment no longer in retention buffer, skip
            mRetransmitQueue.erase(mRetransmitQueue.begin());
            return false;
        }

        rData = lIt->second.mData;
        lIt->second.mRetryCount++;
        mStatistics.mRetransmittedFragments++;
        mRetransmitQueue.erase(mRetransmitQueue.begin());
        return true;
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

        auto lTotalSize = sizeof(lHeader) + aSize;

        // Retain if enabled
        retainFragment(lHeader.mSuperFrameNo, 0, aStreamId, mSendBuffer.data(), lTotalSize);

        invokeCallback(lTotalSize, aStreamId);

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

        // Determine bundle size based on mode
        auto lBundleSize = (uint8_t)(mSubFragmentMode);

        if (mSubFragmentMode == SubFragmentMode::SINGLE) {
            // Original behavior: send each fragment individually
            return sendFragmentedSingle(apData, aSize, aPayloadType, aPts, aDtsPtsDiff,
                                        aPayloadCode, aStreamId, aFlags, aType1PayloadSize,
                                        lSuperFrameNo, lOfFragmentNo, lNumType1Fragments,
                                        lNeedsType3, lType3DataSize, lType2DataSize);
        }

        // Bundled mode: pack multiple fragments into Type4 bundles
        return sendFragmentedBundled(apData, aSize, aPayloadType, aPts, aDtsPtsDiff,
                                     aPayloadCode, aStreamId, aFlags, aType1PayloadSize,
                                     lSuperFrameNo, lOfFragmentNo, lNumType1Fragments,
                                     lNeedsType3, lType3DataSize, lType2DataSize, lBundleSize);
    }

    Result sendFragmentedSingle(const uint8_t* apData, size_t aSize,
                                uint8_t aPayloadType, uint64_t aPts, uint32_t aDtsPtsDiff,
                                uint32_t aPayloadCode, uint8_t aStreamId, uint8_t aFlags,
                                size_t aType1PayloadSize, uint16_t aSuperFrameNo,
                                uint16_t aOfFragmentNo, size_t aNumType1Fragments,
                                bool aNeedsType3, size_t aType3DataSize, size_t aType2DataSize) {

        size_t lDataOffset = 0;

        // Send Type1 fragments
        for (size_t lFragNo = 0; lFragNo < aNumType1Fragments; lFragNo++) {
            FrameType1 lHeader;
            lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE1, aFlags);
            lHeader.mStreamId = aStreamId;
            lHeader.mSuperFrameNo = aSuperFrameNo;
            lHeader.mFragmentNo = (uint16_t)(lFragNo);
            lHeader.mOfFragmentNo = aOfFragmentNo;

            std::memcpy(mSendBuffer.data(), &lHeader, sizeof(lHeader));
            std::memcpy(mSendBuffer.data() + sizeof(lHeader), apData + lDataOffset, aType1PayloadSize);
            lDataOffset += aType1PayloadSize;

            // Retain if enabled
            retainFragment(aSuperFrameNo, (uint16_t)(lFragNo), aStreamId, mSendBuffer.data(), mMtu);

            invokeCallback(mMtu, aStreamId);
        }

        // Send Type3 if needed (penultimate fragment)
        if (aNeedsType3) [[unlikely]] {
            FrameType3 lHeader;
            lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE3, aFlags);
            lHeader.mStreamId = aStreamId;
            lHeader.mSuperFrameNo = aSuperFrameNo;
            lHeader.mType1PacketSize = (uint16_t)(aType1PayloadSize);
            lHeader.mOfFragmentNo = aOfFragmentNo;

            std::memcpy(mSendBuffer.data(), &lHeader, sizeof(lHeader));
            std::memcpy(mSendBuffer.data() + sizeof(lHeader), apData + lDataOffset, aType3DataSize);
            lDataOffset += aType3DataSize;

            auto lTotalSize = sizeof(lHeader) + aType3DataSize;
            retainFragment(aSuperFrameNo, (uint16_t)(aOfFragmentNo - 1), aStreamId,
                           mSendBuffer.data(), lTotalSize);

            invokeCallback(lTotalSize, aStreamId);
        }

        // Send Type2 (final fragment)
        FrameType2 lHeader;
        lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE2, aFlags);
        lHeader.mStreamId = aStreamId;
        lHeader.mPayloadType = aPayloadType;
        lHeader.mSizeOfData = (uint16_t)(aType2DataSize);
        lHeader.mSuperFrameNo = aSuperFrameNo;
        lHeader.mOfFragmentNo = aOfFragmentNo;
        lHeader.mType1PacketSize = (uint16_t)(aType1PayloadSize);
        lHeader.mPts = aPts;
        lHeader.mDtsPtsDiff = aDtsPtsDiff;
        lHeader.mPayloadCode = aPayloadCode;

        std::memcpy(mSendBuffer.data(), &lHeader, sizeof(lHeader));
        std::memcpy(mSendBuffer.data() + sizeof(lHeader), apData + lDataOffset, aType2DataSize);

        auto lTotalSize = sizeof(lHeader) + aType2DataSize;
        retainFragment(aSuperFrameNo, aOfFragmentNo, aStreamId, mSendBuffer.data(), lTotalSize);

        invokeCallback(lTotalSize, aStreamId);

        return Result::OK;
    }

    Result sendFragmentedBundled(const uint8_t* apData, size_t aSize,
                                 uint8_t aPayloadType, uint64_t aPts, uint32_t aDtsPtsDiff,
                                 uint32_t aPayloadCode, uint8_t aStreamId, uint8_t aFlags,
                                 size_t aType1PayloadSize, uint16_t aSuperFrameNo,
                                 uint16_t aOfFragmentNo, size_t aNumType1Fragments,
                                 bool aNeedsType3, size_t aType3DataSize, size_t aType2DataSize,
                                 uint8_t aBundleSize) {

        // Collect all fragments into a temporary list
        std::vector<std::vector<uint8_t>> lFragments;
        lFragments.reserve(aOfFragmentNo + 1);

        size_t lDataOffset = 0;

        // Build Type1 fragments
        for (size_t lFragNo = 0; lFragNo < aNumType1Fragments; lFragNo++) {
            FrameType1 lHeader;
            lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE1, aFlags);
            lHeader.mStreamId = aStreamId;
            lHeader.mSuperFrameNo = aSuperFrameNo;
            lHeader.mFragmentNo = (uint16_t)(lFragNo);
            lHeader.mOfFragmentNo = aOfFragmentNo;

            std::vector<uint8_t> lFrag(mMtu);
            std::memcpy(lFrag.data(), &lHeader, sizeof(lHeader));
            std::memcpy(lFrag.data() + sizeof(lHeader), apData + lDataOffset, aType1PayloadSize);
            lDataOffset += aType1PayloadSize;

            lFragments.push_back(std::move(lFrag));
        }

        // Build Type3 if needed
        if (aNeedsType3) [[unlikely]] {
            FrameType3 lHeader;
            lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE3, aFlags);
            lHeader.mStreamId = aStreamId;
            lHeader.mSuperFrameNo = aSuperFrameNo;
            lHeader.mType1PacketSize = (uint16_t)(aType1PayloadSize);
            lHeader.mOfFragmentNo = aOfFragmentNo;

            std::vector<uint8_t> lFrag(sizeof(lHeader) + aType3DataSize);
            std::memcpy(lFrag.data(), &lHeader, sizeof(lHeader));
            std::memcpy(lFrag.data() + sizeof(lHeader), apData + lDataOffset, aType3DataSize);
            lDataOffset += aType3DataSize;

            lFragments.push_back(std::move(lFrag));
        }

        // Build Type2 (final fragment)
        {
            FrameType2 lHeader;
            lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE2, aFlags);
            lHeader.mStreamId = aStreamId;
            lHeader.mPayloadType = aPayloadType;
            lHeader.mSizeOfData = (uint16_t)(aType2DataSize);
            lHeader.mSuperFrameNo = aSuperFrameNo;
            lHeader.mOfFragmentNo = aOfFragmentNo;
            lHeader.mType1PacketSize = (uint16_t)(aType1PayloadSize);
            lHeader.mPts = aPts;
            lHeader.mDtsPtsDiff = aDtsPtsDiff;
            lHeader.mPayloadCode = aPayloadCode;

            std::vector<uint8_t> lFrag(sizeof(lHeader) + aType2DataSize);
            std::memcpy(lFrag.data(), &lHeader, sizeof(lHeader));
            std::memcpy(lFrag.data() + sizeof(lHeader), apData + lDataOffset, aType2DataSize);

            lFragments.push_back(std::move(lFrag));
        }

        // Now send fragments in bundles
        size_t lFragIdx = 0;
        while (lFragIdx < lFragments.size()) {
            // Build bundle header
            FrameType4 lBundleHeader;
            lBundleHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE4, aFlags);

            auto lFragsInBundle = std::min((size_t)(aBundleSize), lFragments.size() - lFragIdx);

            // Check for retransmit fragments to interleave
            std::vector<uint8_t> lRetransmitData;
            auto lHasRetransmit = false;
            if (!mRetransmitQueue.empty() && lFragsInBundle > 1) {
                lHasRetransmit = getRetransmitFragment(lRetransmitData);
                if (lHasRetransmit) {
                    lFragsInBundle--;  // Make room for retransmit
                }
            }

            lBundleHeader.mFrameCount = (uint8_t)(lFragsInBundle + (lHasRetransmit ? 1 : 0));

            // Build bundle
            size_t lBundleOffset = 0;
            std::memcpy(mBundleBuffer.data(), &lBundleHeader, sizeof(lBundleHeader));
            lBundleOffset += sizeof(lBundleHeader);

            // Add retransmit fragment first if present
            if (lHasRetransmit) {
                std::memcpy(mBundleBuffer.data() + lBundleOffset, lRetransmitData.data(), lRetransmitData.size());
                lBundleOffset += lRetransmitData.size();
            }

            // Add current fragments
            for (size_t lI = 0; lI < lFragsInBundle; lI++) {
                auto& lFrag = lFragments[lFragIdx + lI];

                // Retain fragment
                auto lFragNo = (uint16_t)(lFragIdx + lI);
                // Adjust for Type3 being at ofFragmentNo - 1
                if (aNeedsType3 && lFragNo == aOfFragmentNo) {
                    // This is Type2, already at correct index
                } else if (aNeedsType3 && lFragNo == aOfFragmentNo - 1) {
                    // This is Type3
                }
                retainFragment(aSuperFrameNo, lFragNo, aStreamId, lFrag.data(), lFrag.size());

                std::memcpy(mBundleBuffer.data() + lBundleOffset, lFrag.data(), lFrag.size());
                lBundleOffset += lFrag.size();
                mStatistics.mFragmentsSent++;
            }

            invokeBundleCallback(lBundleOffset, aStreamId);
            lFragIdx += lFragsInBundle;
        }

        return Result::OK;
    }

    uint16_t mMtu;
    uint16_t mSuperFrameNo = 0;
    SubFragmentMode mSubFragmentMode;
    uint32_t mRetentionMs;
    size_t mRetentionMaxBytes;
    size_t mRetentionBufferBytes = 0;
    mutable std::mutex mMutex;
    std::vector<uint8_t> mSendBuffer;
    std::vector<uint8_t> mBundleBuffer;
    std::map<uint32_t, RetainedFragment> mRetentionBuffer;
    std::vector<RetransmitRequest> mRetransmitQueue;
    SenderStatistics mStatistics;
    [[no_unique_address]] SendCallbackT mCallback;
};

// Deduction guide for Sender
template<typename SendCallbackT>
Sender(uint16_t, SendCallbackT, SubFragmentMode = SubFragmentMode::SINGLE,
       uint32_t = 0, size_t = 50 * 1024 * 1024) -> Sender<SendCallbackT>;

// Factory function for easier instantiation
template<typename SendCallbackT, uint16_t BUFFER_SIZE = DEFAULT_BUFFER_SIZE>
    requires SendCallbackConcept<SendCallbackT>
[[nodiscard]] auto makeSender(uint16_t aMtu, SendCallbackT aCallback,
                               SubFragmentMode aSubFragmentMode = SubFragmentMode::SINGLE,
                               uint32_t aRetentionMs = 0,
                               size_t aRetentionMaxBytes = 50 * 1024 * 1024) {
    return Sender<SendCallbackT, BUFFER_SIZE>(aMtu, std::move(aCallback),
                                               aSubFragmentMode, aRetentionMs, aRetentionMaxBytes);
}

//------------------------------------------------------------------------------
// Receiver: Reassembles EFP fragments into SuperFrames
// Template callback for zero-overhead invocation (no std::function)
//------------------------------------------------------------------------------
template<typename ReceiveCallbackT, typename NackCallbackT, uint16_t BUFFER_SIZE = DEFAULT_BUFFER_SIZE>
    requires ValidBufferSize<BUFFER_SIZE> && ReceiveCallbackConcept<ReceiveCallbackT> && NackCallbackConcept<NackCallbackT>
class Receiver {
public:
    explicit Receiver(ReceiveCallbackT aCallback, NackCallbackT aNackCallback,
                      uint32_t aTimeoutMs = 100,
                      uint32_t aHolTimeoutMs = 0,
                      uint8_t aMaxNackRetries = 3,
                      uint32_t aNackIntervalMs = 0,
                      ReceiverMode aMode = ReceiverMode::THREADED)
        : mCallback(std::move(aCallback)), mNackCallback(std::move(aNackCallback)),
          mTimeoutMs(aTimeoutMs), mHolTimeoutMs(aHolTimeoutMs),
          mMaxNackRetries(aMaxNackRetries), mNackIntervalMs(aNackIntervalMs), mMode(aMode) {

        // Validate timing configuration
        if (mHolTimeoutMs > 0 && mHolTimeoutMs >= mTimeoutMs) {
            throw std::invalid_argument("HOL timeout must be less than frame timeout");
        }
        if (mMaxNackRetries > 0 && mNackIntervalMs > 0) {
            auto lNackBudgetMs = mNackIntervalMs * mMaxNackRetries;
            if (lNackBudgetMs >= mTimeoutMs) {
                throw std::invalid_argument("NACK budget (interval * retries) must be less than frame timeout");
            }
        }

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
            case FrameType::TYPE4:
                lResult = handleType4(aData.data(), aData.size(), aSourceId);
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

    // Get current statistics
    [[nodiscard]] ReceiverStatistics getStatistics() const {
        std::lock_guard<std::recursive_mutex> lLock(mNetMutex);
        auto lStats = mStatistics;
        lStats.mPendingBuckets = (uint32_t)(mBucketMap.size());
        return lStats;
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
        int64_t  mFirstArrivalUs = 0;  // When first fragment arrived (for NACK timing)
        int64_t  mLastNackUs     = 0;  // When last NACK was sent (0 = never)
        uint8_t  mNackCount      = 0;  // Number of NACKs sent for this bucket
        std::bitset<8192> mReceivedFragments;  // Max fragments per superframe
        SuperFramePtr mpFrame;
    };

    [[nodiscard]] int64_t nowUs() const noexcept {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Update jitter estimate using RFC 3550 algorithm (exponential moving average)
    void updateJitter(int64_t aArrivalUs) noexcept {
        if (mLastArrivalUs > 0) [[likely]] {
            auto lInterArrival = aArrivalUs - mLastArrivalUs;
            auto lDeviation = lInterArrival - mExpectedIntervalUs;
            if (lDeviation < 0) lDeviation = -lDeviation;
            // J(i) = J(i-1) + (|D(i)| - J(i-1)) / 16
            mJitterUs += (lDeviation - mJitterUs) / 16;
            // Update expected interval as moving average
            mExpectedIntervalUs += (lInterArrival - mExpectedIntervalUs) / 16;
        }
        mLastArrivalUs = aArrivalUs;
    }

    // Calculate delay before sending NACK (adaptive or manual)
    [[nodiscard]] int64_t calculateNackDelayUs(const Bucket* apBucket) const noexcept {
        int64_t lBaseDelayUs;
        if (mNackIntervalMs > 0) {
            // Manual override: use fixed interval
            lBaseDelayUs = (int64_t)(mNackIntervalMs) * 1000;
        } else {
            // Adaptive: use 4x jitter estimate, minimum 10ms
            lBaseDelayUs = std::max((int64_t)(10000), mJitterUs * 4);
        }

        // Apply exponential backoff based on retry count
        auto lBackoffDelayUs = lBaseDelayUs << apBucket->mNackCount;

        // Cap at remaining time to timeout (leave 10ms margin for delivery)
        auto lRemainingUs = apBucket->mTimeoutUs - nowUs() - 10000;
        if (lRemainingUs < 0) lRemainingUs = 0;

        return std::min(lBackoffDelayUs, lRemainingUs);
    }

    // Build NACK message for missing fragments in a bucket
    [[nodiscard]] std::vector<uint8_t> buildNack(const Bucket* apBucket) const {
        std::vector<NackEntry> lEntries;
        lEntries.reserve(16);

        uint16_t lRangeStart = UINT16_MAX;
        uint16_t lRangeCount = 0;

        // Scan for missing fragments and coalesce consecutive gaps
        for (uint16_t lI = 0; lI <= apBucket->mOfFragmentNo; lI++) {
            if (!apBucket->mReceivedFragments[lI]) {
                // Fragment is missing
                if (lRangeStart == UINT16_MAX) {
                    // Start new range
                    lRangeStart = lI;
                    lRangeCount = 0;
                } else if (lRangeCount < 255) {
                    // Extend current range (max 256 consecutive in one entry)
                    lRangeCount++;
                } else {
                    // Range full, emit and start new
                    NackEntry lEntry;
                    lEntry.mStreamId = apBucket->mStreamId;
                    lEntry.mSuperFrameNo = apBucket->mSavedFrameNo;
                    lEntry.mFragmentNo = lRangeStart;
                    lEntry.mFragmentCount = lRangeCount;
                    lEntries.push_back(lEntry);
                    lRangeStart = lI;
                    lRangeCount = 0;
                }
            } else if (lRangeStart != UINT16_MAX) {
                // Fragment received, emit pending range
                NackEntry lEntry;
                lEntry.mStreamId = apBucket->mStreamId;
                lEntry.mSuperFrameNo = apBucket->mSavedFrameNo;
                lEntry.mFragmentNo = lRangeStart;
                lEntry.mFragmentCount = lRangeCount;
                lEntries.push_back(lEntry);
                lRangeStart = UINT16_MAX;
                lRangeCount = 0;
            }
        }

        // Emit final pending range if any
        if (lRangeStart != UINT16_MAX) {
            NackEntry lEntry;
            lEntry.mStreamId = apBucket->mStreamId;
            lEntry.mSuperFrameNo = apBucket->mSavedFrameNo;
            lEntry.mFragmentNo = lRangeStart;
            lEntry.mFragmentCount = lRangeCount;
            lEntries.push_back(lEntry);
        }

        if (lEntries.empty()) {
            return {};
        }

        // Limit entries per NACK (mNackCount field is uint8_t, max 255)
        if (lEntries.size() > 255) {
            lEntries.resize(255);
        }

        // Build NACK packet
        std::vector<uint8_t> lNackData(sizeof(FrameType0Nack) + lEntries.size() * sizeof(NackEntry));

        FrameType0Nack lHeader;
        lHeader.mFrameType = makeFrameTypeByte(FrameType::TYPE0, 0);
        lHeader.mSubtype = (uint8_t)(Type0Subtype::NACK);
        lHeader.mNackCount = (uint8_t)(lEntries.size());

        std::memcpy(lNackData.data(), &lHeader, sizeof(lHeader));
        std::memcpy(lNackData.data() + sizeof(lHeader), lEntries.data(),
                    lEntries.size() * sizeof(NackEntry));

        return lNackData;
    }

    // Process NACKs for all incomplete buckets
    void processNacks() {
        if (mMaxNackRetries == 0) return;  // NACKs disabled

        auto lNow = nowUs();

        for (auto& [lOrder, lpBucket] : mBucketMap) {
            // Skip complete buckets
            if (lpBucket->mFragmentCount == lpBucket->mOfFragmentNo + 1) continue;

            // Skip if max retries exceeded
            if (lpBucket->mNackCount >= mMaxNackRetries) continue;

            auto lDelayUs = calculateNackDelayUs(lpBucket);
            int64_t lTriggerTimeUs;

            if (lpBucket->mLastNackUs == 0) {
                // First NACK: trigger after grace period from first arrival
                lTriggerTimeUs = lpBucket->mFirstArrivalUs + lDelayUs;
            } else {
                // Retry NACK: trigger after delay from last NACK
                lTriggerTimeUs = lpBucket->mLastNackUs + lDelayUs;
            }

            if (lNow < lTriggerTimeUs) continue;

            // Build and send NACK
            auto lNackData = buildNack(lpBucket);
            if (lNackData.empty()) continue;

            mNackCallback(std::span<const uint8_t>(lNackData));

            lpBucket->mLastNackUs = lNow;
            lpBucket->mNackCount++;
            mStatistics.mNacksSent++;
        }
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

        auto lNow = nowUs();
        updateJitter(lNow);

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
            lpBucket->mTimeoutUs = lNow + (mTimeoutMs * 1000);
            lpBucket->mFragmentCount = 1;  // First fragment received
            lpBucket->mOfFragmentNo = lpHeader->mOfFragmentNo;
            lpBucket->mFragmentSize = lPayloadSize;
            lpBucket->mPts = UINT64_MAX;
            lpBucket->mDts = UINT64_MAX;
            lpBucket->mFirstArrivalUs = lNow;  // Track first fragment arrival for NACK timing
            lpBucket->mLastNackUs = 0;
            lpBucket->mNackCount = 0;
            mStatistics.mFragmentsReceived++;

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
            mStatistics.mDuplicateFragments++;
            return Result::DUPLICATE_FRAGMENT;
        }

        lpBucket->mReceivedFragments[lpHeader->mFragmentNo] = true;
        lpBucket->mFragmentCount++;
        mStatistics.mFragmentsReceived++;

        auto lOffset = lpBucket->mFragmentSize * lpHeader->mFragmentNo;
        std::memcpy(lpBucket->mpFrame->mpData + lOffset, apData + sizeof(FrameType1), lPayloadSize);

        return Result::OK;
    }

    Result handleType2(const uint8_t* apData, size_t aSize, uint8_t aSourceId) {
        if (aSize < sizeof(FrameType2)) [[unlikely]] {
            return Result::FRAME_SIZE_MISMATCH;
        }

        std::lock_guard<std::recursive_mutex> lLock(mNetMutex);

        auto lNow = nowUs();
        updateJitter(lNow);

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
            lpBucket->mTimeoutUs = lNow + (mTimeoutMs * 1000);
            lpBucket->mFragmentCount = 1;  // First fragment received
            lpBucket->mOfFragmentNo = lpHeader->mOfFragmentNo;
            lpBucket->mFragmentSize = lpHeader->mType1PacketSize;
            lpBucket->mType2Size = lpHeader->mSizeOfData;  // Store for potential relocation
            lpBucket->mPts = lpHeader->mPts;
            lpBucket->mPayloadType = lpHeader->mPayloadType;
            lpBucket->mPayloadCode = lpHeader->mPayloadCode;
            lpBucket->mFirstArrivalUs = lNow;  // Track first fragment arrival for NACK timing
            lpBucket->mLastNackUs = 0;
            lpBucket->mNackCount = 0;
            mStatistics.mFragmentsReceived++;

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
            mStatistics.mDuplicateFragments++;
            return Result::DUPLICATE_FRAGMENT;
        }

        lpBucket->mReceivedFragments[lpHeader->mOfFragmentNo] = true;
        lpBucket->mFragmentCount++;
        mStatistics.mFragmentsReceived++;
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

        auto lNow = nowUs();
        updateJitter(lNow);

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
            lpBucket->mTimeoutUs = lNow + (mTimeoutMs * 1000);
            lpBucket->mFragmentCount = 1;  // First fragment received
            lpBucket->mOfFragmentNo = lpHeader->mOfFragmentNo;
            lpBucket->mFragmentSize = lpHeader->mType1PacketSize;
            lpBucket->mType3Size = lPayloadSize;  // Store Type3 payload size
            lpBucket->mPts = UINT64_MAX;
            lpBucket->mDts = UINT64_MAX;
            lpBucket->mFirstArrivalUs = lNow;  // Track first fragment arrival for NACK timing
            lpBucket->mLastNackUs = 0;
            lpBucket->mNackCount = 0;
            mStatistics.mFragmentsReceived++;

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
            mStatistics.mDuplicateFragments++;
            return Result::DUPLICATE_FRAGMENT;
        }

        lpBucket->mReceivedFragments[lFragmentNo] = true;
        lpBucket->mFragmentCount++;
        mStatistics.mFragmentsReceived++;
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

    Result handleType4(const uint8_t* apData, size_t aSize, uint8_t aSourceId) {
        if (aSize < sizeof(FrameType4)) [[unlikely]] {
            return Result::FRAME_SIZE_MISMATCH;
        }

        auto* lpHeader = (const FrameType4*)(apData);

        if (lpHeader->mFrameCount == 0) [[unlikely]] {
            return Result::INVALID_PARAMETER;
        }

        mStatistics.mBundlesReceived++;

        // Process each bundled frame
        size_t lOffset = sizeof(FrameType4);
        Result lLastResult = Result::OK;

        for (uint8_t lI = 0; lI < lpHeader->mFrameCount; lI++) {
            if (lOffset >= aSize) [[unlikely]] {
                return Result::FRAME_SIZE_MISMATCH;
            }

            auto lFrameType = getFrameType(apData[lOffset]);
            size_t lFrameSize = 0;

            // Determine frame size based on type
            switch (lFrameType) {
                case FrameType::TYPE1: {
                    if (lOffset + sizeof(FrameType1) > aSize) [[unlikely]] {
                        return Result::FRAME_SIZE_MISMATCH;
                    }
                    auto* lpType1 = (const FrameType1*)(apData + lOffset);
                    // Type1 frames in bundles are typically full MTU minus header
                    // We need to calculate based on the remaining data or use a heuristic
                    // For now, assume uniform fragment sizes within bundle
                    // The payload size would be (total size - all headers) / frame count
                    // But actually, we need to peek at the next frame to know where this one ends
                    // For Type1, the payload goes until the next frame header or end of bundle

                    // Calculate remaining space for this and subsequent frames
                    auto lRemainingSize = aSize - lOffset;
                    auto lRemainingFrames = lpHeader->mFrameCount - lI;

                    // Try to find the next Type1/Type2/Type3 header
                    lFrameSize = sizeof(FrameType1);
                    auto lNextOffset = lOffset + sizeof(FrameType1);

                    // Scan for next valid frame type byte or use remaining divided equally
                    if (lRemainingFrames > 1) {
                        // Calculate approximate fragment size
                        auto lApproxFragSize = lRemainingSize / lRemainingFrames;
                        lFrameSize = lApproxFragSize;
                    } else {
                        // Last frame takes all remaining
                        lFrameSize = lRemainingSize;
                    }

                    lLastResult = handleType1(apData + lOffset, lFrameSize, aSourceId);
                    break;
                }
                case FrameType::TYPE2: {
                    if (lOffset + sizeof(FrameType2) > aSize) [[unlikely]] {
                        return Result::FRAME_SIZE_MISMATCH;
                    }
                    auto* lpType2 = (const FrameType2*)(apData + lOffset);
                    lFrameSize = sizeof(FrameType2) + lpType2->mSizeOfData;
                    if (lOffset + lFrameSize > aSize) [[unlikely]] {
                        return Result::FRAME_SIZE_MISMATCH;
                    }
                    lLastResult = handleType2(apData + lOffset, lFrameSize, aSourceId);
                    break;
                }
                case FrameType::TYPE3: {
                    if (lOffset + sizeof(FrameType3) > aSize) [[unlikely]] {
                        return Result::FRAME_SIZE_MISMATCH;
                    }
                    // Type3 size is remaining after header, similar to Type1
                    auto lRemainingSize = aSize - lOffset;
                    auto lRemainingFrames = lpHeader->mFrameCount - lI;
                    if (lRemainingFrames > 1) {
                        lFrameSize = lRemainingSize / lRemainingFrames;
                    } else {
                        lFrameSize = lRemainingSize;
                    }
                    lLastResult = handleType3(apData + lOffset, lFrameSize, aSourceId);
                    break;
                }
                default:
                    return Result::INVALID_PARAMETER;
            }

            if (lLastResult != Result::OK && lLastResult != Result::DUPLICATE_FRAGMENT) [[unlikely]] {
                return lLastResult;
            }

            lOffset += lFrameSize;
        }

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

        // Update statistics
        if (apBucket->mpFrame->mBroken) {
            mStatistics.mBrokenFrames++;
        } else {
            mStatistics.mCompleteFrames++;
        }

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

        // First, process NACKs for incomplete buckets
        processNacks();

        std::vector<Bucket*> lToDeliver;
        lToDeliver.reserve(16);  // Preallocate for typical case

        // First pass: identify complete, timed-out, and HOL-timed-out buckets
        for (auto& [lOrder, lpBucket] : mBucketMap) {
            auto lComplete = (lpBucket->mFragmentCount == lpBucket->mOfFragmentNo + 1);
            auto lTimedOut = (lpBucket->mTimeoutUs <= lNow);

            if (lComplete || lTimedOut) {
                lToDeliver.push_back(lpBucket);
            } else if (mHolTimeoutMs > 0) {
                // HOL timeout: deliver incomplete frames that have been waiting too long
                auto lHolTimeoutUs = (int64_t)(mHolTimeoutMs) * 1000;
                auto lElapsedUs = lNow - lpBucket->mFirstArrivalUs;
                if (lElapsedUs >= lHolTimeoutUs) {
                    lToDeliver.push_back(lpBucket);
                }
            }
        }

        // Sort by delivery order to maintain sequence
        std::sort(lToDeliver.begin(), lToDeliver.end(),
                  [](const Bucket* apA, const Bucket* apB) {
                      return apA->mDeliveryOrder < apB->mDeliveryOrder;
                  });

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
    [[no_unique_address]] NackCallbackT mNackCallback;
    uint32_t mTimeoutMs;
    uint32_t mHolTimeoutMs;
    uint8_t mMaxNackRetries;
    uint32_t mNackIntervalMs;
    ReceiverMode mMode;

    Bucket* mpBuckets;
    std::map<uint64_t, Bucket*> mBucketMap;
    Stream mStreams[256];  // All 256 stream IDs (0-255)
    mutable std::recursive_mutex mNetMutex;

    uint16_t mOldFrameNo = 0;
    uint64_t mFrameNoRecalc = 0;
    bool mFirstFrame = true;

    // Jitter tracking for adaptive NACK timing
    int64_t mJitterUs = 10000;      // Smoothed jitter estimate (start at 10ms)
    int64_t mLastArrivalUs = 0;     // Previous fragment arrival time
    int64_t mExpectedIntervalUs = 0; // Expected inter-arrival time

    std::atomic<bool> mRunning{false};
    std::jthread mWorkerThread;
    std::jthread mDeliveryThread;

    std::mutex mDeliveryMutex;
    std::deque<SuperFramePtr> mDeliveryQueue;
    std::condition_variable mDeliveryCondition;
    bool mDeliveryReady = false;

    ReceiverStatistics mStatistics;
};

// Deduction guide for Receiver
template<typename ReceiveCallbackT, typename NackCallbackT>
Receiver(ReceiveCallbackT, NackCallbackT, uint32_t = 100, uint32_t = 0, uint8_t = 3, uint32_t = 0, ReceiverMode = ReceiverMode::THREADED)
    -> Receiver<ReceiveCallbackT, NackCallbackT>;

// Factory function for easier instantiation
template<typename ReceiveCallbackT, typename NackCallbackT, uint16_t BUFFER_SIZE = DEFAULT_BUFFER_SIZE>
    requires ReceiveCallbackConcept<ReceiveCallbackT> && NackCallbackConcept<NackCallbackT>
[[nodiscard]] auto makeReceiver(ReceiveCallbackT aCallback, NackCallbackT aNackCallback,
                                 uint32_t aTimeoutMs = 100,
                                 uint32_t aHolTimeoutMs = 0,
                                 uint8_t aMaxNackRetries = 3,
                                 uint32_t aNackIntervalMs = 0,
                                 ReceiverMode aMode = ReceiverMode::THREADED) {
    return Receiver<ReceiveCallbackT, NackCallbackT, BUFFER_SIZE>(
        std::move(aCallback), std::move(aNackCallback),
        aTimeoutMs, aHolTimeoutMs, aMaxNackRetries, aNackIntervalMs, aMode);
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

