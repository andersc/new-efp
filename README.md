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
    efp::Sender sender(1400);  // MTU size
    efp::Receiver receiver(100);  // 100ms timeout

    // Receive callback
    receiver.setCallback([](efp::SuperFramePtr frame) {
        // Process received data
        process(frame->data, frame->size);
    });

    // Connect sender to receiver (via your transport)
    sender.setCallback([&](const uint8_t* data, size_t size, uint8_t streamId) {
        // Send over network, then on receive:
        receiver.receive(data, size);
    });

    // Send data
    std::vector<uint8_t> payload = getData();
    sender.send(payload, 0x01, pts, dts, 0, streamId);
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
template<uint16_t BufferSize = 8191>
class Sender {
    explicit Sender(uint16_t mtu);

    void setCallback(SendCallback callback);

    Result send(const uint8_t* data, size_t size,
                uint8_t payloadType, uint64_t pts, uint64_t dts,
                uint32_t payloadCode, uint8_t streamId,
                uint8_t flags = Flags::None);
};
```

### Receiver

```cpp
template<uint16_t BufferSize = 8191>
class Receiver {
    explicit Receiver(uint32_t timeoutMs = 100,
                      uint32_t holTimeoutMs = 0,
                      ReceiverMode mode = ReceiverMode::Threaded);

    void setCallback(ReceiveCallback callback);

    Result receive(const uint8_t* data, size_t size, uint8_t sourceId = 0);

    void poll();  // For RunToCompletion mode
    void stop();
};
```

### SuperFrame (received data)

```cpp
class SuperFrame {
    uint8_t* data;         // Frame data (32-byte aligned)
    size_t   size;         // Frame size
    uint8_t  payloadType;  // User-defined type
    uint32_t payloadCode;  // User-defined code
    uint64_t pts;          // Presentation timestamp
    uint64_t dts;          // Decode timestamp
    uint8_t  streamId;     // Stream identifier
    uint8_t  sourceId;     // Source identifier
    uint8_t  flags;        // Frame flags
    bool     broken;       // True if incomplete
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
efp::Sender<1023> sender(1400);   // 2^10 - 1
efp::Sender<4095> sender(1400);   // 2^12 - 1
efp::Sender<8191> sender(1400);   // 2^13 - 1 (default)
efp::Sender<16383> sender(1400);  // 2^14 - 1
```

Invalid sizes cause compile-time errors:

```cpp
efp::Sender<1000> sender(1400);  // ERROR: not 2^n - 1
```

### Receiver Modes

```cpp
// Threaded mode (default): Background threads handle assembly
efp::Receiver receiver(100, 0, efp::ReceiverMode::Threaded);

// Run-to-completion: No threads, caller drives processing
efp::Receiver receiver(100, 0, efp::ReceiverMode::RunToCompletion);
receiver.poll();  // Must call periodically
```

## Media Types (Optional)

Include `efp_media_types.h` for predefined media constants:

```cpp
#include "efp_media_types.h"

sender.send(nalUnit,
            efp::media::PayloadType::H264,
            pts,
            dts,
            efp::media::PayloadCode::ANXB,  // Annex B framing
            streamId);
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
