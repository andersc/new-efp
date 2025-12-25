//
// Elastic Frame Protocol - Main Header
// Copyright 2024-2025
//
// A lightweight, generic data framing protocol for fragmenting and
// reassembling data over unreliable or size-limited transport layers.
//

#ifndef EFP_H
#define EFP_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <functional>
#include <vector>
#include <map>
#include <bitset>
#include <mutex>
#include <atomic>
#include <thread>
#include <deque>
#include <condition_variable>
#include <chrono>

#include "efp_internal.h"

namespace efp {

// Version info
constexpr uint8_t  VERSION_MAJOR = 1;
constexpr uint8_t  VERSION_MINOR = 0;
constexpr uint16_t VERSION = (static_cast<uint16_t>(VERSION_MAJOR) << 8) | VERSION_MINOR;

// Default circular buffer size (must be 2^n - 1 for bitmask operations)
constexpr uint16_t DEFAULT_BUFFER_SIZE = 8191;

// Result codes
enum class Result : int16_t {
    // Errors (negative)
    MemoryAllocationError    = -10,
    BufferOutOfBounds        = -9,
    BufferOutOfResources     = -8,
    FrameSizeMismatch        = -7,
    TooLargeFrame            = -6,
    TooLargeEmbeddedData     = -5,
    InvalidParameter         = -4,
    ReceiverNotRunning       = -3,
    InternalError            = -2,
    NotImplemented           = -1,

    // Success
    Ok                       = 0,

    // Informational (positive)
    DuplicateFragment        = 1,
    FragmentTooOld           = 2,
    FrameTimeout             = 3,
};

// Receiver operating modes
enum class ReceiverMode : uint8_t {
    Threaded        = 1,  // Background threads handle assembly and delivery
    RunToCompletion = 2   // Caller-driven, no internal threads
};

//------------------------------------------------------------------------------
// SuperFrame: Assembled data frame delivered by receiver
//------------------------------------------------------------------------------
class SuperFrame {
public:
    uint8_t* data         = nullptr;       // Pointer to frame data (32-byte aligned)
    size_t   size         = 0;             // Frame size in bytes
    uint8_t  payloadType  = 0;             // User-defined payload type
    uint32_t payloadCode  = UINT32_MAX;    // User-defined payload code
    uint64_t pts          = UINT64_MAX;    // Presentation timestamp
    uint64_t dts          = UINT64_MAX;    // Decode timestamp
    uint8_t  streamId     = 0;             // Stream identifier
    uint8_t  sourceId     = 0;             // Source identifier (passed through)
    uint8_t  flags        = 0;             // Flags from the frame
    uint16_t superFrameNo = 0;             // Sequence number
    bool     broken       = true;          // True if frame is incomplete

    SuperFrame() = default;
    SuperFrame(const SuperFrame&) = delete;
    SuperFrame& operator=(const SuperFrame&) = delete;

    explicit SuperFrame(size_t allocSize) {
        if (allocSize > 0) {
#ifdef _WIN64
            data = static_cast<uint8_t*>(_aligned_malloc(allocSize, 32));
#else
            if (posix_memalign(reinterpret_cast<void**>(&data), 32, allocSize) != 0) {
                data = nullptr;
            }
#endif
            if (data) {
                size = allocSize;
            }
        }
    }

    ~SuperFrame() {
        if (data) {
#ifdef _WIN64
            _aligned_free(data);
#else
            free(data);
#endif
        }
    }
};

using SuperFramePtr = std::unique_ptr<SuperFrame>;

//------------------------------------------------------------------------------
// SendCallback: Called for each fragment to be transmitted
//------------------------------------------------------------------------------
using SendCallback = std::function<void(const uint8_t* data, size_t size, uint8_t streamId)>;

//------------------------------------------------------------------------------
// ReceiveCallback: Called when a SuperFrame is assembled (or times out)
//------------------------------------------------------------------------------
using ReceiveCallback = std::function<void(SuperFramePtr frame)>;

//------------------------------------------------------------------------------
// Sender: Fragments data into EFP packets
//------------------------------------------------------------------------------
template<uint16_t BufferSize = DEFAULT_BUFFER_SIZE>
class Sender {
    static_assert((BufferSize & (BufferSize + 1)) == 0,
                  "BufferSize must be 2^n - 1 for bitmask operations");

public:
    explicit Sender(uint16_t mtu) : mtu_(mtu) {
        if (mtu_ < 256) mtu_ = 256;
        sendBuffer_.resize(mtu_);
    }

    ~Sender() = default;

    // Non-copyable, non-movable
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    Sender(Sender&&) = delete;
    Sender& operator=(Sender&&) = delete;

    // Get version
    static uint16_t version() { return VERSION; }

    // Set send callback
    void setCallback(SendCallback callback) {
        callback_ = std::move(callback);
    }

    // Pack and send data
    Result send(const uint8_t* data, size_t size,
                uint8_t payloadType, uint64_t pts, uint64_t dts,
                uint32_t payloadCode, uint8_t streamId, uint8_t flags = Flags::None) {

        if (!data || size == 0) {
            return Result::InvalidParameter;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        const size_t type1PayloadSize = mtu_ - sizeof(FrameType1);
        const size_t type2PayloadSize = mtu_ - sizeof(FrameType2);

        // Calculate DTS-PTS difference
        uint32_t dtsPtsDiff = UINT32_MAX;
        if (dts != UINT64_MAX && pts != UINT64_MAX && pts >= dts) {
            uint64_t diff = pts - dts;
            if (diff <= UINT32_MAX - 1) {
                dtsPtsDiff = static_cast<uint32_t>(diff);
            }
        }

        // Single small frame? Use Type2 only
        if (size <= type2PayloadSize) {
            return sendType2Only(data, size, payloadType, pts, dtsPtsDiff,
                                 payloadCode, streamId, flags);
        }

        // Multiple fragments needed
        return sendFragmented(data, size, payloadType, pts, dtsPtsDiff,
                              payloadCode, streamId, flags, type1PayloadSize);
    }

    // Convenience overload for vector
    Result send(const std::vector<uint8_t>& data,
                uint8_t payloadType, uint64_t pts, uint64_t dts,
                uint32_t payloadCode, uint8_t streamId, uint8_t flags = Flags::None) {
        return send(data.data(), data.size(), payloadType, pts, dts,
                    payloadCode, streamId, flags);
    }

private:
    Result sendType2Only(const uint8_t* data, size_t size,
                         uint8_t payloadType, uint64_t pts, uint32_t dtsPtsDiff,
                         uint32_t payloadCode, uint8_t streamId, uint8_t flags) {

        FrameType2 header;
        header.frameType = makeFrameTypeByte(FrameType::Type2, flags);
        header.streamId = streamId;
        header.payloadType = payloadType;
        header.sizeOfData = static_cast<uint16_t>(size);
        header.superFrameNo = superFrameNo_++;
        header.ofFragmentNo = 0;
        header.type1PacketSize = 0;
        header.pts = pts;
        header.dtsPtsDiff = dtsPtsDiff;
        header.payloadCode = payloadCode;

        std::memcpy(sendBuffer_.data(), &header, sizeof(header));
        std::memcpy(sendBuffer_.data() + sizeof(header), data, size);

        if (callback_) {
            callback_(sendBuffer_.data(), sizeof(header) + size, streamId);
        }

        return Result::Ok;
    }

    Result sendFragmented(const uint8_t* data, size_t size,
                          uint8_t payloadType, uint64_t pts, uint32_t dtsPtsDiff,
                          uint32_t payloadCode, uint8_t streamId, uint8_t flags,
                          size_t type1PayloadSize) {

        const size_t type2HeaderSize = sizeof(FrameType2);
        const size_t type3HeaderSize = sizeof(FrameType3);

        // Calculate fragment count
        // Last fragment uses Type2, may need Type3 for penultimate
        size_t remainingAfterType1s = size % type1PayloadSize;
        size_t numType1Fragments = size / type1PayloadSize;

        uint16_t totalFragments;
        bool needsType3 = false;
        size_t type2DataSize;
        size_t type3DataSize = 0;

        if (remainingAfterType1s == 0) {
            // Perfect fit into Type1s, last one becomes Type2 with full payload
            totalFragments = static_cast<uint16_t>(numType1Fragments);
            type2DataSize = type1PayloadSize;
            numType1Fragments--;
        } else if (remainingAfterType1s <= (mtu_ - type2HeaderSize)) {
            // Remainder fits in Type2
            totalFragments = static_cast<uint16_t>(numType1Fragments + 1);
            type2DataSize = remainingAfterType1s;
        } else {
            // Need Type3 for overflow
            needsType3 = true;
            totalFragments = static_cast<uint16_t>(numType1Fragments + 2);
            type3DataSize = type1PayloadSize;  // Type3 carries full fragment
            type2DataSize = remainingAfterType1s - type1PayloadSize + (mtu_ - type2HeaderSize);
            // Recalculate: remaining split between Type3 and Type2
            size_t combinedSpace = type1PayloadSize + (mtu_ - type2HeaderSize);
            if (remainingAfterType1s <= combinedSpace) {
                type3DataSize = remainingAfterType1s - (mtu_ - type2HeaderSize);
                if (type3DataSize > type1PayloadSize) {
                    type3DataSize = type1PayloadSize;
                }
                type2DataSize = remainingAfterType1s - type3DataSize;
            }
        }

        uint16_t superFrameNo = superFrameNo_++;
        uint16_t ofFragmentNo = totalFragments - 1;
        size_t dataOffset = 0;

        // Send Type1 fragments
        for (uint16_t fragNo = 0; fragNo < numType1Fragments; fragNo++) {
            FrameType1 header;
            header.frameType = makeFrameTypeByte(FrameType::Type1, flags);
            header.streamId = streamId;
            header.superFrameNo = superFrameNo;
            header.fragmentNo = fragNo;
            header.ofFragmentNo = ofFragmentNo;

            std::memcpy(sendBuffer_.data(), &header, sizeof(header));
            std::memcpy(sendBuffer_.data() + sizeof(header), data + dataOffset, type1PayloadSize);
            dataOffset += type1PayloadSize;

            if (callback_) {
                callback_(sendBuffer_.data(), mtu_, streamId);
            }
        }

        // Send Type3 if needed (penultimate fragment)
        if (needsType3) {
            FrameType3 header;
            header.frameType = makeFrameTypeByte(FrameType::Type3, flags);
            header.streamId = streamId;
            header.superFrameNo = superFrameNo;
            header.type1PacketSize = static_cast<uint16_t>(type1PayloadSize);
            header.ofFragmentNo = ofFragmentNo;

            std::memcpy(sendBuffer_.data(), &header, sizeof(header));
            std::memcpy(sendBuffer_.data() + sizeof(header), data + dataOffset, type3DataSize);
            dataOffset += type3DataSize;

            if (callback_) {
                callback_(sendBuffer_.data(), sizeof(header) + type3DataSize, streamId);
            }
        }

        // Send Type2 (final fragment)
        FrameType2 header;
        header.frameType = makeFrameTypeByte(FrameType::Type2, flags);
        header.streamId = streamId;
        header.payloadType = payloadType;
        header.sizeOfData = static_cast<uint16_t>(type2DataSize);
        header.superFrameNo = superFrameNo;
        header.ofFragmentNo = ofFragmentNo;
        header.type1PacketSize = static_cast<uint16_t>(type1PayloadSize);
        header.pts = pts;
        header.dtsPtsDiff = dtsPtsDiff;
        header.payloadCode = payloadCode;

        std::memcpy(sendBuffer_.data(), &header, sizeof(header));
        std::memcpy(sendBuffer_.data() + sizeof(header), data + dataOffset, type2DataSize);

        if (callback_) {
            callback_(sendBuffer_.data(), sizeof(header) + type2DataSize, streamId);
        }

        return Result::Ok;
    }

    uint16_t mtu_;
    uint16_t superFrameNo_ = 0;
    std::mutex mutex_;
    std::vector<uint8_t> sendBuffer_;
    SendCallback callback_;
};

//------------------------------------------------------------------------------
// Receiver: Reassembles EFP fragments into SuperFrames
//------------------------------------------------------------------------------
template<uint16_t BufferSize = DEFAULT_BUFFER_SIZE>
class Receiver {
    static_assert((BufferSize & (BufferSize + 1)) == 0,
                  "BufferSize must be 2^n - 1 for bitmask operations");

public:
    explicit Receiver(uint32_t timeoutMs = 100, uint32_t holTimeoutMs = 0,
                      ReceiverMode mode = ReceiverMode::Threaded)
        : timeoutMs_(timeoutMs), holTimeoutMs_(holTimeoutMs), mode_(mode) {

        buckets_ = new Bucket[BufferSize + 1];

        if (mode_ == ReceiverMode::Threaded) {
            running_ = true;
            workerThread_ = std::thread(&Receiver::workerLoop, this);
            deliveryThread_ = std::thread(&Receiver::deliveryLoop, this);
        }
    }

    ~Receiver() {
        stop();
        delete[] buckets_;
    }

    // Non-copyable, non-movable
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    Receiver(Receiver&&) = delete;
    Receiver& operator=(Receiver&&) = delete;

    // Get version
    static uint16_t version() { return VERSION; }

    // Set receive callback
    void setCallback(ReceiveCallback callback) {
        callback_ = std::move(callback);
    }

    // Receive a fragment
    Result receive(const uint8_t* data, size_t size, uint8_t sourceId = 0) {
        if (!data || size == 0) {
            return Result::InvalidParameter;
        }

        FrameType type = getFrameType(data[0]);
        Result result;

        switch (type) {
            case FrameType::Type0:
                result = handleType0(data, size, sourceId);
                break;
            case FrameType::Type1:
                result = handleType1(data, size, sourceId);
                break;
            case FrameType::Type2:
                result = handleType2(data, size, sourceId);
                break;
            case FrameType::Type3:
                result = handleType3(data, size, sourceId);
                break;
            default:
                return Result::InvalidParameter;
        }

        // In RunToCompletion mode, automatically process completed frames
        if (mode_ == ReceiverMode::RunToCompletion) {
            std::lock_guard<std::recursive_mutex> lock(netMutex_);
            processTimeouts();
        }

        return result;
    }

    // Convenience overload for vector
    Result receive(const std::vector<uint8_t>& data, uint8_t sourceId = 0) {
        return receive(data.data(), data.size(), sourceId);
    }

    // For RunToCompletion mode: process timeouts and deliver frames
    void poll() {
        if (mode_ != ReceiverMode::RunToCompletion) return;

        std::lock_guard<std::recursive_mutex> lock(netMutex_);
        processTimeouts();
    }

    // Stop receiver threads
    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;  // Already stopped or not started
        }

        deliveryCondition_.notify_all();

        if (workerThread_.joinable()) {
            workerThread_.join();
        }
        if (deliveryThread_.joinable()) {
            deliveryThread_.join();
        }
    }

private:
    struct Stream {
        uint8_t  payloadType = 0;
        uint32_t payloadCode = UINT32_MAX;
    };

    struct Bucket {
        bool     active         = false;
        uint8_t  payloadType    = 0;
        uint32_t payloadCode    = UINT32_MAX;
        uint16_t savedFrameNo   = 0;
        int64_t  timeoutUs      = 0;
        uint16_t fragmentCount  = 0;
        uint16_t ofFragmentNo   = 0;
        uint64_t deliveryOrder  = UINT64_MAX;
        size_t   fragmentSize   = 0;
        size_t   type3Size      = 0;  // Size of Type3 payload (0 if no Type3)
        size_t   type2Size      = 0;  // Size of Type2 payload (for relocation if Type3 arrives after)
        uint64_t pts            = UINT64_MAX;
        uint64_t dts            = UINT64_MAX;
        uint8_t  streamId       = 0;
        uint8_t  sourceId       = 0;
        uint8_t  flags          = 0;
        std::bitset<8192> receivedFragments;  // Max fragments per superframe
        SuperFramePtr frame;
    };

    int64_t nowUs() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    uint64_t recalculateSuperFrameNo(uint16_t frameNo) {
        if (firstFrame_) {
            oldFrameNo_ = frameNo;
            frameNoRecalc_ = frameNo;
            firstFrame_ = false;
            return frameNoRecalc_;
        }
        int16_t delta = static_cast<int16_t>(frameNo) - static_cast<int16_t>(oldFrameNo_);
        oldFrameNo_ = frameNo;
        frameNoRecalc_ += delta;
        return frameNoRecalc_;
    }

    Result handleType0(const uint8_t* /*data*/, size_t /*size*/, uint8_t /*sourceId*/) {
        // Type0 signaling - pass through or handle separately
        return Result::Ok;
    }

    Result handleType1(const uint8_t* data, size_t size, uint8_t sourceId) {
        if (size < sizeof(FrameType1)) {
            return Result::FrameSizeMismatch;
        }

        std::lock_guard<std::recursive_mutex> lock(netMutex_);

        auto* header = reinterpret_cast<const FrameType1*>(data);

        // Bounds check: fragmentNo and ofFragmentNo must fit in bitset (8192 max)
        if (header->fragmentNo >= 8192 || header->ofFragmentNo >= 8192) {
            return Result::BufferOutOfBounds;
        }

        // Check fragmentNo doesn't exceed ofFragmentNo
        if (header->fragmentNo > header->ofFragmentNo) {
            return Result::BufferOutOfBounds;
        }

        Bucket* bucket = &buckets_[header->superFrameNo & BufferSize];

        size_t payloadSize = size - sizeof(FrameType1);

        if (!bucket->active) {
            uint64_t order = recalculateSuperFrameNo(header->superFrameNo);
            if (order == bucket->deliveryOrder) {
                return Result::FragmentTooOld;
            }

            // Sanity check on total size to prevent huge allocations
            size_t totalSize = payloadSize * (static_cast<size_t>(header->ofFragmentNo) + 1);
            if (totalSize > 100 * 1024 * 1024) {  // 100MB max
                return Result::TooLargeFrame;
            }

            bucket->deliveryOrder = order;
            bucketMap_[order] = bucket;
            bucket->active = true;
            bucket->sourceId = sourceId;
            bucket->flags = getFlags(header->frameType);
            bucket->streamId = header->streamId;
            bucket->savedFrameNo = header->superFrameNo;
            bucket->receivedFragments.reset();
            bucket->receivedFragments[header->fragmentNo] = true;
            bucket->timeoutUs = nowUs() + (timeoutMs_ * 1000);
            bucket->fragmentCount = 1;  // First fragment received
            bucket->ofFragmentNo = header->ofFragmentNo;
            bucket->fragmentSize = payloadSize;
            bucket->pts = UINT64_MAX;
            bucket->dts = UINT64_MAX;

            // Get cached stream info
            Stream* stream = &streams_[header->streamId];
            bucket->payloadType = stream->payloadType;
            bucket->payloadCode = stream->payloadCode;

            bucket->frame = std::make_unique<SuperFrame>(totalSize);
            if (!bucket->frame->data) {
                bucketMap_.erase(order);
                bucket->active = false;
                return Result::MemoryAllocationError;
            }
            bucket->frame->size = totalSize;  // Set expected full frame size

            size_t offset = payloadSize * header->fragmentNo;
            std::memcpy(bucket->frame->data + offset, data + sizeof(FrameType1), payloadSize);

            return Result::Ok;
        }

        // Existing bucket
        if (header->superFrameNo != bucket->savedFrameNo) {
            return Result::BufferOutOfResources;
        }

        if (header->ofFragmentNo != bucket->ofFragmentNo ||
            header->fragmentNo > bucket->ofFragmentNo) {
            bucketMap_.erase(bucket->deliveryOrder);
            bucket->active = false;
            return Result::BufferOutOfBounds;
        }

        if (bucket->receivedFragments[header->fragmentNo]) {
            return Result::DuplicateFragment;
        }

        bucket->receivedFragments[header->fragmentNo] = true;
        bucket->fragmentCount++;

        size_t offset = bucket->fragmentSize * header->fragmentNo;
        std::memcpy(bucket->frame->data + offset, data + sizeof(FrameType1), payloadSize);

        return Result::Ok;
    }

    Result handleType2(const uint8_t* data, size_t size, uint8_t sourceId) {
        if (size < sizeof(FrameType2)) {
            return Result::FrameSizeMismatch;
        }

        std::lock_guard<std::recursive_mutex> lock(netMutex_);

        auto* header = reinterpret_cast<const FrameType2*>(data);

        if (size < sizeof(FrameType2) + header->sizeOfData) {
            return Result::FrameSizeMismatch;
        }

        // Bounds check: ofFragmentNo must fit in bitset (8192 max)
        if (header->ofFragmentNo >= 8192) {
            return Result::BufferOutOfBounds;
        }

        // Sanity check on total size
        size_t totalSize = (static_cast<size_t>(header->type1PacketSize) * header->ofFragmentNo) +
                           header->sizeOfData;
        if (totalSize > 100 * 1024 * 1024) {  // 100MB max
            return Result::TooLargeFrame;
        }

        Bucket* bucket = &buckets_[header->superFrameNo & BufferSize];

        if (!bucket->active) {
            uint64_t order = recalculateSuperFrameNo(header->superFrameNo);
            if (order == bucket->deliveryOrder) {
                return Result::FragmentTooOld;
            }

            bucket->deliveryOrder = order;
            bucketMap_[order] = bucket;
            bucket->active = true;
            bucket->sourceId = sourceId;
            bucket->flags = getFlags(header->frameType);
            bucket->streamId = header->streamId;
            bucket->savedFrameNo = header->superFrameNo;
            bucket->receivedFragments.reset();
            bucket->receivedFragments[header->ofFragmentNo] = true;
            bucket->timeoutUs = nowUs() + (timeoutMs_ * 1000);
            bucket->fragmentCount = 1;  // First fragment received
            bucket->ofFragmentNo = header->ofFragmentNo;
            bucket->fragmentSize = header->type1PacketSize;
            bucket->type2Size = header->sizeOfData;  // Store for potential relocation
            bucket->pts = header->pts;
            bucket->payloadType = header->payloadType;
            bucket->payloadCode = header->payloadCode;

            if (header->dtsPtsDiff == UINT32_MAX) {
                bucket->dts = UINT64_MAX;
            } else {
                bucket->dts = header->pts - header->dtsPtsDiff;
            }

            // Update stream cache
            Stream* stream = &streams_[header->streamId];
            stream->payloadType = header->payloadType;
            stream->payloadCode = header->payloadCode;

            bucket->frame = std::make_unique<SuperFrame>(totalSize);
            if (!bucket->frame->data) {
                bucketMap_.erase(order);
                bucket->active = false;
                return Result::MemoryAllocationError;
            }
            bucket->frame->size = totalSize;  // Explicitly set frame size

            size_t offset = static_cast<size_t>(header->type1PacketSize) * header->ofFragmentNo;
            std::memcpy(bucket->frame->data + offset, data + sizeof(FrameType2), header->sizeOfData);

            return Result::Ok;
        }

        // Existing bucket
        if (header->superFrameNo != bucket->savedFrameNo) {
            return Result::BufferOutOfResources;
        }

        if (header->ofFragmentNo != bucket->ofFragmentNo) {
            bucketMap_.erase(bucket->deliveryOrder);
            bucket->active = false;
            return Result::BufferOutOfBounds;
        }

        if (bucket->receivedFragments[header->ofFragmentNo]) {
            return Result::DuplicateFragment;
        }

        bucket->receivedFragments[header->ofFragmentNo] = true;
        bucket->fragmentCount++;
        bucket->pts = header->pts;
        bucket->payloadType = header->payloadType;
        bucket->payloadCode = header->payloadCode;
        bucket->flags = getFlags(header->frameType);
        bucket->type2Size = header->sizeOfData;  // Store for potential relocation by Type3

        if (header->dtsPtsDiff == UINT32_MAX) {
            bucket->dts = UINT64_MAX;
        } else {
            bucket->dts = header->pts - header->dtsPtsDiff;
        }

        // Update stream cache
        Stream* stream = &streams_[header->streamId];
        stream->payloadType = header->payloadType;
        stream->payloadCode = header->payloadCode;

        // Set actual frame size, accounting for Type3 if present
        size_t offset;
        if (bucket->type3Size > 0) {
            // Type3 exists: size = type1 fragments + type3 + type2
            bucket->frame->size = (bucket->fragmentSize * (header->ofFragmentNo - 1)) +
                                  bucket->type3Size + header->sizeOfData;
            // Type2 data follows Type3 data
            offset = (bucket->fragmentSize * (header->ofFragmentNo - 1)) + bucket->type3Size;
        } else {
            // No Type3: size = type1 fragments + type2
            bucket->frame->size = (bucket->fragmentSize * header->ofFragmentNo) + header->sizeOfData;
            offset = static_cast<size_t>(header->type1PacketSize) * header->ofFragmentNo;
        }

        std::memcpy(bucket->frame->data + offset, data + sizeof(FrameType2), header->sizeOfData);

        return Result::Ok;
    }

    Result handleType3(const uint8_t* data, size_t size, uint8_t sourceId) {
        if (size < sizeof(FrameType3)) {
            return Result::FrameSizeMismatch;
        }

        std::lock_guard<std::recursive_mutex> lock(netMutex_);

        auto* header = reinterpret_cast<const FrameType3*>(data);

        // Bounds check: ofFragmentNo must fit in bitset and be > 0 for Type3
        if (header->ofFragmentNo >= 8192 || header->ofFragmentNo == 0) {
            return Result::BufferOutOfBounds;
        }

        Bucket* bucket = &buckets_[header->superFrameNo & BufferSize];

        uint16_t fragmentNo = header->ofFragmentNo - 1;  // Type3 is always penultimate
        size_t payloadSize = size - sizeof(FrameType3);

        // Sanity check on total size
        size_t totalSize = (static_cast<size_t>(header->type1PacketSize) * (header->ofFragmentNo - 1)) +
                           payloadSize;
        if (totalSize > 100 * 1024 * 1024) {  // 100MB max
            return Result::TooLargeFrame;
        }

        if (!bucket->active) {
            uint64_t order = recalculateSuperFrameNo(header->superFrameNo);
            if (order == bucket->deliveryOrder) {
                return Result::FragmentTooOld;
            }

            bucket->deliveryOrder = order;
            bucketMap_[order] = bucket;
            bucket->active = true;
            bucket->sourceId = sourceId;
            bucket->flags = getFlags(header->frameType);
            bucket->streamId = header->streamId;
            bucket->savedFrameNo = header->superFrameNo;
            bucket->receivedFragments.reset();
            bucket->receivedFragments[fragmentNo] = true;
            bucket->timeoutUs = nowUs() + (timeoutMs_ * 1000);
            bucket->fragmentCount = 1;  // First fragment received
            bucket->ofFragmentNo = header->ofFragmentNo;
            bucket->fragmentSize = header->type1PacketSize;
            bucket->type3Size = payloadSize;  // Store Type3 payload size
            bucket->pts = UINT64_MAX;
            bucket->dts = UINT64_MAX;

            // Get cached stream info
            Stream* stream = &streams_[header->streamId];
            bucket->payloadType = stream->payloadType;
            bucket->payloadCode = stream->payloadCode;

            bucket->frame = std::make_unique<SuperFrame>(totalSize);
            if (!bucket->frame->data) {
                bucketMap_.erase(order);
                bucket->active = false;
                return Result::MemoryAllocationError;
            }
            bucket->frame->size = totalSize;  // Set expected frame size

            size_t offset = header->type1PacketSize * fragmentNo;
            std::memcpy(bucket->frame->data + offset, data + sizeof(FrameType3), payloadSize);

            return Result::Ok;
        }

        // Existing bucket
        if (header->superFrameNo != bucket->savedFrameNo) {
            return Result::BufferOutOfResources;
        }

        if (header->ofFragmentNo != bucket->ofFragmentNo || fragmentNo > bucket->ofFragmentNo) {
            bucketMap_.erase(bucket->deliveryOrder);
            bucket->active = false;
            return Result::BufferOutOfBounds;
        }

        if (bucket->receivedFragments[fragmentNo]) {
            return Result::DuplicateFragment;
        }

        bucket->receivedFragments[fragmentNo] = true;
        bucket->fragmentCount++;
        bucket->type3Size = payloadSize;  // Store Type3 payload size for Type2's calculation

        // If Type2 was received before Type3, we need to relocate Type2's data
        // Type2 was placed at fragmentSize * ofFragmentNo, but should be at
        // fragmentSize * (ofFragmentNo - 1) + type3Size
        if (bucket->receivedFragments[header->ofFragmentNo] && bucket->type2Size > 0) {
            size_t oldOffset = bucket->fragmentSize * header->ofFragmentNo;
            size_t newOffset = (bucket->fragmentSize * (header->ofFragmentNo - 1)) + payloadSize;
            // Use memmove because regions may overlap
            std::memmove(bucket->frame->data + newOffset,
                         bucket->frame->data + oldOffset,
                         bucket->type2Size);
            // Update frame size now that we know type3Size
            bucket->frame->size = (bucket->fragmentSize * (header->ofFragmentNo - 1)) +
                                  payloadSize + bucket->type2Size;
        } else if (!bucket->receivedFragments[header->ofFragmentNo]) {
            // Only update frame size if Type2 hasn't been received yet
            bucket->frame->size = (bucket->fragmentSize * (header->ofFragmentNo - 1)) + payloadSize;
        }

        size_t offset = bucket->fragmentSize * fragmentNo;
        std::memcpy(bucket->frame->data + offset, data + sizeof(FrameType3), payloadSize);

        return Result::Ok;
    }

    void deliverFrame(Bucket* bucket) {
        bucket->frame->payloadType = bucket->payloadType;
        bucket->frame->payloadCode = bucket->payloadCode;
        bucket->frame->pts = bucket->pts;
        bucket->frame->dts = bucket->dts;
        bucket->frame->streamId = bucket->streamId;
        bucket->frame->sourceId = bucket->sourceId;
        bucket->frame->flags = bucket->flags;
        bucket->frame->superFrameNo = bucket->savedFrameNo;
        bucket->frame->broken = (bucket->fragmentCount != bucket->ofFragmentNo + 1);

        if (mode_ == ReceiverMode::Threaded) {
            std::lock_guard<std::mutex> lock(deliveryMutex_);
            deliveryQueue_.push_back(std::move(bucket->frame));
            deliveryReady_ = true;
            deliveryCondition_.notify_one();
        } else if (callback_) {
            callback_(std::move(bucket->frame));
        }

        bucketMap_.erase(bucket->deliveryOrder);
        bucket->active = false;
        bucket->frame = nullptr;
    }

    void processTimeouts() {
        int64_t now = nowUs();
        std::vector<Bucket*> toDeliver;

        for (auto& [order, bucket] : bucketMap_) {
            // Total fragments = ofFragmentNo + 1 (since ofFragmentNo is 0-based index of last fragment)
            bool complete = (bucket->fragmentCount == bucket->ofFragmentNo + 1);
            bool timedOut = (bucket->timeoutUs <= now);

            if (complete || timedOut) {
                toDeliver.push_back(bucket);
            }
        }

        for (auto* bucket : toDeliver) {
            deliverFrame(bucket);
        }
    }

    void workerLoop() {
        constexpr int64_t sleepUs = 10000;  // 10ms

        while (running_.load()) {
            {
                std::lock_guard<std::recursive_mutex> lock(netMutex_);
                processTimeouts();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
        }
    }

    void deliveryLoop() {
        while (running_.load()) {
            SuperFramePtr frame;
            {
                std::unique_lock<std::mutex> lock(deliveryMutex_);
                deliveryCondition_.wait(lock, [this] {
                    return deliveryReady_ || !running_.load();
                });

                if (!running_.load() && deliveryQueue_.empty()) break;

                if (!deliveryQueue_.empty()) {
                    frame = std::move(deliveryQueue_.front());
                    deliveryQueue_.pop_front();
                }

                if (deliveryQueue_.empty()) {
                    deliveryReady_ = false;
                }
            }

            if (frame && callback_) {
                callback_(std::move(frame));
            }
        }
    }

    uint32_t timeoutMs_;
    uint32_t holTimeoutMs_;
    ReceiverMode mode_;

    Bucket* buckets_;
    std::map<uint64_t, Bucket*> bucketMap_;
    Stream streams_[256];  // All 256 stream IDs (0-255)
    std::recursive_mutex netMutex_;

    uint16_t oldFrameNo_ = 0;
    uint64_t frameNoRecalc_ = 0;
    bool firstFrame_ = true;

    std::atomic<bool> running_{false};
    std::thread workerThread_;
    std::thread deliveryThread_;

    std::mutex deliveryMutex_;
    std::deque<SuperFramePtr> deliveryQueue_;
    std::condition_variable deliveryCondition_;
    bool deliveryReady_ = false;

    ReceiveCallback callback_;
};

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

