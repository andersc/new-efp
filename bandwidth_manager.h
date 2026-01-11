//
// Elastic Frame Protocol - Bandwidth Manager
// Copyright 2024-2026
//
// Provides congestion-aware streaming with per-stream bandwidth controls.
// Uses composition wrapper pattern around existing EFP classes with
// hybrid congestion detection (delay-based + NACK-based) and RTT probing.
//

#ifndef BANDWIDTH_MANAGER_H
#define BANDWIDTH_MANAGER_H

#include "efp.h"
#include "efp_internal.h"

#include <cstdint>
#include <map>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <functional>

namespace efp {

//------------------------------------------------------------------------------
// Network health states for congestion detection
//------------------------------------------------------------------------------
enum class NetworkHealth : uint8_t {
    HEALTHY   = 0,  // Normal operation, full bandwidth
    DEGRADED  = 1,  // Minor congestion detected, 30% reduction
    CONGESTED = 2,  // Significant congestion, 50% reduction
    SEVERE    = 3   // Critical congestion, minimum bandwidth or drop
};

//------------------------------------------------------------------------------
// Per-stream bandwidth configuration
//------------------------------------------------------------------------------
struct StreamBandwidthConfig {
    float mMinMultiplier = 0.0f;           // Minimum bandwidth (0.0 = can go to 0%)
    float mMaxMultiplier = 1.0f;           // Maximum bandwidth (1.0 = 100%)
    bool  mDropOnSevereCongestion = false; // Pause stream if SEVERE
    uint8_t mPriority = 128;               // Higher = more important (audio > video)
};

//------------------------------------------------------------------------------
// Bandwidth manager configuration
//------------------------------------------------------------------------------
struct BandwidthManagerConfig {
    // Detection thresholds (NACKs per second)
    float mNackRateDegraded = 5.0f;      // NACKs/sec for DEGRADED
    float mNackRateCongested = 15.0f;    // NACKs/sec for CONGESTED
    float mNackRateSevere = 30.0f;       // NACKs/sec for SEVERE

    // Jitter thresholds (microseconds)
    int64_t mJitterDegradedUs = 50000;   // 50ms jitter → DEGRADED
    int64_t mJitterCongestedUs = 100000; // 100ms jitter → CONGESTED

    // RTT probing
    uint32_t mProbeIntervalMs = 100;     // Probe every 100ms in recovery
    uint32_t mRecoveryWindowMs = 1000;   // Stable for 1s before increasing
    uint8_t  mMaxProbeFailures = 3;      // Failures before aborting recovery

    // Update interval
    uint32_t mUpdateIntervalMs = 50;     // Evaluate health every 50ms

    // AIMD parameters
    float mAdditiveIncrease = 0.1f;      // +10% per recovery window
    float mMultiplicativeDecrease = 0.5f; // ×0.5 on congestion
};

//------------------------------------------------------------------------------
// Bandwidth manager statistics
//------------------------------------------------------------------------------
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

//------------------------------------------------------------------------------
// C++20 concepts for callback validation
//------------------------------------------------------------------------------
template<typename T>
concept BandwidthChangeCallbackConcept =
    std::invocable<T, uint8_t /*streamId*/, float /*newMultiplier*/>;

template<typename T>
concept StreamDropCallbackConcept =
    std::invocable<T, uint8_t /*streamId*/, bool /*dropped*/>;

//------------------------------------------------------------------------------
// BandwidthManager: Wraps Sender and monitors network health
// Provides per-stream bandwidth control with congestion-aware adaptation
//------------------------------------------------------------------------------
template<typename SendCallbackT,
         typename BandwidthChangeCallbackT,
         typename StreamDropCallbackT,
         uint16_t BUFFER_SIZE = DEFAULT_BUFFER_SIZE>
    requires ValidBufferSize<BUFFER_SIZE> &&
             SendCallbackConcept<SendCallbackT> &&
             BandwidthChangeCallbackConcept<BandwidthChangeCallbackT> &&
             StreamDropCallbackConcept<StreamDropCallbackT>
class BandwidthManager {
public:
    explicit BandwidthManager(uint16_t aMtu,
                              SendCallbackT aSendCallback,
                              BandwidthChangeCallbackT aBandwidthCallback,
                              StreamDropCallbackT aDropCallback,
                              SubFragmentMode aSubFragmentMode = SubFragmentMode::SINGLE,
                              uint32_t aRetentionMs = 1000,
                              size_t aRetentionMaxBytes = 50 * 1024 * 1024,
                              BandwidthManagerConfig aConfig = {})
        : mSender(aMtu, std::move(aSendCallback), aSubFragmentMode, aRetentionMs, aRetentionMaxBytes),
          mBandwidthCallback(std::move(aBandwidthCallback)),
          mDropCallback(std::move(aDropCallback)),
          mConfig(aConfig),
          mMtu(aMtu) {

        // Initialize all streams to default config
        for (size_t lI = 0; lI < 256; lI++) {
            mStreamConfigs[lI] = StreamBandwidthConfig{};
            mCurrentMultipliers[lI] = 1.0f;
            mStreamDropped[lI] = false;
            mManualMultiplier[lI] = -1.0f;  // -1 means not set
        }
    }

    ~BandwidthManager() = default;

    // Non-copyable, non-movable
    BandwidthManager(const BandwidthManager&) = delete;
    BandwidthManager& operator=(const BandwidthManager&) = delete;
    BandwidthManager(BandwidthManager&&) = delete;
    BandwidthManager& operator=(BandwidthManager&&) = delete;

    // Get version
    [[nodiscard]] static consteval uint16_t version() noexcept { return VERSION; }

    //--------------------------------------------------------------------------
    // Per-stream configuration
    //--------------------------------------------------------------------------

    void setStreamConfig(uint8_t aStreamId, StreamBandwidthConfig aConfig) {
        std::lock_guard<std::mutex> lLock(mMutex);
        mStreamConfigs[aStreamId] = aConfig;
        // Recalculate multiplier if needed
        updateStreamMultiplier(aStreamId);
    }

    [[nodiscard]] StreamBandwidthConfig getStreamConfig(uint8_t aStreamId) const {
        std::lock_guard<std::mutex> lLock(mMutex);
        return mStreamConfigs[aStreamId];
    }

    //--------------------------------------------------------------------------
    // Query current state
    //--------------------------------------------------------------------------

    [[nodiscard]] float getCurrentMultiplier(uint8_t aStreamId) const {
        std::lock_guard<std::mutex> lLock(mMutex);
        return mCurrentMultipliers[aStreamId];
    }

    [[nodiscard]] bool isStreamDropped(uint8_t aStreamId) const {
        std::lock_guard<std::mutex> lLock(mMutex);
        return mStreamDropped[aStreamId];
    }

    [[nodiscard]] NetworkHealth getNetworkHealth() const {
        std::lock_guard<std::mutex> lLock(mMutex);
        return mCurrentHealth;
    }

    [[nodiscard]] BandwidthManagerStatistics getStatistics() const {
        std::lock_guard<std::mutex> lLock(mMutex);
        BandwidthManagerStatistics lStats;
        lStats.mCurrentHealth = mCurrentHealth;
        lStats.mSmoothedNackRate = mSmoothedNackRate;
        lStats.mSmoothedJitterUs = mSmoothedJitterUs;
        lStats.mSmoothedRttUs = mSmoothedRttUs;
        lStats.mProbesSent = mProbesSent;
        lStats.mProbesReceived = mProbesReceived;
        lStats.mBandwidthReductions = mBandwidthReductions;
        lStats.mBandwidthIncreases = mBandwidthIncreases;
        lStats.mStreamsDropped = mStreamsDropped;
        return lStats;
    }

    [[nodiscard]] SenderStatistics getSenderStatistics() const {
        return mSender.getStatistics();
    }

    //--------------------------------------------------------------------------
    // Send data (wraps efp::Sender::send)
    //--------------------------------------------------------------------------

    [[nodiscard]] Result send(std::span<const uint8_t> aData,
                              uint8_t aPayloadType, uint64_t aPts, uint64_t aDts,
                              uint32_t aPayloadCode, uint8_t aStreamId, uint8_t aFlags = Flags::NONE) {

        std::lock_guard<std::mutex> lLock(mMutex);

        // Check if stream is dropped
        if (mStreamDropped[aStreamId]) [[unlikely]] {
            return Result::OK;  // Silently drop
        }

        return mSender.send(aData, aPayloadType, aPts, aDts, aPayloadCode, aStreamId, aFlags);
    }

    // Convenience overload for vector
    [[nodiscard]] Result send(const std::vector<uint8_t>& aData,
                              uint8_t aPayloadType, uint64_t aPts, uint64_t aDts,
                              uint32_t aPayloadCode, uint8_t aStreamId, uint8_t aFlags = Flags::NONE) {
        return send(std::span<const uint8_t>(aData), aPayloadType, aPts, aDts,
                    aPayloadCode, aStreamId, aFlags);
    }

    //--------------------------------------------------------------------------
    // Process NACK from receiver (wraps efp::Sender::receiveNack)
    //--------------------------------------------------------------------------

    [[nodiscard]] Result receiveNack(std::span<const uint8_t> aData) {
        auto lResult = mSender.receiveNack(aData);

        if (lResult == Result::OK) {
            std::lock_guard<std::mutex> lLock(mMutex);
            mRecentNackCount++;
        }

        return lResult;
    }

    //--------------------------------------------------------------------------
    // Process retransmits (wraps efp::Sender::processRetransmits)
    //--------------------------------------------------------------------------

    [[nodiscard]] size_t processRetransmits(size_t aMaxCount = SIZE_MAX) {
        return mSender.processRetransmits(aMaxCount);
    }

    //--------------------------------------------------------------------------
    // Periodic update (call from main loop or timer)
    //--------------------------------------------------------------------------

    void update() {
        std::lock_guard<std::mutex> lLock(mMutex);

        auto lNow = nowUs();

        // Check if enough time has passed
        if (lNow - mLastUpdateUs < (int64_t)(mConfig.mUpdateIntervalMs) * 1000) {
            return;
        }

        auto lElapsedMs = (float)(lNow - mLastUpdateUs) / 1000.0f;
        mLastUpdateUs = lNow;

        // Calculate NACK rate (NACKs per second)
        auto lNackRate = (float)(mRecentNackCount) / (lElapsedMs / 1000.0f);
        mRecentNackCount = 0;

        // Update smoothed NACK rate (exponential moving average)
        mSmoothedNackRate = mSmoothedNackRate * 0.7f + lNackRate * 0.3f;

        // Evaluate network health
        auto lNewHealth = evaluateHealth();

        if (lNewHealth != mCurrentHealth) {
            handleHealthChange(lNewHealth);
        }

        // Process RTT probing in recovery states
        if (mCurrentHealth != NetworkHealth::HEALTHY) {
            processProbing(lNow);
        }
    }

    //--------------------------------------------------------------------------
    // Update jitter estimate (call when receiving packets)
    //--------------------------------------------------------------------------

    void updateJitter(int64_t aJitterUs) {
        std::lock_guard<std::mutex> lLock(mMutex);
        // Exponential moving average: J(i) = J(i-1) + (|D(i)| - J(i-1)) / 16
        auto lDeviation = (aJitterUs < 0) ? -aJitterUs : aJitterUs;
        mSmoothedJitterUs += (lDeviation - mSmoothedJitterUs) / 16;
    }

    //--------------------------------------------------------------------------
    // Update RTT estimate (call when receiving RTT probe responses)
    //--------------------------------------------------------------------------

    void updateRtt(int64_t aRttUs) {
        std::lock_guard<std::mutex> lLock(mMutex);
        // Exponential moving average for RTT
        mSmoothedRttUs += (aRttUs - mSmoothedRttUs) / 8;
        mProbesReceived++;
        mProbeFailures = 0;  // Reset failure count on success

        // Check if RTT is stable (within 20% of baseline)
        if (mBaselineRttUs == 0) {
            mBaselineRttUs = mSmoothedRttUs;
        } else {
            auto lRatio = (float)(mSmoothedRttUs) / (float)(mBaselineRttUs);
            if (lRatio >= 0.8f && lRatio <= 1.2f) {
                mStableRttSamples++;
            } else {
                mStableRttSamples = 0;
            }
        }
    }

    //--------------------------------------------------------------------------
    // Send RTT probe (returns probe data to send)
    //--------------------------------------------------------------------------

    [[nodiscard]] std::vector<uint8_t> buildRttProbe() {
        std::lock_guard<std::mutex> lLock(mMutex);

        std::vector<uint8_t> lProbeData(sizeof(FrameType0RttProbe));

        FrameType0RttProbe lProbe;
        lProbe.mFrameType = makeFrameTypeByte(FrameType::TYPE0, 0);
        lProbe.mSubtype = (uint8_t)(Type0Subtype::RTT_PROBE);
        lProbe.mSequenceNo = mProbeSequenceNo++;
        lProbe.mTimestampUs = (uint64_t)(nowUs());

        std::memcpy(lProbeData.data(), &lProbe, sizeof(lProbe));

        mProbesSent++;

        return lProbeData;
    }

    //--------------------------------------------------------------------------
    // Build RTT response from probe (returns response data to send)
    //--------------------------------------------------------------------------

    [[nodiscard]] static std::vector<uint8_t> buildRttResponse(std::span<const uint8_t> aProbeData) {
        if (aProbeData.size() < sizeof(FrameType0RttProbe)) {
            return {};
        }

        auto* lpProbe = (const FrameType0RttProbe*)(aProbeData.data());

        if (getFrameType(lpProbe->mFrameType) != FrameType::TYPE0) {
            return {};
        }
        if (lpProbe->mSubtype != (uint8_t)(Type0Subtype::RTT_PROBE)) {
            return {};
        }

        std::vector<uint8_t> lResponseData(sizeof(FrameType0RttResponse));

        FrameType0RttResponse lResponse;
        lResponse.mFrameType = makeFrameTypeByte(FrameType::TYPE0, 0);
        lResponse.mSubtype = (uint8_t)(Type0Subtype::RTT_RESPONSE);
        lResponse.mSequenceNo = lpProbe->mSequenceNo;
        lResponse.mTimestampUs = lpProbe->mTimestampUs;
        lResponse.mReceiverTimeUs = (uint64_t)(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

        std::memcpy(lResponseData.data(), &lResponse, sizeof(lResponse));

        return lResponseData;
    }

    //--------------------------------------------------------------------------
    // Process RTT response (extracts RTT and updates estimate)
    //--------------------------------------------------------------------------

    [[nodiscard]] Result processRttResponse(std::span<const uint8_t> aData) {
        if (aData.size() < sizeof(FrameType0RttResponse)) {
            return Result::FRAME_SIZE_MISMATCH;
        }

        auto* lpResponse = (const FrameType0RttResponse*)(aData.data());

        if (getFrameType(lpResponse->mFrameType) != FrameType::TYPE0) {
            return Result::INVALID_PARAMETER;
        }
        if (lpResponse->mSubtype != (uint8_t)(Type0Subtype::RTT_RESPONSE)) {
            return Result::INVALID_PARAMETER;
        }

        auto lNow = nowUs();
        auto lRtt = lNow - (int64_t)(lpResponse->mTimestampUs);

        if (lRtt > 0 && lRtt < 30000000) {  // Sanity check: RTT < 30 seconds
            updateRtt(lRtt);
        }

        return Result::OK;
    }

    //--------------------------------------------------------------------------
    // Manual bandwidth adjustment (override automatic)
    //--------------------------------------------------------------------------

    void setManualMultiplier(uint8_t aStreamId, float aMultiplier) {
        std::lock_guard<std::mutex> lLock(mMutex);
        mManualMultiplier[aStreamId] = std::clamp(aMultiplier, 0.0f, 1.0f);
        updateStreamMultiplier(aStreamId);
    }

    void clearManualMultiplier(uint8_t aStreamId) {
        std::lock_guard<std::mutex> lLock(mMutex);
        mManualMultiplier[aStreamId] = -1.0f;  // -1 means not set
        updateStreamMultiplier(aStreamId);
    }

    //--------------------------------------------------------------------------
    // Access to underlying sender (for advanced use)
    //--------------------------------------------------------------------------

    [[nodiscard]] Sender<SendCallbackT, BUFFER_SIZE>& getSender() {
        return mSender;
    }

private:
    [[nodiscard]] int64_t nowUs() const noexcept {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    [[nodiscard]] NetworkHealth evaluateHealth() const {
        // Evaluate NACK-based health
        auto lNackHealth = evaluateNackHealth();

        // Evaluate delay-based health
        auto lDelayHealth = evaluateDelayHealth();

        // Take the worse of the two
        return (lNackHealth > lDelayHealth) ? lNackHealth : lDelayHealth;
    }

    [[nodiscard]] NetworkHealth evaluateNackHealth() const {
        if (mSmoothedNackRate >= mConfig.mNackRateSevere) {
            return NetworkHealth::SEVERE;
        }
        if (mSmoothedNackRate >= mConfig.mNackRateCongested) {
            return NetworkHealth::CONGESTED;
        }
        if (mSmoothedNackRate >= mConfig.mNackRateDegraded) {
            return NetworkHealth::DEGRADED;
        }
        return NetworkHealth::HEALTHY;
    }

    [[nodiscard]] NetworkHealth evaluateDelayHealth() const {
        if (mSmoothedJitterUs >= mConfig.mJitterCongestedUs) {
            return NetworkHealth::CONGESTED;
        }
        if (mSmoothedJitterUs >= mConfig.mJitterDegradedUs) {
            return NetworkHealth::DEGRADED;
        }
        return NetworkHealth::HEALTHY;
    }

    void handleHealthChange(NetworkHealth aNewHealth) {
        auto lOldHealth = mCurrentHealth;
        mCurrentHealth = aNewHealth;

        // Multiplicative decrease on worsening health
        if (aNewHealth > lOldHealth) {
            mBandwidthReductions++;

            // Apply AIMD multiplicative decrease
            for (size_t lI = 0; lI < 256; lI++) {
                if (mManualMultiplier[lI] < 0) {  // Only if not manually set
                    auto lTarget = getTargetMultiplier((uint8_t)(lI), aNewHealth);
                    if (lTarget < mCurrentMultipliers[lI]) {
                        mCurrentMultipliers[lI] = lTarget;
                        notifyBandwidthChange((uint8_t)(lI));
                    }

                    // Check for stream dropping
                    if (aNewHealth == NetworkHealth::SEVERE &&
                        mStreamConfigs[lI].mDropOnSevereCongestion &&
                        !mStreamDropped[lI]) {
                        mStreamDropped[lI] = true;
                        mStreamsDropped++;
                        mDropCallback((uint8_t)(lI), true);
                    }
                }
            }
        }

        // Reset recovery tracking on health change
        mStableRttSamples = 0;
        mRecoveryStartUs = nowUs();
    }

    void processProbing(int64_t aNow) {
        // Check if it's time to try recovery
        if (aNow - mRecoveryStartUs < (int64_t)(mConfig.mRecoveryWindowMs) * 1000) {
            return;
        }

        // Check if RTT has been stable
        auto lRequiredSamples = mConfig.mRecoveryWindowMs / mConfig.mProbeIntervalMs;
        if (mStableRttSamples >= lRequiredSamples) {
            // Additive increase
            tryBandwidthIncrease();
        }
    }

    void tryBandwidthIncrease() {
        auto lImproved = false;

        // Try to improve health state
        if (mCurrentHealth == NetworkHealth::SEVERE) {
            mCurrentHealth = NetworkHealth::CONGESTED;
            lImproved = true;
        } else if (mCurrentHealth == NetworkHealth::CONGESTED) {
            mCurrentHealth = NetworkHealth::DEGRADED;
            lImproved = true;
        } else if (mCurrentHealth == NetworkHealth::DEGRADED) {
            mCurrentHealth = NetworkHealth::HEALTHY;
            lImproved = true;
        }

        if (lImproved) {
            mBandwidthIncreases++;

            // Apply additive increase to all streams
            for (size_t lI = 0; lI < 256; lI++) {
                if (mManualMultiplier[lI] < 0) {  // Only if not manually set
                    auto lTarget = getTargetMultiplier((uint8_t)(lI), mCurrentHealth);
                    auto lNewMult = mCurrentMultipliers[lI] + mConfig.mAdditiveIncrease;
                    lNewMult = std::min(lNewMult, lTarget);
                    lNewMult = std::clamp(lNewMult, mStreamConfigs[lI].mMinMultiplier,
                                          mStreamConfigs[lI].mMaxMultiplier);

                    if (lNewMult != mCurrentMultipliers[lI]) {
                        mCurrentMultipliers[lI] = lNewMult;
                        notifyBandwidthChange((uint8_t)(lI));
                    }

                    // Un-drop stream if not severe
                    if (mStreamDropped[lI] && mCurrentHealth != NetworkHealth::SEVERE) {
                        mStreamDropped[lI] = false;
                        mDropCallback((uint8_t)(lI), false);
                    }
                }
            }

            // Reset recovery tracking
            mStableRttSamples = 0;
            mRecoveryStartUs = nowUs();
        }
    }

    [[nodiscard]] float getTargetMultiplier(uint8_t aStreamId, NetworkHealth aHealth) const {
        auto& lConfig = mStreamConfigs[aStreamId];
        float lTarget;

        switch (aHealth) {
            case NetworkHealth::HEALTHY:
                lTarget = 1.0f;
                break;
            case NetworkHealth::DEGRADED:
                lTarget = 0.7f;
                break;
            case NetworkHealth::CONGESTED:
                lTarget = 0.5f;
                break;
            case NetworkHealth::SEVERE:
                lTarget = lConfig.mMinMultiplier;
                break;
        }

        // Clamp to stream's configured range
        return std::clamp(lTarget, lConfig.mMinMultiplier, lConfig.mMaxMultiplier);
    }

    void updateStreamMultiplier(uint8_t aStreamId) {
        // If manual multiplier is set, use it
        if (mManualMultiplier[aStreamId] >= 0) {
            auto lNewMult = std::clamp(mManualMultiplier[aStreamId],
                                       mStreamConfigs[aStreamId].mMinMultiplier,
                                       mStreamConfigs[aStreamId].mMaxMultiplier);
            if (lNewMult != mCurrentMultipliers[aStreamId]) {
                mCurrentMultipliers[aStreamId] = lNewMult;
                notifyBandwidthChange(aStreamId);
            }
            return;
        }

        // Otherwise calculate based on health
        auto lTarget = getTargetMultiplier(aStreamId, mCurrentHealth);
        if (lTarget != mCurrentMultipliers[aStreamId]) {
            mCurrentMultipliers[aStreamId] = lTarget;
            notifyBandwidthChange(aStreamId);
        }
    }

    void notifyBandwidthChange(uint8_t aStreamId) {
        mBandwidthCallback(aStreamId, mCurrentMultipliers[aStreamId]);
    }

    // Sender (composition, not inheritance)
    Sender<SendCallbackT, BUFFER_SIZE> mSender;

    // Callbacks
    BandwidthChangeCallbackT mBandwidthCallback;
    StreamDropCallbackT mDropCallback;

    // Configuration
    BandwidthManagerConfig mConfig;
    uint16_t mMtu;

    // Per-stream state
    StreamBandwidthConfig mStreamConfigs[256];
    float mCurrentMultipliers[256];
    bool mStreamDropped[256];
    float mManualMultiplier[256];  // -1 means not set

    // Network health state
    NetworkHealth mCurrentHealth = NetworkHealth::HEALTHY;

    // Smoothed metrics
    float mSmoothedNackRate = 0.0f;
    int64_t mSmoothedJitterUs = 0;
    int64_t mSmoothedRttUs = 0;

    // Timing
    int64_t mLastUpdateUs = 0;
    int64_t mRecoveryStartUs = 0;

    // NACK tracking
    uint32_t mRecentNackCount = 0;

    // RTT probing
    uint16_t mProbeSequenceNo = 0;
    uint64_t mProbesSent = 0;
    uint64_t mProbesReceived = 0;
    uint8_t mProbeFailures = 0;
    int64_t mBaselineRttUs = 0;
    uint32_t mStableRttSamples = 0;

    // Statistics
    uint64_t mBandwidthReductions = 0;
    uint64_t mBandwidthIncreases = 0;
    uint64_t mStreamsDropped = 0;

    // Thread safety
    mutable std::mutex mMutex;
};

// Factory function for easier instantiation
template<typename SendCallbackT,
         typename BandwidthChangeCallbackT,
         typename StreamDropCallbackT,
         uint16_t BUFFER_SIZE = DEFAULT_BUFFER_SIZE>
    requires SendCallbackConcept<SendCallbackT> &&
             BandwidthChangeCallbackConcept<BandwidthChangeCallbackT> &&
             StreamDropCallbackConcept<StreamDropCallbackT>
[[nodiscard]] auto makeBandwidthManager(
    uint16_t aMtu,
    SendCallbackT aSendCallback,
    BandwidthChangeCallbackT aBandwidthCallback,
    StreamDropCallbackT aDropCallback,
    SubFragmentMode aSubFragmentMode = SubFragmentMode::SINGLE,
    uint32_t aRetentionMs = 1000,
    size_t aRetentionMaxBytes = 50 * 1024 * 1024,
    BandwidthManagerConfig aConfig = {}) {

    return BandwidthManager<SendCallbackT, BandwidthChangeCallbackT,
                            StreamDropCallbackT, BUFFER_SIZE>(
        aMtu, std::move(aSendCallback), std::move(aBandwidthCallback),
        std::move(aDropCallback), aSubFragmentMode, aRetentionMs,
        aRetentionMaxBytes, aConfig);
}

} // namespace efp

#endif // BANDWIDTH_MANAGER_H

