//
// Elastic Frame Protocol - Internal Header
// Copyright 2024-2025
//
// Internal packet structures - not for public use
//

#ifndef EFP_INTERNAL_H
#define EFP_INTERNAL_H

#include <cstdint>

namespace efp {

// Frame type identifiers (4 LSB of first byte)
enum class FrameType : uint8_t {
    TYPE0 = 0,  // Signaling frame (1 byte header)
    TYPE1 = 1,  // Fragment frame (8 bytes header)
    TYPE2 = 2,  // Final/small frame with metadata (27 bytes header)
    TYPE3 = 3   // Penultimate overflow frame (8 bytes header)
};

// Flags (4 MSB of first byte)
namespace Flags {
    constexpr uint8_t NONE           = 0b00000000;
    constexpr uint8_t INLINE_PAYLOAD = 0b00010000;  // Frame contains inline/embedded payload
    constexpr uint8_t PRIORITY_0     = 0b00000000;  // Low priority
    constexpr uint8_t PRIORITY_1     = 0b00100000;  // Normal priority
    constexpr uint8_t PRIORITY_2     = 0b01000000;  // High priority
    constexpr uint8_t PRIORITY_3     = 0b01100000;  // Highest priority
    constexpr uint8_t RESERVED       = 0b10000000;  // Reserved for future use
}

// Type0: Signaling frame - minimal 1 byte header
struct __attribute__((packed)) FrameType0 {
    uint8_t mFrameType = (uint8_t)(FrameType::TYPE0);
};
static_assert(sizeof(FrameType0) == 1, "FrameType0 must be 1 byte");

// Type1: Fragment frame - 8 bytes header
// Used for all fragments except the last (Type2) and penultimate overflow (Type3)
struct __attribute__((packed)) FrameType1 {
    uint8_t  mFrameType    = (uint8_t)(FrameType::TYPE1);  // Frame type + flags
    uint8_t  mStreamId     = 0;       // Stream identifier
    uint16_t mSuperFrameNo = 0;       // Super frame sequence number
    uint16_t mFragmentNo   = 0;       // This fragment's number (0-based)
    uint16_t mOfFragmentNo = 0;       // Total number of fragments (last fragment index)
};
static_assert(sizeof(FrameType1) == 8, "FrameType1 must be 8 bytes");

// Type2: Final/small frame with full metadata - 27 bytes header
// Used for the last fragment or single small frames
struct __attribute__((packed)) FrameType2 {
    uint8_t  mFrameType       = (uint8_t)(FrameType::TYPE2);  // Frame type + flags
    uint8_t  mStreamId        = 0;           // Stream identifier
    uint8_t  mPayloadType     = 0;           // User-defined payload type
    uint16_t mSizeOfData      = 0;           // Size of payload in this fragment
    uint16_t mSuperFrameNo    = 0;           // Super frame sequence number
    uint16_t mOfFragmentNo    = 0;           // Total number of fragments (last fragment index)
    uint16_t mType1PacketSize = 0;           // Size of Type1 fragment payloads
    uint64_t mPts             = UINT64_MAX;  // Presentation timestamp
    uint32_t mDtsPtsDiff      = UINT32_MAX;  // DTS = PTS - dtsPtsDiff (UINT32_MAX = no DTS)
    uint32_t mPayloadCode     = UINT32_MAX;  // User-defined payload code
};
static_assert(sizeof(FrameType2) == 27, "FrameType2 must be 27 bytes");

// Type3: Penultimate overflow frame - 8 bytes header
// Used when the second-to-last fragment doesn't fit Type2's remaining space
struct __attribute__((packed)) FrameType3 {
    uint8_t  mFrameType       = (uint8_t)(FrameType::TYPE3);  // Frame type + flags
    uint8_t  mStreamId        = 0;       // Stream identifier
    uint16_t mSuperFrameNo    = 0;       // Super frame sequence number
    uint16_t mType1PacketSize = 0;       // Size of Type1 fragment payloads
    uint16_t mOfFragmentNo    = 0;       // Total number of fragments (last fragment index)
};
static_assert(sizeof(FrameType3) == 8, "FrameType3 must be 8 bytes");

// Helper to extract frame type from first byte
inline FrameType getFrameType(uint8_t aFirstByte) {
    return (FrameType)(aFirstByte & 0x0F);
}

// Helper to extract flags from first byte
inline uint8_t getFlags(uint8_t aFirstByte) {
    return aFirstByte & 0xF0;
}

// Helper to combine frame type and flags
inline uint8_t makeFrameTypeByte(FrameType aType, uint8_t aFlags) {
    return (uint8_t)(aType) | (aFlags & 0xF0);
}

} // namespace efp

#endif // EFP_INTERNAL_H

