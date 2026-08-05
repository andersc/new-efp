# Elastic Frame Protocol (EFP)

[![Linux Build](https://github.com/andersc/new-efp/actions/workflows/linux.yml/badge.svg)](https://github.com/andersc/new-efp/actions/workflows/linux.yml)
[![macOS Build](https://github.com/andersc/new-efp/actions/workflows/macos.yml/badge.svg)](https://github.com/andersc/new-efp/actions/workflows/macos.yml)
[![Windows Build](https://github.com/andersc/new-efp/actions/workflows/windows.yml/badge.svg)](https://github.com/andersc/new-efp/actions/workflows/windows.yml)


A lightweight, header-only C++20 library for fragmenting and reassembling data over unreliable or size-limited transport layers.

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application                              │
│  (Video, Audio, Sensors, Files, RPC, Custom Data)              │
├─────────────────────────────────────────────────────────────────┤
│                     Elastic Frame Protocol                      │
│  • Fragments large data    • Reassembles fragments              │
│  • 64-bit timestamps       • Loss detection                     │
│  • Stream multiplexing     • 8-byte data-fragment header        │
├─────────────────────────────────────────────────────────────────┤
│                      Transport Layer                            │
│  (UDP, SRT, RIST, QUIC, WebRTC, TCP, Custom)                   │
└─────────────────────────────────────────────────────────────────┘
```

## Features

- **Header-only** — Single include, no library linking required
- **Generic** — Works with any data type (media, sensors, files, RPC)
- **Compact framing** — 8-byte Type1 data-fragment headers; 27-byte final metadata header
- **64-bit timestamps** — No wraparound issues (unlike MPEG-TS 33-bit)
- **Transport agnostic** — Works over datagram or record-preserving transports; stream transports need external packet boundaries
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
- **`consteval`** — Compile-time only evaluation for version queries
- **`<bit>` header** — `std::has_single_bit` for power-of-2 validation
- **Template-based callbacks** — Callbacks are template parameters for zero-overhead inlining (no `std::function`)

## Platform Support

EFP requires a **64-bit system** and is tested on:
- **Linux** — GCC and Clang (x86_64, ARM64)
- **macOS** — Apple Clang (x86_64, ARM64)
- **Windows** — MSVC (x64)

> **Note:** 32-bit systems are not supported. The library enforces this with a compile-time static assertion.

## Quick Start

```cpp
#include "efp.h"

int main() {
    // Create receiver with callback (callback is required at construction time)
    auto lReceiver = efp::makeReceiver([](efp::SuperFramePtr apFrame) {
        // Process received data
        process(apFrame->mpData, apFrame->mSize);
    }, [](std::span<const uint8_t> aNack) {
        sendNackBackToSender(aNack);
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
        GIT_REPOSITORY https://github.com/andersc/new-efp.git
        GIT_TAG master
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
    50,   // Early incomplete-frame timeout in ms (0 = disabled)
    3,    // Max NACK retries (0 = disabled)
    0     // NACK interval (0 = adaptive based on jitter)
);
```

Each `Receiver` represents one sender sequence space. When a UDP server accepts
multiple remote endpoints, keep one receiver per endpoint (for example, a
`std::shared_ptr<Receiver>` in that client's `std::any` context). Stream IDs
multiplex streams from one sender; they do not separate independent senders.

#### NACK and Retransmission

The receiver automatically detects missing fragments and sends NACK (Negative Acknowledgment) messages to request retransmission:

- **Adaptive timing**: By default (`aNackIntervalMs=0`), NACK timing adapts to network jitter
- **Exponential backoff**: Each retry waits longer (delay doubles)
- **Retry limit**: No more than `aMaxNackRetries` NACK messages are emitted for a frame
- **Early incomplete-frame timeout**: `aHolTimeoutMs` can deliver any incomplete frame before the full `aTimeoutMs`; complete frames do not wait behind older incomplete frames

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
    50,   // Deliver incomplete frames after 50ms
    3,    // 3 NACK retries
    20    // 20ms fixed NACK interval (or 0 for adaptive)
);
```

**Sender retransmission**: Call `processRetransmits()` after `receiveNack()` to send queued retransmissions immediately. Bundled modes may also interleave an equal-sized retransmit into a later Type4 packet. You can call `processRetransmits(n)` to limit work per call.

**Validation**: The receiver throws `std::invalid_argument` if:
- `aHolTimeoutMs >= aTimeoutMs` (the early timeout must be less than the full frame timeout)
- `aNackIntervalMs * aMaxNackRetries >= aTimeoutMs` (NACK budget must fit in timeout)

#### Bandwidth Management (Optional)

For congestion-aware streaming with per-stream bandwidth controls, include `bandwidth_manager.h`:

```cpp
#include "bandwidth_manager.h"

auto lBwManager = efp::makeBandwidthManager(
    1400,                    // MTU
    sendCallback,            // Send callback (same as makeSender)
    [](uint8_t streamId, float multiplier) {
        // Adjust encoder bitrate: newBitrate = baseBitrate * multiplier
        adjustEncoderBitrate(streamId, multiplier);
    },
    [](uint8_t streamId, bool dropped) {
        // Stream dropped/restored (severe congestion)
        if (dropped) pauseEncoder(streamId);
        else resumeEncoder(streamId);
    },
    efp::SubFragmentMode::SINGLE,
    1000                     // 1 second retention for retransmit
);

// Configure per-stream policies
efp::StreamBandwidthConfig lVideoConfig;
lVideoConfig.mMinMultiplier = 0.3f;        // Can reduce to 30% of base bitrate
lVideoConfig.mMaxMultiplier = 1.0f;        // Max 100%
lVideoConfig.mDropOnSevereCongestion = true;  // Drop video on severe congestion
lVideoConfig.mPriority = 100;              // Lower priority than audio

efp::StreamBandwidthConfig lAudioConfig;
lAudioConfig.mMinMultiplier = 1.0f;        // Fixed bandwidth
lAudioConfig.mDropOnSevereCongestion = false; // Never drop audio
lAudioConfig.mPriority = 200;              // Higher priority

lBwManager.setStreamConfig(VIDEO_STREAM_ID, lVideoConfig);
lBwManager.setStreamConfig(AUDIO_STREAM_ID, lAudioConfig);

// Send data through manager
lBwManager.send(videoFrame, payloadType, pts, dts, code, VIDEO_STREAM_ID);

// Call update() periodically (every ~50ms) to evaluate network health
lBwManager.update();

// Query current state
float multiplier = lBwManager.getCurrentMultiplier(VIDEO_STREAM_ID);
efp::NetworkHealth health = lBwManager.getNetworkHealth();
```

**Network Health States:**
| State | Multiplier | Description |
|-------|------------|-------------|
| HEALTHY | 1.0 | Normal operation, full bandwidth |
| DEGRADED | 0.7 | Minor congestion detected |
| CONGESTED | 0.5 | Significant congestion |
| SEVERE | mMinMultiplier | Critical congestion, may drop streams |

**Congestion Detection**: Uses hybrid approach combining:
- **Delay-based**: Jitter threshold monitoring (proactive, detects before loss)
- **NACK-based**: NACK rate monitoring (reactive, responds to actual loss)

**RTT Probing**: For bandwidth restoration, send RTT probes during recovery:
```cpp
// Sender side: periodically send probe
auto probeData = lBwManager.buildRttProbe();
sendToReceiver(probeData);

// Receiver side: respond to probe
auto responseData = efp::BandwidthManager<...>::buildRttResponse(probeData);
sendToSender(responseData);

// Sender side: process response
lBwManager.processRttResponse(responseData);
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

The total counters and current queue/buffer gauges are populated. The four
`*PerSecond` fields are reserved for a future sliding-window implementation and
currently remain zero.

## Protocol Format

EFP uses 5 frame types optimized for different scenarios:

| Type | Size | Purpose |
|------|------|---------|
| Type0 | 1B or subtype-defined | Signaling: NACK and RTT measurement |
| Type1 | 8B | Fragment header |
| Type2 | 27B | Final fragment with metadata |
| Type3 | 8B | Penultimate overflow fragment |
| Type4 | 2B | Bundle wrapper containing equal-sized Type1 frames |

### Frame Structure

The low four bits of the first byte select the frame type; the high four bits
carry flags. Sizes below are packed wire-header sizes and do not include payload
bytes. Multi-byte fields are currently copied in host byte order, so peers must
use the same byte order (the tested x86_64 and ARM64 targets are little-endian).

```text
Type0 base (1 byte):
┌──────────┐
│ Type+Flg │
│  1 byte  │
└──────────┘

Type0 NACK (3 + 6N bytes):
┌──────────┬──────────┬───────────┬───────────────────────────┐
│ Type+Flg │ Subtype  │ NackCount │ NackEntry × NackCount     │
│  1 byte  │  1 byte  │  1 byte   │ 6 bytes each              │
└──────────┴──────────┴───────────┴───────────────────────────┘

NackEntry (6 bytes):
┌──────────┬──────────────┬────────────┬───────────────┐
│ StreamID │ SuperFrameNo │ FragmentNo │ FragmentCount │
│  1 byte  │   2 bytes    │  2 bytes   │    1 byte     │
└──────────┴──────────────┴────────────┴───────────────┘

Type0 RTT probe (12 bytes):
┌──────────┬──────────┬────────────┬─────────────┐
│ Type+Flg │ Subtype  │ SequenceNo │ TimestampUs │
│  1 byte  │  1 byte  │  2 bytes   │   8 bytes   │
└──────────┴──────────┴────────────┴─────────────┘

Type0 RTT response (20 bytes):
┌──────────┬──────────┬────────────┬─────────────┬────────────────┐
│ Type+Flg │ Subtype  │ SequenceNo │ TimestampUs │ ReceiverTimeUs │
│  1 byte  │  1 byte  │  2 bytes   │   8 bytes   │    8 bytes     │
└──────────┴──────────┴────────────┴─────────────┴────────────────┘
```

```text
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

Type3 (8 bytes):
┌──────────┬──────────┬──────────────┬──────────────┬──────────────┐
│ Type+Flg │ StreamID │ SuperFrameNo │ Type1PktSize │ OfFragmentNo │
│  1 byte  │  1 byte  │   2 bytes    │   2 bytes    │   2 bytes    │
└──────────┴──────────┴──────────────┴──────────────┴──────────────┘

Type4 (2 bytes + equal-sized Type1 frames):
┌──────────┬────────────┬──────────────────────────────────────────┐
│ Type+Flg │ FrameCount │ Type1 frame × FrameCount                 │
│  1 byte  │   1 byte   │ equal-sized; wrapper total ≤ sender MTU │
└──────────┴────────────┴──────────────────────────────────────────┘
```

Type1 carries ordinary non-final fragments. Type2 is both the final fragment
and the complete representation of a small one-packet SuperFrame. Type3 is used
only when data immediately before Type2 cannot fill a regular Type1 fragment.
Type4 bundles only equal-sized Type1 frames; Type2 and Type3 remain standalone.
The sender's `aMtu` is the maximum size passed to its send callback, making it a
natural UDP-payload ceiling. EFP does not add UDP/IP headers.

A SuperFrame is limited to 100 MiB and 8192 fragments. The fragment limit can
become the effective limit first in bundled modes because their Type1 payloads
are intentionally smaller.

EFP expects one complete EFP packet per `receive()` call. UDP already preserves
those boundaries. TCP and other byte-stream transports must add an outer length
prefix or another record-framing mechanism before feeding packets to EFP.

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
auto lNackCallback = [](std::span<const uint8_t> nack) { sendNack(nack); };

// Threaded mode (default): background threads handle timeout and delivery work
auto lReceiver = efp::makeReceiver(callback, lNackCallback,
    100, 0, 3, 0, efp::ReceiverMode::THREADED);

// Run-to-completion: no threads; caller drives timeout and NACK work
auto lReceiver = efp::makeReceiver(callback, lNackCallback,
    100, 0, 3, 0, efp::ReceiverMode::RUN_TO_COMPLETION);
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

## Credits

Anders Cedronius
