# Bandwidth Management Proposal for EFP

## Overview

This document proposes a bandwidth management layer for the Elastic Frame Protocol (EFP) that provides congestion-aware streaming with per-stream bandwidth controls. The implementation uses a **composition wrapper** pattern around existing EFP classes, employing **hybrid congestion detection** (delay-based + NACK-based) and **RTT probing** for bandwidth restoration.

## Table of Contents

1. [Motivation](#motivation)
2. [Architecture](#architecture)
3. [Congestion Detection Algorithm](#congestion-detection-algorithm)
4. [Per-Stream Bandwidth Control](#per-stream-bandwidth-control)
5. [Bandwidth Restoration via RTT Probing](#bandwidth-restoration-via-rtt-probing)
6. [API Reference](#api-reference)
7. [State Machine](#state-machine)
8. [Configuration Guidelines](#configuration-guidelines)
9. [Comparison with TCP Congestion Algorithms](#comparison-with-tcp-congestion-algorithms)
10. [Testing Strategy](#testing-strategy)

---

## Motivation

EFP currently provides reliable delivery via NACK-based ARQ but lacks automatic bitrate adaptation. As noted in `nack_and_hol_information.md`:

> ❌ Congestion control — No automatic bitrate adaptation

This creates problems in real-world deployments:

1. **Network congestion causes packet loss** — Without reducing bitrate, retransmissions compound the problem
2. **Burst loss overwhelms recovery** — Resending packets on an already-failing connection is counterproductive
3. **No proactive detection** — Loss-based detection is reactive; delay increases ~0.6s before loss begins (per NetSim research)
4. **Per-stream priorities ignored** — Audio may be more critical than video, but current system treats all streams equally

### Goals

- Detect congestion **before** significant packet loss occurs (delay-based)
- React quickly to **burst loss** events (NACK-based)
- Allow **per-stream bandwidth policies** (min/max multipliers, drop thresholds)
- **Restore bandwidth** safely when network conditions improve
- Maintain EFP's **zero-overhead callback design** and code style

---

## Architecture

### Composition Pattern

`BandwidthManager` wraps existing `efp::Sender` and `efp::Receiver` via composition rather than inheritance:

```
┌─────────────────────────────────────────────────────────────┐
│                     Application                             │
│  (Video Encoder, Audio Encoder)                            │
├─────────────────────────────────────────────────────────────┤
│                   BandwidthManager                          │
│  • Per-stream config      • Hybrid congestion detection    │
│  • Bandwidth callbacks    • RTT probing                    │
├────────────────────────┬────────────────────────────────────┤
│     efp::Sender        │         efp::Receiver             │
│  • Fragmentation       │      • Reassembly                 │
│  • NACK processing     │      • NACK generation            │
│  • Retention buffer    │      • Jitter tracking            │
├────────────────────────┴────────────────────────────────────┤
│                    Transport (UDP)                          │
└─────────────────────────────────────────────────────────────┘
```

### Benefits of Composition

1. **Optional feature** — Applications can use plain EFP if bandwidth management not needed
2. **No core changes** — EFP classes remain unchanged and stable
3. **Flexible mixing** — Some streams can be managed, others unmanaged
4. **Testable** — BandwidthManager can be tested with mock Sender/Receiver

---

## Congestion Detection Algorithm

### Hybrid Approach

The algorithm combines two detection methods:

#### 1. Delay-Based Detection (Proactive)

Based on research showing delay increases ~0.6 seconds before packet loss:

```
Jitter Trend Analysis:
- Track smoothed jitter estimate (RFC 3550 algorithm)
- Calculate jitter derivative (rate of change)
- If jitter increasing consistently → DEGRADED state
- If jitter > threshold → CONGESTED state

Delay Threshold:
- Monitor one-way delay (requires clock sync) or RTT
- If delay > baseline + margin → early warning
```

#### 2. NACK-Based Detection (Reactive)

Responds to actual packet loss:

```
NACK Rate Monitoring:
- Track NACKs per second from ReceiverStatistics
- Track retransmits per second from SenderStatistics
- Exponential moving average for smoothing

Thresholds:
- NACK rate > degradedThreshold → DEGRADED
- NACK rate > congestedThreshold → CONGESTED
- NACK rate > severeThreshold → SEVERE
```

### Combined Decision

```cpp
NetworkHealth evaluateHealth() {
    auto lDelayHealth = evaluateDelayHealth();    // From jitter/RTT
    auto lNackHealth = evaluateNackHealth();      // From NACK rate
    
    // Take the worse of the two assessments
    return std::max(lDelayHealth, lNackHealth);
}
```

This ensures:
- **Proactive response** to building congestion (delay-based)
- **Rapid response** to sudden burst loss (NACK-based)

---

## Per-Stream Bandwidth Control

### Configuration Structure

```cpp
struct StreamBandwidthConfig {
    float mMinMultiplier = 1.0f;        // Minimum bandwidth (0.5 = 50%)
    float mMaxMultiplier = 1.0f;        // Maximum bandwidth (1.0 = 100%)
    bool  mDropOnSevereCongestion = false;  // Pause stream if SEVERE
    uint8_t mPriority = 128;            // Higher = more important (audio > video)
};
```

### Example Configurations

```cpp
// Video: can reduce to 30%, drop if severe
StreamBandwidthConfig lVideoConfig;
lVideoConfig.mMinMultiplier = 0.3f;
lVideoConfig.mMaxMultiplier = 1.0f;
lVideoConfig.mDropOnSevereCongestion = true;
lVideoConfig.mPriority = 100;

// Audio: fixed bandwidth, never drop
StreamBandwidthConfig lAudioConfig;
lAudioConfig.mMinMultiplier = 1.0f;
lAudioConfig.mMaxMultiplier = 1.0f;
lAudioConfig.mDropOnSevereCongestion = false;
lAudioConfig.mPriority = 200;

lBwManager.setStreamConfig(VIDEO_STREAM_ID, lVideoConfig);
lBwManager.setStreamConfig(AUDIO_STREAM_ID, lAudioConfig);
```

### Bandwidth Reduction Strategy

When congestion detected, reduce lower-priority streams first:

```
1. Sort streams by priority (ascending)
2. For each stream (lowest priority first):
   a. Calculate target multiplier based on health state
   b. Clamp to stream's [min, max] range
   c. If SEVERE and dropOnSevereCongestion → pause stream
   d. Notify via callback
3. Re-evaluate periodically
```

### Multiplier Calculation

| Network Health | Target Multiplier | Notes |
|----------------|-------------------|-------|
| HEALTHY        | 1.0               | Full bandwidth |
| DEGRADED       | 0.7               | 30% reduction |
| CONGESTED      | 0.5               | 50% reduction |
| SEVERE         | mMinMultiplier    | Minimum or drop |

---

## Bandwidth Restoration via RTT Probing

### Protocol Extension

New Type0 subtypes for RTT measurement:

```cpp
enum class Type0Subtype : uint8_t {
    RESERVED     = 0x00,
    NACK         = 0x01,
    RTT_PROBE    = 0x02,  // NEW: Sender → Receiver
    RTT_RESPONSE = 0x03   // NEW: Receiver → Sender
};
```

### Probe Frame Structure

```cpp
struct FrameType0RttProbe {
    uint8_t  mFrameType;      // Type0 with RTT_PROBE subtype
    uint8_t  mSubtype;        // 0x02
    uint16_t mSequenceNo;     // Probe sequence number
    uint64_t mTimestampUs;    // Sender timestamp (microseconds)
};

struct FrameType0RttResponse {
    uint8_t  mFrameType;      // Type0 with RTT_RESPONSE subtype
    uint8_t  mSubtype;        // 0x03
    uint16_t mSequenceNo;     // Echo probe sequence
    uint64_t mTimestampUs;    // Original sender timestamp
    uint64_t mReceiverTimeUs; // Receiver's local time (optional)
};
```

### Restoration Algorithm

```
When in RECOVERY state:
1. Start probe timer (default: 100ms interval)
2. Send RTT_PROBE packet
3. Wait for RTT_RESPONSE
4. If response received:
   - Calculate RTT
   - Update smoothed RTT estimate
   - If RTT stable for mRecoveryWindowMs → increase multiplier
5. If probe lost:
   - Increment loss counter
   - If too many losses → stay in current state
6. Repeat until HEALTHY or timeout
```

### Gradual Restoration

```cpp
// Additive increase (conservative)
if (lProbeSuccess && lRttStable) {
    mCurrentMultiplier += 0.1f;  // +10% per successful window
    mCurrentMultiplier = std::min(mCurrentMultiplier, mConfig.mMaxMultiplier);
}

// Multiplicative decrease (aggressive) - on new congestion
if (lCongestionDetected) {
    mCurrentMultiplier *= 0.5f;  // Halve immediately
    mCurrentMultiplier = std::max(mCurrentMultiplier, mConfig.mMinMultiplier);
}
```

This follows the classic **AIMD** (Additive Increase, Multiplicative Decrease) pattern proven effective in TCP.

---

## API Reference

### BandwidthManager Class

```cpp
namespace efp {

// Callback when bandwidth should change for a stream
// Application should adjust encoder bitrate accordingly
template<typename T>
concept BandwidthChangeCallbackConcept = 
    std::invocable<T, uint8_t /*streamId*/, float /*newMultiplier*/>;

// Callback when a stream should be dropped/paused
template<typename T>
concept StreamDropCallbackConcept = 
    std::invocable<T, uint8_t /*streamId*/, bool /*dropped*/>;

template<typename SendCallbackT, 
         typename BandwidthChangeCallbackT,
         typename StreamDropCallbackT>
class BandwidthManager {
public:
    // Constructor
    BandwidthManager(uint16_t aMtu,
                     SendCallbackT aSendCallback,
                     BandwidthChangeCallbackT aBandwidthCallback,
                     StreamDropCallbackT aDropCallback,
                     BandwidthManagerConfig aConfig = {});
    
    // Per-stream configuration
    void setStreamConfig(uint8_t aStreamId, StreamBandwidthConfig aConfig);
    StreamBandwidthConfig getStreamConfig(uint8_t aStreamId) const;
    
    // Query current state
    float getCurrentMultiplier(uint8_t aStreamId) const;
    bool isStreamDropped(uint8_t aStreamId) const;
    NetworkHealth getNetworkHealth() const;
    BandwidthManagerStatistics getStatistics() const;
    
    // Send data (wraps efp::Sender::send)
    Result send(std::span<const uint8_t> aData,
                uint8_t aPayloadType, uint64_t aPts, uint64_t aDts,
                uint32_t aPayloadCode, uint8_t aStreamId, uint8_t aFlags = 0);
    
    // Receive data (wraps efp::Receiver::receive)
    Result receive(std::span<const uint8_t> aData, uint8_t aSourceId = 0);
    
    // Process NACK from receiver (wraps efp::Sender::receiveNack)
    Result receiveNack(std::span<const uint8_t> aData);
    
    // Periodic update (call from main loop or timer)
    void update();
    
    // Manual bandwidth adjustment (override automatic)
    void setManualMultiplier(uint8_t aStreamId, float aMultiplier);
    void clearManualMultiplier(uint8_t aStreamId);
};

} // namespace efp
```

### Configuration Structures

```cpp
struct BandwidthManagerConfig {
    // Detection thresholds
    float mNackRateDegraded = 5.0f;     // NACKs/sec for DEGRADED
    float mNackRateCongested = 15.0f;   // NACKs/sec for CONGESTED  
    float mNackRateSevere = 30.0f;      // NACKs/sec for SEVERE
    
    // Jitter thresholds (microseconds)
    int64_t mJitterDegradedUs = 50000;  // 50ms jitter → DEGRADED
    int64_t mJitterCongestedUs = 100000; // 100ms jitter → CONGESTED
    
    // RTT probing
    uint32_t mProbeIntervalMs = 100;    // Probe every 100ms in recovery
    uint32_t mRecoveryWindowMs = 1000;  // Stable for 1s before increasing
    uint8_t  mMaxProbeFailures = 3;     // Failures before aborting recovery
    
    // Update interval
    uint32_t mUpdateIntervalMs = 50;    // Evaluate health every 50ms
    
    // AIMD parameters
    float mAdditiveIncrease = 0.1f;     // +10% per recovery window
    float mMultiplicativeDecrease = 0.5f; // ×0.5 on congestion
};

struct StreamBandwidthConfig {
    float mMinMultiplier = 1.0f;
    float mMaxMultiplier = 1.0f;
    bool  mDropOnSevereCongestion = false;
    uint8_t mPriority = 128;
};

struct BandwidthManagerStatistics {
    NetworkHealth mCurrentHealth = NetworkHealth::HEALTHY;
    float mSmoothedNackRate = 0.0f;
    int64_t mSmoothedJitterUs = 0;
    int64_t mSmoothedRttUs = 0;
    uint64_t mProbesSent = 0;
    uint64_t mProbesReceived = 0;
    uint64_t mBandwidthReductions = 0;
    uint64_t mBandwidthIncreases = 0;
    uint64_t mStreamsDropped = 0;
};
```

---

## State Machine

### Network Health States

```
                    ┌─────────────────────────────────────────┐
                    │                                         │
                    ▼                                         │
              ┌──────────┐                                    │
    ┌────────►│ HEALTHY  │◄───────────────────────┐          │
    │         └────┬─────┘                        │          │
    │              │ jitter↑ OR nack_rate > 5     │          │
    │              ▼                              │          │
    │         ┌──────────┐                        │          │
    │         │ DEGRADED │────────────────────────┤          │
    │         └────┬─────┘  RTT probes OK         │          │
    │              │ jitter↑↑ OR nack_rate > 15   │          │
    │              ▼                              │          │
    │         ┌───────────┐                       │          │
    │         │ CONGESTED │───────────────────────┤          │
    │         └────┬──────┘  RTT probes OK        │          │
    │              │ nack_rate > 30               │          │
    │              ▼                              │          │
    │         ┌──────────┐                        │          │
    └─────────│  SEVERE  │────────────────────────┘          │
              └────┬─────┘  RTT probes OK (slow)             │
                   │                                          │
                   │ timeout OR manual reset                  │
                   └──────────────────────────────────────────┘
```

### State Transitions

| From | To | Trigger |
|------|----|---------|
| HEALTHY | DEGRADED | `nackRate > 5` OR `jitter > 50ms` OR `jitterDerivative > 0` for 3 samples |
| DEGRADED | CONGESTED | `nackRate > 15` OR `jitter > 100ms` |
| CONGESTED | SEVERE | `nackRate > 30` |
| SEVERE | CONGESTED | RTT probes succeed for `recoveryWindowMs` |
| CONGESTED | DEGRADED | RTT probes succeed for `recoveryWindowMs` |
| DEGRADED | HEALTHY | RTT probes succeed for `recoveryWindowMs` |
| Any | SEVERE | Burst loss detected (>10% in 100ms window) |

---

## Configuration Guidelines

### Low-Latency Live Streaming

```cpp
BandwidthManagerConfig lConfig;
lConfig.mNackRateDegraded = 3.0f;      // React quickly
lConfig.mNackRateCongested = 10.0f;
lConfig.mProbeIntervalMs = 50;          // Fast probing
lConfig.mRecoveryWindowMs = 500;        // Quick recovery
lConfig.mUpdateIntervalMs = 20;         // Frequent updates

// Video: aggressive reduction
StreamBandwidthConfig lVideo;
lVideo.mMinMultiplier = 0.25f;          // Can go to 25%
lVideo.mDropOnSevereCongestion = true;

// Audio: protected
StreamBandwidthConfig lAudio;
lAudio.mMinMultiplier = 1.0f;
lAudio.mDropOnSevereCongestion = false;
```

### Reliable Video Conferencing

```cpp
BandwidthManagerConfig lConfig;
lConfig.mNackRateDegraded = 5.0f;
lConfig.mNackRateCongested = 20.0f;
lConfig.mRecoveryWindowMs = 2000;       // Conservative recovery

// Video: moderate reduction
StreamBandwidthConfig lVideo;
lVideo.mMinMultiplier = 0.5f;
lVideo.mDropOnSevereCongestion = false; // Never drop

// Screen share: can reduce more
StreamBandwidthConfig lScreen;
lScreen.mMinMultiplier = 0.2f;
lScreen.mDropOnSevereCongestion = true;
lScreen.mPriority = 50;                 // Lower than video
```

### High-Jitter Networks (Mobile/Satellite)

```cpp
BandwidthManagerConfig lConfig;
lConfig.mJitterDegradedUs = 150000;     // 150ms threshold (higher tolerance)
lConfig.mJitterCongestedUs = 300000;    // 300ms threshold
lConfig.mProbeIntervalMs = 200;         // Slower probing
lConfig.mRecoveryWindowMs = 3000;       // Very conservative recovery
lConfig.mMaxProbeFailures = 5;          // More tolerance for probe loss
```

---

## Comparison with TCP Congestion Algorithms

### TCP CUBIC

| Aspect | TCP CUBIC | EFP BandwidthManager |
|--------|-----------|---------------------|
| Detection | Loss-based only | Hybrid (delay + loss) |
| Recovery | Cubic function | Linear (AIMD) |
| Fairness | Competes with other TCP | Application-controlled |
| RTT sensitivity | High | Moderate |

### TCP BBR

| Aspect | TCP BBR | EFP BandwidthManager |
|--------|---------|---------------------|
| Model | Estimates bottleneck BW | Monitors NACK rate |
| Probing | Periodic BW probing | RTT probing |
| Bufferbloat | Resistant | Delay-based detection helps |
| Complexity | High | Moderate |

### Design Rationale

We chose a simpler AIMD approach because:

1. **Application knows encoding constraints** — Unlike TCP, we can't arbitrarily adjust data rate
2. **Per-stream priorities** — TCP treats all data equally
3. **Predictability** — Linear recovery is easier to reason about
4. **Cooperation** — Not competing with other flows for fairness

---

## Testing Strategy

### Unit Tests

1. **State machine transitions** — Verify correct state changes for various inputs
2. **Multiplier calculations** — Test clamping, priority ordering
3. **RTT probe handling** — Test probe/response round-trip
4. **Statistics accuracy** — Verify smoothed estimates

### Integration Tests with NetSim

```cpp
TEST_CASE("Bandwidth reduces before significant loss") {
    NetSimConfig lNetConfig;
    lNetConfig.mNetworkType = NetworkType::WIFI_5GHZ;
    lNetConfig.mQuality = NetworkQuality::GOOD;
    
    NetSim lSim(lNetConfig);
    // ... setup BandwidthManager with sim ...
    
    // Transition to POOR quality
    lNetConfig.mQuality = NetworkQuality::POOR;
    lSim.configure(lNetConfig);
    
    // Send data for 5 seconds
    // Verify bandwidth reduced BEFORE broken frames delivered
    
    // Transition back to GOOD
    lNetConfig.mQuality = NetworkQuality::GOOD;
    lSim.configure(lNetConfig);
    
    // Verify bandwidth gradually restored
}
```

### Scenarios

1. **Gradual degradation** — Quality: GOOD → FAIR → POOR → FAIR → GOOD
2. **Sudden burst loss** — Inject 20% loss for 500ms
3. **Jitter spike** — Increase jitter without loss
4. **Multi-stream priority** — Video + audio, verify audio protected
5. **Stream dropping** — SEVERE congestion with dropOnSevereCongestion

---

## Implementation Checklist

- [ ] `bandwidth_manager.h` — Main class implementation
- [ ] `efp_internal.h` — Add Type0 RTT probe structures
- [ ] `efp.h` — Handle RTT probe/response in Receiver
- [ ] `tests/test_bandwidth_manager.cpp` — Unit tests
- [ ] `tests/test_bandwidth_integration.cpp` — NetSim integration tests
- [ ] Update `README.md` — Document new feature

---

## Conclusion

This proposal provides a comprehensive bandwidth management solution for EFP that:

1. **Detects congestion proactively** using hybrid delay+NACK analysis
2. **Supports per-stream policies** for different media types
3. **Restores bandwidth safely** via RTT probing
4. **Follows EFP design principles** — header-only, zero-overhead callbacks, clean API
5. **Integrates with existing infrastructure** — NetSim for testing, composition for flexibility

The implementation maintains backward compatibility — existing EFP users are unaffected, and bandwidth management is opt-in via the `BandwidthManager` wrapper class.

