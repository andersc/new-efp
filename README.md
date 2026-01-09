# Elastic Frame Protocol (EFP)

[![Linux Build](https://github.com/yourorg/efp/actions/workflows/linux.yml/badge.svg)](https://github.com/yourorg/efp/actions/workflows/linux.yml)
[![macOS Build](https://github.com/yourorg/efp/actions/workflows/macos.yml/badge.svg)](https://github.com/yourorg/efp/actions/workflows/macos.yml)
[![Windows Build](https://github.com/yourorg/efp/actions/workflows/windows.yml/badge.svg)](https://github.com/yourorg/efp/actions/workflows/windows.yml)

A lightweight, header-only C++20 library for fragmenting and reassembling data over unreliable or size-limited transport layers.

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

## C++20 Features

This library leverages modern C++20 features for improved performance and safety:

- **Concepts** — `ValidBufferSize`, `SendCallbackConcept`, `ReceiveCallbackConcept` for compile-time validation
- **`std::span`** — Zero-copy buffer views for efficient data passing (used in send callback)
- **`std::thread` with `std::atomic`** — Cross-platform threading with atomic stop flags
- **`[[likely]]`/`[[unlikely]]`** — Branch prediction hints for optimized hot paths
- **`[[nodiscard]]`** — Prevents ignoring important return values
- **`[[no_unique_address]]`** — Empty callback types don't take space in the class
- **`consteval`** — Compile-time only evaluation for version queries
- **`<bit>` header** — `std::has_single_bit` for power-of-2 validation
- **Template-based callbacks** — Callbacks are template parameters for zero-overhead inlining (no `std::function`)

## Platform Support

EFP is tested on:
- **Linux** — GCC and Clang
- **macOS** — Apple Clang
- **Windows** — MSVC

## Quick Start

```cpp
#include "efp.h"

int main() {
    // Create receiver with callback (callback is required at construction time)
    auto lReceiver = efp::makeReceiver([](efp::SuperFramePtr apFrame) {
        // Process received data
        process(apFrame->mpData, apFrame->mSize);
    }, 100);  // 100ms timeout

    // Create sender with callback (callback is required at construction time)
    auto lSender = efp::makeSender(1400, [&](std::span<const uint8_t> aData, uint8_t aStreamId) {
        // Send over network, then on receive:
        lReceiver.receive(aData);
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
// Template-based Sender with compile-time callback type deduction
template<typename SendCallbackT, uint16_t BUFFER_SIZE = 8191>
class Sender {
    explicit Sender(uint16_t aMtu, SendCallbackT aCallback,
                    SubFragmentMode aSubFragmentMode = SubFragmentMode::SINGLE,
                    uint32_t aRetentionMs = 0,
                    size_t aRetentionMaxBytes = 50 * 1024 * 1024);

    Result send(std::span<const uint8_t> aData,
                uint8_t aPayloadType, uint64_t aPts, uint64_t aDts,
                uint32_t aPayloadCode, uint8_t aStreamId,
                uint8_t aFlags = Flags::NONE);

    Result receiveNack(std::span<const uint8_t> aData);  // Process NACK, queue retransmits
    size_t processRetransmits(size_t aMaxCount = SIZE_MAX);  // Flush retransmit queue
    SenderStatistics getStatistics() const;
};

// Sub-fragment modes for bundled transmission
enum class SubFragmentMode : uint8_t {
    SINGLE  = 1,  // Normal mode: 1 fragment per UDP packet (default)
    HALF    = 2,  // 2 fragments per UDP packet
    QUARTER = 4,  // 4 fragments per UDP packet
    EIGHTH  = 8   // 8 fragments per UDP packet
};

// Factory function (recommended)
auto lSender = efp::makeSender(mtu, [](std::span<const uint8_t> data, uint8_t stream) {
    // Send callback
});

// With sub-fragmentation and retention for retransmit support
auto lSender = efp::makeSender(mtu, callback,
    efp::SubFragmentMode::QUARTER,  // Bundle 4 fragments per UDP packet
    1000,                           // 1 second retention for retransmit
    50 * 1024 * 1024);              // 50MB max retention buffer
```

### Receiver

```cpp
// Template-based Receiver with compile-time callback type deduction
template<typename ReceiveCallbackT, typename NackCallbackT, uint16_t BUFFER_SIZE = 8191>
class Receiver {
    explicit Receiver(ReceiveCallbackT aCallback,
                      NackCallbackT aNackCallback,
                      uint32_t aTimeoutMs = 100,
                      uint32_t aHolTimeoutMs = 0,
                      uint8_t aMaxNackRetries = 3,
                      uint32_t aNackIntervalMs = 0,  // 0 = adaptive
                      ReceiverMode aMode = ReceiverMode::THREADED);

    Result receive(std::span<const uint8_t> aData, uint8_t aSourceId = 0);

    void poll();  // For RUN_TO_COMPLETION mode
    void stop();
    ReceiverStatistics getStatistics() const;
};

// Factory function (recommended)
auto lReceiver = efp::makeReceiver(
    [](efp::SuperFramePtr frame) {
        // Receive callback - process complete/broken frames
    },
    [&lSender](std::span<const uint8_t> nackData) {
        // NACK callback - send NACK back to sender
        sendToSender(nackData);  // Your network send function
        // Or directly: lSender.receiveNack(nackData);
    },
    100,  // Frame timeout in ms
    50,   // HOL timeout in ms (0 = disabled)
    3,    // Max NACK retries (0 = disabled)
    0     // NACK interval (0 = adaptive based on jitter)
);
```

#### NACK and Retransmission

The receiver automatically detects missing fragments and sends NACK (Negative Acknowledgment) messages to request retransmission:

- **Adaptive timing**: By default (`aNackIntervalMs=0`), NACK timing adapts to network jitter
- **Exponential backoff**: Each retry waits longer (delay doubles)
- **Retry limit**: After `aMaxNackRetries` attempts, the frame is delivered as broken
- **HOL blocking timeout**: If an older incomplete frame blocks newer complete frames, it's delivered after `aHolTimeoutMs`

```cpp
// Example: Full sender-receiver setup with NACK support
auto lSender = efp::makeSender(1400, sendCallback,
    efp::SubFragmentMode::SINGLE,
    1000);  // 1 second retention for retransmit

auto lReceiver = efp::makeReceiver(
    receiveCallback,
    [&lSender](std::span<const uint8_t> nackData) {
        // Route NACK back to sender and process retransmits immediately
        lSender.receiveNack(nackData);
        lSender.processRetransmits();  // Flush retransmit queue
    },
    200,  // 200ms frame timeout
    50,   // 50ms HOL timeout
    3,    // 3 NACK retries
    20    // 20ms fixed NACK interval (or 0 for adaptive)
);
```

**Sender retransmission**: When using `SubFragmentMode::SINGLE`, call `processRetransmits()` after `receiveNack()` to immediately send queued retransmissions. For bundled modes (`HALF`, `QUARTER`, `EIGHTH`), retransmits are processed automatically. You can also call `processRetransmits(n)` to limit the number of retransmissions per call for rate limiting.

**Validation**: The receiver throws `std::invalid_argument` if:
- `aHolTimeoutMs >= aTimeoutMs` (HOL must be less than frame timeout)
- `aNackIntervalMs * aMaxNackRetries >= aTimeoutMs` (NACK budget must fit in timeout)

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

### Statistics

```cpp
struct SenderStatistics {
    uint64_t mRetentionBufferBytes;      // Current bytes in retention buffer
    uint32_t mRetentionBufferFragments;  // Current fragments in retention buffer
    uint64_t mFragmentsSent;             // Total fragments sent
    uint64_t mBundlesSent;               // Total Type4 bundles sent
    uint64_t mNacksReceived;             // Total NACKs received
    uint64_t mRetransmittedFragments;    // Total fragments retransmitted
    uint32_t mRetransmitQueueSize;       // Current retransmit queue depth
    double   mFragmentsPerSecond;        // Recent send rate
    double   mRetransmitsPerSecond;      // Recent retransmit rate
};

struct ReceiverStatistics {
    uint64_t mFragmentsReceived;     // Total fragments received
    uint64_t mBundlesReceived;       // Total Type4 bundles received
    uint64_t mNacksSent;             // Total NACKs sent
    uint64_t mCompleteFrames;        // Total complete SuperFrames delivered
    uint64_t mBrokenFrames;          // Total broken SuperFrames delivered
    uint64_t mDuplicateFragments;    // Duplicate fragments received
    uint32_t mPendingBuckets;        // Current active buckets
    double   mFragmentsPerSecond;    // Recent receive rate
    double   mNacksPerSecond;        // Recent NACK rate
};
```

## Protocol Format

EFP uses 5 frame types optimized for different scenarios:

| Type | Size | Purpose |
|------|------|---------|
| Type0 | 3B+ | Signaling (NACK, etc.) |
| Type1 | 8B | Fragment header |
| Type2 | 27B | Final fragment with metadata |
| Type3 | 8B | Penultimate overflow fragment |
| Type4 | 2B | Bundle wrapper (contains multiple Type1/2/3) |

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

The circular buffer size must be `2^n - 1` for efficient bitmask operations.
The default buffer size (8191) is suitable for most use cases.

For custom buffer sizes, use the class directly with explicit template parameters:

```cpp
// Define a callable type for custom buffer sizes
struct MySendCallback {
    void operator()(std::span<const uint8_t> data, uint8_t stream) { /* ... */ }
};

efp::Sender<MySendCallback, 1023> lSender(1400, MySendCallback{});   // 2^10 - 1
efp::Sender<MySendCallback, 16383> lSender(1400, MySendCallback{});  // 2^14 - 1
```

Invalid sizes cause compile-time errors:

```cpp
efp::Sender<MySendCallback, 1000> lSender(1400, cb);  // ERROR: not 2^n - 1
```

### Receiver Modes

```cpp
// Threaded mode (default): Background threads handle assembly
auto lReceiver = efp::makeReceiver(callback, 100, 0, efp::ReceiverMode::THREADED);

// Run-to-completion: No threads, caller drives processing
auto lReceiver = efp::makeReceiver(callback, 100, 0, efp::ReceiverMode::RUN_TO_COMPLETION);
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

For static analysis with clang-tidy, see `use_clang_tidy.txt` for recommended configurations and project-specific suppressions.

### Static Analysis

The project passes clang-tidy with the recommended "Quick Code Quality Check" configuration:

```bash
/opt/homebrew/Cellar/llvm/21.1.7/bin/clang-tidy <file>.cpp \
    --checks='modernize-use-auto,misc-const-correctness,bugprone-*,performance-*,readability-braces-around-statements,readability-use-std-min-max,-bugprone-easily-swappable-parameters' \
    -- -std=c++20 -I/path/to/efp
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

## Changelog

### v1.1.0 - Template-Based Callbacks

**Breaking Changes:**
- **Callbacks are now required at construction time** — Removed `setCallback()` method; callbacks must be passed to constructor
- **Template-based callback types** — Sender and Receiver are now templated on the callback type for zero-overhead function calls
- **`std::span` for send callbacks** — Send callback signature changed from `(const uint8_t*, size_t, uint8_t)` to `(std::span<const uint8_t>, uint8_t)`

**New Features:**
- **`makeSender()` factory function** — Creates Sender with automatic callback type deduction
- **`makeReceiver()` factory function** — Creates Receiver with automatic callback type deduction
- **`[[no_unique_address]]`** — Empty callback types don't take space
- **Zero-overhead callbacks** — Template-based callbacks are inlined by compiler (no std::function overhead)

**Migration Guide:**
```cpp
// Old API (removed):
efp::Sender lSender(1400);
lSender.setCallback([](const uint8_t* data, size_t size, uint8_t stream) { ... });

// New API:
auto lSender = efp::makeSender(1400, [](std::span<const uint8_t> data, uint8_t stream) { ... });
```

### Previous Changes

- **Improved**: Test data integrity verification - tests now fill payloads with distinctive patterns and verify content arrives correctly:
  - `test_lifecycle.cpp`: "Stop and restart sender" now verifies sequential byte content
  - `test_integration.cpp`: "Send and receive multiple frames sequentially" now verifies payload size and content per frame
  - `test_edge_cases.cpp`: "All 256 stream IDs work" now fills each stream's payload with its stream ID and verifies on receive
  - `test_c_api.cpp`: "New API test" now has proper loopback and content verification
- **Fixed**: Critical bug in `recalculateSuperFrameNo()` where signed int16 subtraction could overflow at frame 32768, causing frames 32768-65535+ to be rejected as "too old" when using buffer sizes ≥32768
- **Fixed**: Test name "Send 100000 superframes" now correctly reflects it sends 50000 superframes
- **Fixed**: Test "Send 1000000 small frames (endurance)" now actually sends 1 million frames as intended
- **Added**: `pendingCount()` method to Receiver for debugging/diagnostics

## Credits

Anders Cedronius
