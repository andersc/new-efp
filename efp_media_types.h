//
// Elastic Frame Protocol - Media Types (Optional)
// Copyright 2024-2025
//
// Legacy media type constants for backward compatibility
// Include this header only if you need predefined media types
//

#ifndef EFP_MEDIA_TYPES_H
#define EFP_MEDIA_TYPES_H

#include <cstdint>

namespace efp {
namespace media {

// Helper macro to create 4-character codes
#define EFP_FOURCC(c0, c1, c2, c3) \
    (static_cast<uint32_t>(c0) << 24 | static_cast<uint32_t>(c1) << 16 | \
     static_cast<uint32_t>(c2) << 8 | static_cast<uint32_t>(c3))

// Payload types (uint8_t)
// Types 0x00-0x7F: Self-describing (no code needed)
// Types 0x80-0xFF: Require payloadCode for format specification
namespace PayloadType {
    constexpr uint8_t Unknown     = 0x00;  // Unknown content
    constexpr uint8_t PrivateData = 0x01;  // User-defined format
    constexpr uint8_t ADTS        = 0x02;  // MPEG-4 AAC ADTS framing
    constexpr uint8_t MPEGTS      = 0x03;  // ITU-T H.222 188-byte TS
    constexpr uint8_t MPEGPES     = 0x04;  // ITU-T H.222 PES packets
    constexpr uint8_t JPEG2000    = 0x05;  // ITU-T T.800 Annex M
    constexpr uint8_t JPEG        = 0x06;  // ITU-T T.81
    constexpr uint8_t JPEGXS      = 0x07;  // ISO/IEC 21122-3
    constexpr uint8_t PCMAudio    = 0x08;  // AES-3 framing
    constexpr uint8_t NDI         = 0x09;  // NDI format
    constexpr uint8_t JSON        = 0x0A;  // RFC 8259 JSON

    // Types requiring payloadCode (MSB set)
    constexpr uint8_t EFPSignal   = 0x80;  // EFP signaling (code: JSON/BINR)
    constexpr uint8_t DIDSDID     = 0x81;  // DID/SDID format (code: FOURCC)
    constexpr uint8_t SDI         = 0x82;  // SDI format (code: FOURCC)
    constexpr uint8_t H264        = 0x83;  // ITU-T H.264 (code: ANXB/AVCC)
    constexpr uint8_t H265        = 0x84;  // ITU-T H.265 (code: ANXB/AVCC)
    constexpr uint8_t H266        = 0x85;  // ITU-T H.266 (code: ANXB/AVCC)
    constexpr uint8_t AV1         = 0x86;  // AOM AV1 (code: XOBU)
    constexpr uint8_t MP4         = 0x87;  // ISO/IEC 14496-12 (code: box name)
    constexpr uint8_t AAC         = 0x88;  // MPEG-4 pt. 14 (code: ADTS/XRAW)
    constexpr uint8_t Opus        = 0x89;  // RFC 6716 Opus
    constexpr uint8_t FLAC        = 0x8A;  // Xiph.Org FLAC
}

// Common payload codes (FOURCC values)
namespace PayloadCode {
    constexpr uint32_t None = UINT32_MAX;  // No code / not applicable

    // Video framing codes
    constexpr uint32_t ANXB = EFP_FOURCC('A', 'N', 'X', 'B');  // Annex B framing
    constexpr uint32_t AVCC = EFP_FOURCC('A', 'V', 'C', 'C');  // AVCC framing
    constexpr uint32_t XOBU = EFP_FOURCC('X', 'O', 'B', 'U');  // Open Bitstream Units

    // Audio framing codes
    constexpr uint32_t ADTS = EFP_FOURCC('A', 'D', 'T', 'S');  // ADTS framing
    constexpr uint32_t XRAW = EFP_FOURCC('X', 'R', 'A', 'W');  // Raw audio

    // Signaling codes
    constexpr uint32_t JSON = EFP_FOURCC('J', 'S', 'O', 'N');  // JSON format
    constexpr uint32_t BINR = EFP_FOURCC('B', 'I', 'N', 'R');  // Binary format
}

// Embedded data types
namespace EmbeddedType {
    constexpr uint8_t Illegal     = 0x00;  // May not be used
    constexpr uint8_t PrivateData = 0x01;  // Private embedded data
    constexpr uint8_t H222PMT     = 0x02;  // PMT from H.222
    constexpr uint8_t MP4FragBox  = 0x03;  // MP4 fragment boxes (excluding payload)
    constexpr uint8_t LastMarker  = 0x80;  // MSB set = last embedded section
}

} // namespace media
} // namespace efp

#endif // EFP_MEDIA_TYPES_H

