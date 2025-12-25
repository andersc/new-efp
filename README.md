# Elastic Frame Protocol (EFP)

A lightweight, header-only C++17 library for fragmenting and reassembling data over unreliable or size-limited transport layers.

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application                              │
│  (Video, Audio, Sensors, Files, RPC, Custom Data)              │
├─────────────────────────────────────────────────────────────────┤
│                     Elastic Frame Protocol                      │
│  • Fragments large data    • Reassembles fragments              │
│  • 64-bit timestamps       • Loss detection                     │
│  • Stream multiplexing     • Minimal overhead (~0.5%)           │
├─────────────────────────────────────────────────────────────────┤
│                      Transport Layer                            │
│  (UDP, SRT, RIST, QUIC, WebRTC, TCP, Custom)                   │
└─────────────────────────────────────────────────────────────────┘
```

## Features

- **Header-only** — Single include, no library linking required
- **Generic** — Works with any data type (media, sensors, files, RPC)
- **Minimal overhead** — ~0.5% protocol overhead
- **64-bit timestamps** — No wraparound issues (unlike MPEG-TS 33-bit)
- **Transport agnostic** — Works over UDP, SRT, RIST, QUIC, or any transport
- **Stream multiplexing** — Up to 256 independent streams
- **Configurable** — Template-based buffer sizing with compile-time validation
- **Thread-safe** — Built-in threading or run-to-completion mode

## Quick Start

```cpp
#include "efp.h"

int main() {
    efp::Sender lSender(1400);  // MTU size
    efp::Receiver lReceiver(100);  // 100ms timeout

    // Receive callback
    lReceiver.setCallback([](efp::SuperFramePtr apFrame) {
        // Process received data
        process(apFrame->mpData, apFrame->mSize);
    });

    // Connect sender to receiver (via your transport)
    lSender.setCallback([&](const uint8_t* apData, size_t aSize, uint8_t aStreamId) {
        // Send over network, then on receive:
        lReceiver.receive(apData, aSize);
    });

    // Send data
    std::vector<uint8_t> lPayload = getData();
    lSender.send(lPayload, 0x01, lPts, lDts, 0, lStreamId);
}
```

## Installation

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(efp
    GIT_REPOSITORY https://github.com/yourorg/efp.git
    GIT_TAG v1.0.0
)
FetchContent_MakeAvailable(efp)

target_link_libraries(your_target PRIVATE efp::efp)
```

### Manual

Copy `efp.h`, `efp_internal.h`, and optionally `efp_media_types.h` to your project.

```cpp
#include "efp.h"
```

## API Reference

### Sender

```cpp
template<uint16_t BUFFER_SIZE = 8191>
class Sender {
    explicit Sender(uint16_t aMtu);

    void setCallback(SendCallback aCallback);

    Result send(const uint8_t* apData, size_t aSize,
                uint8_t aPayloadType, uint64_t aPts, uint64_t aDts,
                uint32_t aPayloadCode, uint8_t aStreamId,
                uint8_t aFlags = Flags::NONE);
};
```

### Receiver

```cpp
template<uint16_t BUFFER_SIZE = 8191>
class Receiver {
    explicit Receiver(uint32_t aTimeoutMs = 100,
                      uint32_t aHolTimeoutMs = 0,
                      ReceiverMode aMode = ReceiverMode::THREADED);

    void setCallback(ReceiveCallback aCallback);

    Result receive(const uint8_t* apData, size_t aSize, uint8_t aSourceId = 0);

    void poll();  // For RUN_TO_COMPLETION mode
    void stop();
};
```

### SuperFrame (received data)

```cpp
class SuperFrame {
    uint8_t* mpData;        // Frame data (32-byte aligned)
    size_t   mSize;         // Frame size
    uint8_t  mPayloadType;  // User-defined type
    uint32_t mPayloadCode;  // User-defined code
    uint64_t mPts;          // Presentation timestamp
    uint64_t mDts;          // Decode timestamp
    uint8_t  mStreamId;     // Stream identifier
    uint8_t  mSourceId;     // Source identifier
    uint8_t  mFlags;        // Frame flags
    bool     mBroken;       // True if incomplete
};
```

## Protocol Format

EFP uses 4 frame types optimized for different scenarios:

| Type | Size | Purpose |
|------|------|---------|
| Type0 | 1B | Signaling (reserved) |
| Type1 | 8B | Fragment header |
| Type2 | 27B | Final fragment with metadata |
| Type3 | 8B | Penultimate overflow fragment |

### Frame Structure

```
Type1 (8 bytes):
┌──────────┬──────────┬──────────────┬────────────┬──────────────┐
│ Type+Flg │ StreamID │ SuperFrameNo │ FragmentNo │ OfFragmentNo │
│  1 byte  │  1 byte  │   2 bytes    │  2 bytes   │   2 bytes    │
└──────────┴──────────┴──────────────┴────────────┴──────────────┘

Type2 (27 bytes):
┌──────────┬──────────┬─────────────┬────────────┬──────────────┐
│ Type+Flg │ StreamID │ PayloadType │ SizeOfData │ SuperFrameNo │
│  1 byte  │  1 byte  │   1 byte    │  2 bytes   │   2 bytes    │
├──────────────┬───────────────┬──────────┬────────────┬────────┤
│ OfFragmentNo │ Type1PktSize  │   PTS    │ DtsPtsDiff │  Code  │
│   2 bytes    │    2 bytes    │ 8 bytes  │  4 bytes   │ 4 bytes│
└──────────────┴───────────────┴──────────┴────────────┴────────┘
```

## Configuration

### Buffer Size

The circular buffer size must be `2^n - 1` for efficient bitmask operations:

```cpp
efp::Sender<1023> lSender(1400);   // 2^10 - 1
efp::Sender<4095> lSender(1400);   // 2^12 - 1
efp::Sender<8191> lSender(1400);   // 2^13 - 1 (default)
efp::Sender<16383> lSender(1400);  // 2^14 - 1
```

Invalid sizes cause compile-time errors:

```cpp
efp::Sender<1000> lSender(1400);  // ERROR: not 2^n - 1
```

### Receiver Modes

```cpp
// Threaded mode (default): Background threads handle assembly
efp::Receiver lReceiver(100, 0, efp::ReceiverMode::THREADED);

// Run-to-completion: No threads, caller drives processing
efp::Receiver lReceiver(100, 0, efp::ReceiverMode::RUN_TO_COMPLETION);
lReceiver.poll();  // Must call periodically
```

## Media Types (Optional)

Include `efp_media_types.h` for predefined media constants:

```cpp
#include "efp_media_types.h"

lSender.send(lNalUnit,
            efp::media::PayloadType::H264,
            lPts,
            lDts,
            efp::media::PayloadCode::ANXB,  // Annex B framing
            lStreamId);
```

## Building Tests

```bash
mkdir build && cd build
cmake -DEFP_BUILD_TESTS=ON ..
make
ctest
```

### Running Specific Test Suites

```bash
# Run all tests
./efp_tests

# Run stress tests only
./efp_tests -ts="Stress Tests"

# Run C API tests
./efp_c_tests
```

## Code Style

This project follows a specific naming convention (see `codestyle.txt`):

- **Local variables**: `l` prefix (e.g., `lResult`, `lSize`)
- **Parameters**: `a` prefix for value, `r` for reference, `p` for pointer (e.g., `aSize`, `rData`, `apBuffer`)
- **Member variables**: `m` prefix (e.g., `mSize`, `mCallback`)
- **Member pointers**: `mp` prefix (e.g., `mpData`, `mpFrame`)
- **Global variables**: `g` prefix (e.g., `gHandlesMutex`)
- **Constants**: `UPPER_SNAKE_CASE` (e.g., `BUFFER_SIZE`, `INLINE_PAYLOAD`)
- **Types**: PascalCase (e.g., `SuperFrame`, `ReceiverMode`)
- **Functions**: camelCase (e.g., `setCallback`, `receive`)
- **Casts**: C-style casts only (e.g., `(uint8_t)(value)`)
- **No `_` prefix/postfix** on identifiers

## Use Cases

- **Video streaming** — Fragment H.264/H.265/AV1 NAL units
- **Audio streaming** — Transport AAC/Opus/PCM frames
- **IoT sensors** — Aggregate and transport sensor readings
- **File transfer** — Chunk large files for unreliable transport
- **RPC/messaging** — Frame protocol buffers or JSON messages
- **Game networking** — Reliable delivery over UDP

## License

MIT License - See [LICENSE](LICENSE) for details.

## Credits

Anders Cedronius
