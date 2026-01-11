//
// EFP Unit Tests - Bandwidth Manager
//
// Tests for BandwidthManager class that provides congestion-aware streaming
// with per-stream bandwidth controls.
//

#include <doctest/doctest.h>

#include "bandwidth_manager.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <chrono>
#include <thread>

constexpr uint16_t MTU = 1456;

TEST_SUITE("Bandwidth Manager") {

    // =========================================================================
    // Basic instantiation and configuration
    // =========================================================================
    TEST_CASE("BandwidthManager can be instantiated") {
        std::vector<std::vector<uint8_t>> lSentPackets;
        std::vector<std::pair<uint8_t, float>> lBandwidthChanges;
        std::vector<std::pair<uint8_t, bool>> lDropChanges;

        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [&](std::span<const uint8_t> aData, uint8_t) {
                lSentPackets.emplace_back(aData.begin(), aData.end());
            },
            [&](uint8_t aStreamId, float aMultiplier) {
                lBandwidthChanges.emplace_back(aStreamId, aMultiplier);
            },
            [&](uint8_t aStreamId, bool aDropped) {
                lDropChanges.emplace_back(aStreamId, aDropped);
            }
        );

        CHECK(lBwManager.version() == efp::VERSION);
        CHECK(lBwManager.getNetworkHealth() == efp::NetworkHealth::HEALTHY);
    }

    TEST_CASE("Per-stream configuration") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        efp::StreamBandwidthConfig lVideoConfig;
        lVideoConfig.mMinMultiplier = 0.3f;
        lVideoConfig.mMaxMultiplier = 1.0f;
        lVideoConfig.mDropOnSevereCongestion = true;
        lVideoConfig.mPriority = 100;

        lBwManager.setStreamConfig(1, lVideoConfig);

        auto lRetrieved = lBwManager.getStreamConfig(1);
        CHECK(lRetrieved.mMinMultiplier == doctest::Approx(0.3f));
        CHECK(lRetrieved.mMaxMultiplier == doctest::Approx(1.0f));
        CHECK(lRetrieved.mDropOnSevereCongestion == true);
        CHECK(lRetrieved.mPriority == 100);
    }

    TEST_CASE("Default stream multiplier is 1.0") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        CHECK(lBwManager.getCurrentMultiplier(0) == doctest::Approx(1.0f));
        CHECK(lBwManager.getCurrentMultiplier(1) == doctest::Approx(1.0f));
        CHECK(lBwManager.getCurrentMultiplier(255) == doctest::Approx(1.0f));
    }

    TEST_CASE("Stream is not dropped by default") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        CHECK(lBwManager.isStreamDropped(0) == false);
        CHECK(lBwManager.isStreamDropped(1) == false);
        CHECK(lBwManager.isStreamDropped(255) == false);
    }

    // =========================================================================
    // Send functionality
    // =========================================================================
    TEST_CASE("Send passes through to underlying sender") {
        std::vector<std::vector<uint8_t>> lSentPackets;

        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [&](std::span<const uint8_t> aData, uint8_t) {
                lSentPackets.emplace_back(aData.begin(), aData.end());
            },
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        std::vector<uint8_t> lData(100);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lBwManager.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);
        CHECK(lSentPackets.size() == 1);
    }

    TEST_CASE("Send with vector overload") {
        std::vector<std::vector<uint8_t>> lSentPackets;

        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [&](std::span<const uint8_t> aData, uint8_t) {
                lSentPackets.emplace_back(aData.begin(), aData.end());
            },
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        std::vector<uint8_t> lData(100, 0xAB);

        auto lResult = lBwManager.send(lData, 0x02, 2000, 1900, 43, 2);
        CHECK(lResult == efp::Result::OK);
        CHECK(lSentPackets.size() == 1);
    }

    // =========================================================================
    // NACK processing
    // =========================================================================
    TEST_CASE("NACK processing updates NACK count") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {},
            efp::SubFragmentMode::SINGLE,
            1000  // Enable retention
        );

        // Send a frame first to populate retention buffer
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 4;
        std::vector<uint8_t> lData(FRAME_SIZE);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lBwManager.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);

        // Build a NACK
        std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack) + sizeof(efp::NackEntry));

        efp::FrameType0Nack lNackHeader;
        lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);
        lNackHeader.mSubtype = (uint8_t)(efp::Type0Subtype::NACK);
        lNackHeader.mNackCount = 1;

        efp::NackEntry lNackEntry;
        lNackEntry.mStreamId = 1;
        lNackEntry.mSuperFrameNo = 0;
        lNackEntry.mFragmentNo = 1;
        lNackEntry.mFragmentCount = 0;

        std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));
        std::memcpy(lNackData.data() + sizeof(lNackHeader), &lNackEntry, sizeof(lNackEntry));

        lResult = lBwManager.receiveNack(std::span<const uint8_t>(lNackData));
        CHECK(lResult == efp::Result::OK);

        auto lStats = lBwManager.getSenderStatistics();
        CHECK(lStats.mNacksReceived == 1);
    }

    // =========================================================================
    // RTT probe structures
    // =========================================================================
    TEST_CASE("RTT probe structure sizes") {
        CHECK(sizeof(efp::FrameType0RttProbe) == 12);
        CHECK(sizeof(efp::FrameType0RttResponse) == 20);
    }

    TEST_CASE("Type0Subtype RTT enum values") {
        CHECK((uint8_t)(efp::Type0Subtype::RTT_PROBE) == 0x02);
        CHECK((uint8_t)(efp::Type0Subtype::RTT_RESPONSE) == 0x03);
    }

    TEST_CASE("Build RTT probe") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        auto lProbeData = lBwManager.buildRttProbe();
        CHECK(lProbeData.size() == sizeof(efp::FrameType0RttProbe));

        auto* lpProbe = (const efp::FrameType0RttProbe*)(lProbeData.data());
        CHECK(efp::getFrameType(lpProbe->mFrameType) == efp::FrameType::TYPE0);
        CHECK(lpProbe->mSubtype == (uint8_t)(efp::Type0Subtype::RTT_PROBE));
        CHECK(lpProbe->mSequenceNo == 0);
        CHECK(lpProbe->mTimestampUs > 0);

        // Second probe should have incremented sequence number
        auto lProbeData2 = lBwManager.buildRttProbe();
        auto* lpProbe2 = (const efp::FrameType0RttProbe*)(lProbeData2.data());
        CHECK(lpProbe2->mSequenceNo == 1);
    }

    TEST_CASE("Build RTT response from probe") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        auto lProbeData = lBwManager.buildRttProbe();
        auto lResponseData = efp::BandwidthManager<
            decltype([](std::span<const uint8_t>, uint8_t) {}),
            decltype([](uint8_t, float) {}),
            decltype([](uint8_t, bool) {})
        >::buildRttResponse(std::span<const uint8_t>(lProbeData));

        CHECK(lResponseData.size() == sizeof(efp::FrameType0RttResponse));

        auto* lpResponse = (const efp::FrameType0RttResponse*)(lResponseData.data());
        CHECK(efp::getFrameType(lpResponse->mFrameType) == efp::FrameType::TYPE0);
        CHECK(lpResponse->mSubtype == (uint8_t)(efp::Type0Subtype::RTT_RESPONSE));

        auto* lpProbe = (const efp::FrameType0RttProbe*)(lProbeData.data());
        CHECK(lpResponse->mSequenceNo == lpProbe->mSequenceNo);
        CHECK(lpResponse->mTimestampUs == lpProbe->mTimestampUs);
        CHECK(lpResponse->mReceiverTimeUs > 0);
    }

    TEST_CASE("Process RTT response updates RTT estimate") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        auto lProbeData = lBwManager.buildRttProbe();

        // Simulate network delay
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        auto lResponseData = efp::BandwidthManager<
            decltype([](std::span<const uint8_t>, uint8_t) {}),
            decltype([](uint8_t, float) {}),
            decltype([](uint8_t, bool) {})
        >::buildRttResponse(std::span<const uint8_t>(lProbeData));

        auto lResult = lBwManager.processRttResponse(std::span<const uint8_t>(lResponseData));
        CHECK(lResult == efp::Result::OK);

        auto lStats = lBwManager.getStatistics();
        CHECK(lStats.mProbesSent == 1);
        CHECK(lStats.mProbesReceived == 1);
        CHECK(lStats.mSmoothedRttUs > 0);
    }

    // =========================================================================
    // Manual bandwidth control
    // =========================================================================
    TEST_CASE("Manual multiplier overrides automatic") {
        std::vector<std::pair<uint8_t, float>> lBandwidthChanges;

        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [&](uint8_t aStreamId, float aMultiplier) {
                lBandwidthChanges.emplace_back(aStreamId, aMultiplier);
            },
            [](uint8_t, bool) {}
        );

        lBwManager.setManualMultiplier(1, 0.5f);
        CHECK(lBwManager.getCurrentMultiplier(1) == doctest::Approx(0.5f));

        // Verify callback was invoked
        CHECK(lBandwidthChanges.size() == 1);
        CHECK(lBandwidthChanges[0].first == 1);
        CHECK(lBandwidthChanges[0].second == doctest::Approx(0.5f));
    }

    TEST_CASE("Clear manual multiplier returns to automatic") {
        std::vector<std::pair<uint8_t, float>> lBandwidthChanges;

        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [&](uint8_t aStreamId, float aMultiplier) {
                lBandwidthChanges.emplace_back(aStreamId, aMultiplier);
            },
            [](uint8_t, bool) {}
        );

        lBwManager.setManualMultiplier(1, 0.5f);
        CHECK(lBwManager.getCurrentMultiplier(1) == doctest::Approx(0.5f));

        lBwManager.clearManualMultiplier(1);
        // Should return to 1.0 (healthy network)
        CHECK(lBwManager.getCurrentMultiplier(1) == doctest::Approx(1.0f));
    }

    TEST_CASE("Manual multiplier is clamped to stream config") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        efp::StreamBandwidthConfig lConfig;
        lConfig.mMinMultiplier = 0.3f;
        lConfig.mMaxMultiplier = 0.8f;
        lBwManager.setStreamConfig(1, lConfig);

        // Try to set below minimum
        lBwManager.setManualMultiplier(1, 0.1f);
        CHECK(lBwManager.getCurrentMultiplier(1) == doctest::Approx(0.3f));

        // Try to set above maximum
        lBwManager.setManualMultiplier(1, 1.0f);
        CHECK(lBwManager.getCurrentMultiplier(1) == doctest::Approx(0.8f));
    }

    // =========================================================================
    // Statistics
    // =========================================================================
    TEST_CASE("Statistics track probe counts") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        auto lStatsBefore = lBwManager.getStatistics();
        CHECK(lStatsBefore.mProbesSent == 0);
        CHECK(lStatsBefore.mProbesReceived == 0);

        // Send a probe
        auto lProbeData = lBwManager.buildRttProbe();

        auto lStatsAfter = lBwManager.getStatistics();
        CHECK(lStatsAfter.mProbesSent == 1);
    }

    TEST_CASE("Statistics track bandwidth changes") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        auto lStatsBefore = lBwManager.getStatistics();
        CHECK(lStatsBefore.mBandwidthReductions == 0);
        CHECK(lStatsBefore.mBandwidthIncreases == 0);
        CHECK(lStatsBefore.mCurrentHealth == efp::NetworkHealth::HEALTHY);
    }

    // =========================================================================
    // Jitter and health updates
    // =========================================================================
    TEST_CASE("Jitter updates affect health evaluation") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        // Initial health should be healthy
        CHECK(lBwManager.getNetworkHealth() == efp::NetworkHealth::HEALTHY);

        // Simulate high jitter samples to trigger health change
        for (int lI = 0; lI < 100; lI++) {
            lBwManager.updateJitter(60000);  // 60ms jitter (above degraded threshold)
        }

        // Update to evaluate health
        lBwManager.update();

        auto lStats = lBwManager.getStatistics();
        CHECK(lStats.mSmoothedJitterUs > 0);
    }

    // =========================================================================
    // Integration with Sender
    // =========================================================================
    TEST_CASE("Access to underlying sender") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {},
            efp::SubFragmentMode::SINGLE,
            1000  // Enable retention
        );

        auto& lSender = lBwManager.getSender();

        // Send through underlying sender directly
        std::vector<uint8_t> lData(100);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lSender.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);
    }

    TEST_CASE("Sender statistics accessible") {
        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {}
        );

        std::vector<uint8_t> lData(100);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lBwManager.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);

        auto lStats = lBwManager.getSenderStatistics();
        CHECK(lStats.mFragmentsSent == 1);
    }

    // =========================================================================
    // Configuration validation
    // =========================================================================
    TEST_CASE("Custom BandwidthManagerConfig") {
        efp::BandwidthManagerConfig lConfig;
        lConfig.mNackRateDegraded = 3.0f;
        lConfig.mNackRateCongested = 10.0f;
        lConfig.mNackRateSevere = 25.0f;
        lConfig.mJitterDegradedUs = 30000;
        lConfig.mJitterCongestedUs = 80000;
        lConfig.mProbeIntervalMs = 50;
        lConfig.mRecoveryWindowMs = 500;
        lConfig.mAdditiveIncrease = 0.05f;
        lConfig.mMultiplicativeDecrease = 0.7f;

        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [](uint8_t, float) {},
            [](uint8_t, bool) {},
            efp::SubFragmentMode::SINGLE,
            1000,
            50 * 1024 * 1024,
            lConfig
        );

        // Should instantiate without error
        CHECK(lBwManager.version() == efp::VERSION);
    }

    // =========================================================================
    // Example configurations from proposal
    // =========================================================================
    TEST_CASE("Video and audio stream configuration example") {
        std::vector<std::pair<uint8_t, float>> lBandwidthChanges;

        auto lBwManager = efp::makeBandwidthManager(
            MTU,
            [](std::span<const uint8_t>, uint8_t) {},
            [&](uint8_t aStreamId, float aMultiplier) {
                lBandwidthChanges.emplace_back(aStreamId, aMultiplier);
            },
            [](uint8_t, bool) {}
        );

        constexpr uint8_t VIDEO_STREAM_ID = 1;
        constexpr uint8_t AUDIO_STREAM_ID = 2;

        // Video: can reduce to 30%, drop if severe
        efp::StreamBandwidthConfig lVideoConfig;
        lVideoConfig.mMinMultiplier = 0.3f;
        lVideoConfig.mMaxMultiplier = 1.0f;
        lVideoConfig.mDropOnSevereCongestion = true;
        lVideoConfig.mPriority = 100;

        // Audio: fixed bandwidth, never drop
        efp::StreamBandwidthConfig lAudioConfig;
        lAudioConfig.mMinMultiplier = 1.0f;
        lAudioConfig.mMaxMultiplier = 1.0f;
        lAudioConfig.mDropOnSevereCongestion = false;
        lAudioConfig.mPriority = 200;

        lBwManager.setStreamConfig(VIDEO_STREAM_ID, lVideoConfig);
        lBwManager.setStreamConfig(AUDIO_STREAM_ID, lAudioConfig);

        auto lVideoRetrieved = lBwManager.getStreamConfig(VIDEO_STREAM_ID);
        auto lAudioRetrieved = lBwManager.getStreamConfig(AUDIO_STREAM_ID);

        CHECK(lVideoRetrieved.mMinMultiplier == doctest::Approx(0.3f));
        CHECK(lVideoRetrieved.mDropOnSevereCongestion == true);
        CHECK(lAudioRetrieved.mMinMultiplier == doctest::Approx(1.0f));
        CHECK(lAudioRetrieved.mDropOnSevereCongestion == false);
    }

}  // TEST_SUITE

