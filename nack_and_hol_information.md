# NACK and HOL Timeout: Configuration Guide

This document describes the NACK (Negative Acknowledgment) and HOL (Head-of-Line) blocking timeout mechanisms in the EFP receiver, including configuration options, behavior, best practices, and comparisons to other protocols.

## Table of Contents

1. [Overview](#overview)
2. [Configuration Parameters](#configuration-parameters)
3. [Behavior and Timing](#behavior-and-timing)
4. [Recommended Settings](#recommended-settings)
5. [Tips and Tricks](#tips-and-tricks)
6. [Capabilities and Limitations](#capabilities-and-limitations)
7. [Comparison with RIST and SRT](#comparison-with-rist-and-srt)

---

## Overview

EFP implements a **receiver-driven NACK mechanism** for requesting retransmission of missing fragments. When the receiver detects gaps in received fragments, it sends NACK messages back to the sender, which can then retransmit the missing data from its retention buffer.

Key features:
- **Adaptive timing**: NACK intervals automatically adjust based on network jitter
- **Exponential backoff**: Each retry waits longer to avoid congestion
- **HOL timeout**: Prevents older incomplete frames from blocking newer complete frames indefinitely
- **Coalesced NACKs**: Multiple missing fragments are batched into single NACK messages

---

## Configuration Parameters

### Receiver Constructor Parameters

```cpp
Receiver(ReceiveCallbackT aCallback,
         NackCallbackT aNackCallback,
         uint32_t aTimeoutMs = 100,        // Frame timeout
         uint32_t aHolTimeoutMs = 0,       // HOL blocking timeout (0 = disabled)
         uint8_t aMaxNackRetries = 3,      // Max NACK attempts per frame
         uint32_t aNackIntervalMs = 0,     // NACK interval (0 = adaptive)
         ReceiverMode aMode = ReceiverMode::THREADED);
```

### Parameter Details

| Parameter | Default | Description |
|-----------|---------|-------------|
| `aTimeoutMs` | 100 | Maximum time to wait for a frame to complete before delivering as broken |
| `aHolTimeoutMs` | 0 | HOL timeout - deliver incomplete frames after this time if blocking newer frames (0 = disabled) |
| `aMaxNackRetries` | 3 | Maximum number of NACK attempts per incomplete frame (0 = disable NACKs) |
| `aNackIntervalMs` | 0 | Fixed NACK interval in milliseconds (0 = adaptive based on jitter) |

### Sender Retention Buffer

For NACKs to work, the sender must retain sent fragments:

```cpp
auto sender = efp::makeSender(
    1456,                           // MTU
    sendCallback,
    efp::SubFragmentMode::SINGLE,
    1000                            // Retention time in ms
);
```

The retention time should be at least `2 * RTT + processingTime` to ensure fragments are still available when NACK arrives.

---

## Behavior and Timing

### NACK Generation Flow

```
Fragment received with gap detected
         │
         ▼
   Wait for NACK delay
   (adaptive or fixed)
         │
         ▼
    Build NACK message
   (coalesce consecutive gaps)
         │
         ▼
   Send via NackCallback
         │
         ▼
   Increment retry count
         │
         ▼
   Apply exponential backoff
   for next retry
```

### Adaptive NACK Timing

When `aNackIntervalMs = 0` (default), NACK timing adapts to network conditions:

1. **Jitter estimation**: Uses RFC 3550 algorithm to track inter-arrival variance
2. **Base delay**: `max(10ms, 4 × jitter estimate)`
3. **Backoff**: Each retry doubles the delay (`delay << retryCount`)
4. **Cap**: Total delay never exceeds remaining time before frame timeout

### HOL Timeout Behavior

Without HOL timeout:
- Frames are delivered in strict order
- One missing fragment blocks all subsequent frames
- Can cause unbounded latency

With HOL timeout enabled:
- Incomplete frames are delivered as "broken" after HOL timeout
- Allows newer complete frames to proceed
- Trades completeness for latency

### Timing Diagram

```
Time ─────────────────────────────────────────────────────►

Frame arrives (incomplete)
│
├─── NACK delay (adaptive) ───┤
│                             NACK #1 sent
│                             │
│                             ├─── backoff ───┤
│                             │               NACK #2 sent
│                             │               │
│                             │               ├─── backoff×2 ───┤
│                             │               │                  NACK #3 sent
│                             │               │                  │
├──────────────────────────── HOL timeout ─────────────────────┤
│                                                              Frame delivered (broken)
│
├─────────────────────────────────────── Frame timeout ────────────────────────────────┤
                                                                                       Frame delivered (broken)
```

---

## Recommended Settings

### Low Latency Live Streaming

```cpp
auto receiver = efp::makeReceiver(
    receiveCallback,
    nackCallback,
    50,     // 50ms frame timeout
    20,     // 20ms HOL timeout
    2,      // 2 NACK retries
    0       // Adaptive timing
);
```

- **Use case**: Live video, gaming, real-time communication
- **Trade-off**: May deliver some broken frames but maintains low latency

### Reliable File Transfer

```cpp
auto receiver = efp::makeReceiver(
    receiveCallback,
    nackCallback,
    5000,   // 5 second frame timeout
    0,      // No HOL timeout (strict ordering)
    10,     // 10 NACK retries
    100     // 100ms fixed NACK interval
);
```

- **Use case**: File transfer, reliable data delivery
- **Trade-off**: Higher latency but better recovery

### Moderate Quality Video

```cpp
auto receiver = efp::makeReceiver(
    receiveCallback,
    nackCallback,
    200,    // 200ms frame timeout
    80,     // 80ms HOL timeout
    3,      // 3 NACK retries
    0       // Adaptive timing
);
```

- **Use case**: Standard video streaming
- **Trade-off**: Good balance of quality and latency

### High Jitter Networks (Satellite, Mobile)

```cpp
auto receiver = efp::makeReceiver(
    receiveCallback,
    nackCallback,
    500,    // 500ms frame timeout
    200,    // 200ms HOL timeout
    5,      // 5 NACK retries
    50      // 50ms fixed interval (override adaptive)
);
```

- **Use case**: Satellite links, mobile networks with high jitter
- **Trade-off**: Higher latency tolerance, more recovery attempts

---

## Tips and Tricks

### 1. Matching Sender Retention to Receiver Timeout

**Rule of thumb**: Sender retention ≥ Frame timeout + RTT

```cpp
// If frame timeout is 200ms and RTT is 100ms:
auto sender = efp::makeSender(MTU, callback, mode, 400); // 400ms retention
```

### 2. Disabling NACKs for One-Way Links

If there's no return path for NACKs:

```cpp
auto receiver = efp::makeReceiver(
    receiveCallback,
    [](std::span<const uint8_t>) {}, // Empty NACK callback
    100,
    50,
    0     // Zero retries = NACKs disabled
);
```

### 3. Tuning for Known RTT

If you know the network RTT, set NACK interval slightly higher:

```cpp
// For 30ms RTT network:
uint32_t nackInterval = rtt * 1.5;  // 45ms
```

### 4. Monitoring Statistics

Check receiver statistics to tune parameters:

```cpp
auto stats = receiver.getStatistics();
// If mNacksSent is high but mBrokenFrames is also high,
// increase frame timeout or NACK retries
// If mCompleteFrames >> mBrokenFrames, settings are good
```

### 5. Multi-Path Delivery

When using multiple network paths (bonding):
- Use adaptive NACK timing (it handles varying delays)
- Set HOL timeout higher to allow out-of-order arrival
- Consider disabling strict ordering if paths have very different latencies

### 6. Frame Size Considerations

Large frames need more time:
- Increase timeout proportionally to typical frame size
- Very large frames (>1MB) may need 500ms+ timeout

### 7. Avoiding NACK Storms

In high-loss scenarios:
- Limit `aMaxNackRetries` to prevent excessive retransmit requests
- Use HOL timeout to prevent unbounded waiting
- Consider forward error correction (FEC) as complement

---

## Capabilities and Limitations

### What You CAN Do

| Capability | Description |
|------------|-------------|
| ✅ Recover from packet loss | NACKs request retransmission of missing fragments |
| ✅ Handle out-of-order delivery | Fragments can arrive in any order within timeout |
| ✅ Multi-path delivery | Works with bonded/multi-link networks |
| ✅ Adaptive to network conditions | Jitter tracking adjusts NACK timing |
| ✅ Coalesce NACK requests | Multiple gaps combined into single message |
| ✅ Graceful degradation | HOL timeout delivers partial data vs blocking |
| ✅ Mix reliable and unreliable | Per-frame timeout and retry control |
| ✅ Zero-copy receive | Data delivered in-place |

### What You CANNOT Do

| Limitation | Explanation |
|------------|-------------|
| ❌ Guarantee delivery | If sender retention expires or max retries exceeded, data is lost |
| ❌ Recover without return path | NACKs require bidirectional communication |
| ❌ Instant recovery | RTT delay between NACK and retransmit |
| ❌ Recover after sender restart | Retention buffer is lost |
| ❌ Infinite retry | Max retries and timeouts limit recovery attempts |
| ❌ In-band FEC | No forward error correction (FEC) built-in |
| ❌ Congestion control | No automatic bitrate adaptation |
| ❌ Encryption | Security must be added at transport layer |

### Edge Cases to Consider

1. **Burst loss**: If entire burst is lost, many NACKs sent simultaneously
2. **Sender overload**: Too many retransmit requests can overwhelm sender
3. **Clock drift**: Long-running sessions may need clock synchronization
4. **Memory pressure**: Large retention buffers consume sender memory

---

## Comparison with RIST and SRT

### Protocol Overview

| Feature | EFP | RIST | SRT |
|---------|-----|------|-----|
| **Primary Use** | Media framing/fragmentation | Reliable media transport | Reliable media transport |
| **Layer** | Application | Transport (over UDP) | Transport (over UDP) |
| **Packet Loss Recovery** | NACK-based | NACK-based | ARQ (NACK-based) |
| **Encryption** | No (add at transport) | Yes (DTLS) | Yes (AES) |
| **Congestion Control** | No | Optional | Yes |
| **FEC** | No | Optional | No |
| **Bonding** | Application handles | Yes (Simple Profile) | Yes (Connection Bonding) |

### Detailed Comparison

#### NACK/ARQ Mechanism

| Aspect | EFP | RIST | SRT |
|--------|-----|------|-----|
| NACK timing | Adaptive (jitter-based) or fixed | RTT-based | Latency-based |
| Retry limit | Configurable (default 3) | Time-limited | Latency-limited |
| NACK batching | Yes (coalesced ranges) | Yes | Yes |
| HOL blocking | Configurable timeout | Buffer-based | Latency buffer |

#### Latency Model

**EFP**:
- Latency = Frame timeout (configurable)
- HOL timeout provides upper bound
- Very flexible, application controls trade-offs

**RIST**:
- Latency = Sender buffer + network + receiver buffer
- Typically 200ms - 2s for reliable delivery
- More structured, less application control

**SRT**:
- Latency = Configured latency parameter (must exceed RTT)
- Minimum ~20ms, typical 120ms - 500ms
- Packets too late are dropped

#### Recovery Capability

| Scenario | EFP | RIST | SRT |
|----------|-----|------|-----|
| Single packet loss | ✅ Full recovery | ✅ Full recovery | ✅ Full recovery |
| Burst loss (10 packets) | ✅ If within timeout | ✅ If within buffer | ✅ If within latency |
| Network outage (1s) | ⚠️ Depends on timeout | ✅ With large buffer | ✅ With high latency setting |
| Path failover | ⚠️ Application handles | ✅ Built-in (Main+Backup) | ✅ Built-in |

#### When to Use Each

**Choose EFP when:**
- You need efficient superframe fragmentation/reassembly
- You want fine-grained control over recovery behavior
- You're building a custom streaming solution
- You want minimal overhead and maximum flexibility
- Integration with existing UDP transport

**Choose RIST when:**
- You need interoperability (open standard)
- You want built-in encryption (DTLS)
- You need FEC in addition to ARQ
- Multiple receiver/sender bonding is required
- Contribution/distribution workflows

**Choose SRT when:**
- You need proven, widely-deployed solution
- Strong encryption is required (AES-128/256)
- Firewall traversal is needed (rendezvous mode)
- You want automatic congestion control
- Single-stream point-to-point delivery

### Code Comparison

#### EFP NACK Setup

```cpp
auto receiver = efp::makeReceiver(
    [](efp::SuperFramePtr frame) { /* process frame */ },
    [&sender](std::span<const uint8_t> nack) {
        // Route NACK to sender
        sendToSender(nack);
    },
    200,   // frame timeout
    80,    // HOL timeout
    3,     // retries
    0      // adaptive interval
);
```

#### RIST Setup (Conceptual)

```c
rist_receiver_config config = {
    .recovery_mode = RIST_RECOVERY_MODE_TIME,
    .recovery_maxbitrate = 100000,
    .recovery_length_min = 50,
    .recovery_length_max = 500
};
rist_receiver_create(&ctx, &config);
```

#### SRT Setup (Conceptual)

```c
SRTSOCKET sock = srt_create_socket();
int latency_ms = 200;
srt_setsockflag(sock, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));
srt_setsockflag(sock, SRTO_PASSPHRASE, passphrase, strlen(passphrase));
```

### Summary Table

| Criteria | EFP | RIST | SRT |
|----------|:---:|:----:|:---:|
| Ease of integration | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐⭐ |
| Recovery flexibility | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐ |
| Built-in security | ❌ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ |
| Interoperability | ⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ |
| Overhead | ⭐⭐⭐⭐⭐ (minimal) | ⭐⭐⭐ | ⭐⭐⭐ |
| Congestion control | ❌ | ⭐⭐⭐ | ⭐⭐⭐⭐⭐ |
| FEC support | ❌ | ⭐⭐⭐⭐ | ❌ |
| Documentation | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ |

---

## Conclusion

EFP's NACK and HOL timeout mechanism provides a flexible, lightweight approach to handling packet loss in media streaming applications. While it doesn't include built-in encryption or congestion control like RIST or SRT, it offers:

1. **Maximum flexibility** - Application controls all timing and recovery parameters
2. **Minimal overhead** - No extra protocol layers or handshaking
3. **Easy integration** - Header-only C++20, template-based callbacks
4. **Adaptive behavior** - Jitter-based timing adjusts to network conditions

For applications that need a lightweight framing layer with optional reliability, EFP is an excellent choice. For full transport-layer solutions with encryption and congestion control, consider RIST or SRT as complements or alternatives.

