//
// EFP Unit Tests - Bundle (Type4) and Sub-fragmentation
//
// Tests for Type4 bundle frames and sub-fragment modes
//

#include <doctest/doctest.h>

#include "efp.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <chrono>
#include <thread>

constexpr uint16_t MTU = 1456;

template<typename Predicate>
bool waitFor(Predicate aPred, std::chrono::milliseconds aTimeout = std::chrono::milliseconds(500)) {
    auto lStart = std::chrono::steady_clock::now();
    while (!aPred()) {
        if (std::chrono::steady_clock::now() - lStart > aTimeout) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

TEST_SUITE("Bundle (Type4)") {

    // =========================================================================
    // Test SubFragmentMode::SINGLE (default) produces no bundles
    // =========================================================================
    TEST_CASE("SubFragmentMode::SINGLE sends individual fragments") {
        std::atomic<size_t> lPacketCount{0};
        std::atomic<size_t> lBundleCount{0};

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lPacketCount++;
            // Check if it's a Type4 bundle
            if (!aData.empty() && efp::getFrameType(aData[0]) == efp::FrameType::TYPE4) {
                lBundleCount++;
            }
        }, efp::SubFragmentMode::SINGLE);

        // Send data that requires multiple fragments
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 4 + 100;
        std::vector<uint8_t> lData(FRAME_SIZE);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lSender.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);

        // Should have sent multiple individual packets, no bundles
        CHECK(lPacketCount > 1);
        CHECK(lBundleCount == 0);
    }

    // =========================================================================
    // Test SubFragmentMode::HALF bundles 2 fragments per packet
    // =========================================================================
    TEST_CASE("SubFragmentMode::HALF bundles 2 fragments per packet") {
        std::atomic<size_t> lBundleCount{0};
        std::vector<size_t> lFrameCounts;

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            if (!aData.empty() && efp::getFrameType(aData[0]) == efp::FrameType::TYPE4) {
                lBundleCount++;
                // Parse frame count from Type4 header
                if (aData.size() >= 2) {
                    lFrameCounts.push_back(aData[1]);
                }
            }
        }, efp::SubFragmentMode::HALF);

        // Send data that requires 4 fragments (should result in 2 bundles of 2)
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 4;
        std::vector<uint8_t> lData(FRAME_SIZE);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lSender.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);

        CHECK(lBundleCount >= 1);
        for (auto lCount : lFrameCounts) {
            CHECK(lCount <= 2);
        }
    }

    // =========================================================================
    // Test SubFragmentMode::QUARTER bundles 4 fragments per packet
    // =========================================================================
    TEST_CASE("SubFragmentMode::QUARTER bundles 4 fragments per packet") {
        std::atomic<size_t> lBundleCount{0};
        std::vector<size_t> lFrameCounts;

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            if (!aData.empty() && efp::getFrameType(aData[0]) == efp::FrameType::TYPE4) {
                lBundleCount++;
                if (aData.size() >= 2) {
                    lFrameCounts.push_back(aData[1]);
                }
            }
        }, efp::SubFragmentMode::QUARTER);

        // Send data that requires 8 fragments (should result in 2 bundles of 4)
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 8;
        std::vector<uint8_t> lData(FRAME_SIZE);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lSender.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);

        CHECK(lBundleCount >= 1);
        for (auto lCount : lFrameCounts) {
            CHECK(lCount <= 4);
        }
    }

    // =========================================================================
    // Test SubFragmentMode::EIGHTH bundles 8 fragments per packet
    // =========================================================================
    TEST_CASE("SubFragmentMode::EIGHTH bundles 8 fragments per packet") {
        std::atomic<size_t> lBundleCount{0};
        std::vector<size_t> lFrameCounts;

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            if (!aData.empty() && efp::getFrameType(aData[0]) == efp::FrameType::TYPE4) {
                lBundleCount++;
                if (aData.size() >= 2) {
                    lFrameCounts.push_back(aData[1]);
                }
            }
        }, efp::SubFragmentMode::EIGHTH);

        // Send data that requires 16 fragments (should result in 2 bundles of 8)
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 16;
        std::vector<uint8_t> lData(FRAME_SIZE);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lSender.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);

        CHECK(lBundleCount >= 1);
        for (auto lCount : lFrameCounts) {
            CHECK(lCount <= 8);
        }
    }

    // =========================================================================
    // Test Type4 bundle is correctly received and unwrapped
    // =========================================================================
    TEST_CASE("Type4 bundle is correctly received and unwrapped") {
        std::atomic<bool> lDataReceived{false};
        std::atomic<size_t> lBundlesSent{0};

        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 4 + 100;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mSize == FRAME_SIZE);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mPts == 1000);
            CHECK(apFrame->mPayloadCode == 42);

            // Verify data integrity
            for (size_t lI = 0; lI < apFrame->mSize; lI++) {
                CHECK(apFrame->mpData[lI] == (uint8_t)(lI & 0xFF));
            }
            lDataReceived = true;
        }, [](std::span<const uint8_t>) {}, 100);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            if (!aData.empty() && efp::getFrameType(aData[0]) == efp::FrameType::TYPE4) {
                lBundlesSent++;
            }
            auto lResult = lReceiver.receive(aData, 0);
            // Allow OK or DUPLICATE (from retransmit testing)
            CHECK((lResult == efp::Result::OK || lResult == efp::Result::DUPLICATE_FRAGMENT));
        }, efp::SubFragmentMode::QUARTER);

        std::vector<uint8_t> lData(FRAME_SIZE);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lSender.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
        CHECK(lBundlesSent > 0);
    }

    // =========================================================================
    // Test small frame with bundle mode still works (falls back to Type2)
    // =========================================================================
    TEST_CASE("Small frame with bundle mode uses Type2") {
        std::atomic<bool> lDataReceived{false};
        std::atomic<size_t> lType2Count{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mSize == 100);
            CHECK(!apFrame->mBroken);
            lDataReceived = true;
        }, [](std::span<const uint8_t>) {}, 100);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            if (!aData.empty() && efp::getFrameType(aData[0]) == efp::FrameType::TYPE2) {
                lType2Count++;
            }
            lReceiver.receive(aData, 0);
        }, efp::SubFragmentMode::QUARTER);

        std::vector<uint8_t> lData(100);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lSender.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
        CHECK(lType2Count == 1);
    }

    // =========================================================================
    // Test statistics are updated for bundles
    // =========================================================================
    TEST_CASE("Statistics track bundle counts") {
        auto lReceiver = efp::makeReceiver([](efp::SuperFramePtr) {}, [](std::span<const uint8_t>) {}, 100);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        }, efp::SubFragmentMode::QUARTER);

        // Send multiple frames
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 8;
        std::vector<uint8_t> lData(FRAME_SIZE);

        for (int lI = 0; lI < 5; lI++) {
            (void)lSender.send(lData, 0x01, 1000, 900, 42, 1);
        }

        auto lSenderStats = lSender.getStatistics();
        CHECK(lSenderStats.mBundlesSent > 0);
        CHECK(lSenderStats.mFragmentsSent > 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        auto lReceiverStats = lReceiver.getStatistics();
        CHECK(lReceiverStats.mBundlesReceived > 0);
        CHECK(lReceiverStats.mFragmentsReceived > 0);
    }

    // =========================================================================
    // Test retention buffer stores fragments when enabled
    // =========================================================================
    TEST_CASE("Retention buffer stores fragments when enabled") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t>, uint8_t) {},
                                        efp::SubFragmentMode::SINGLE,
                                        1000,  // 1 second retention
                                        10 * 1024 * 1024);  // 10MB max

        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 4;
        std::vector<uint8_t> lData(FRAME_SIZE);

        (void)lSender.send(lData, 0x01, 1000, 900, 42, 1);

        auto lStats = lSender.getStatistics();
        CHECK(lStats.mRetentionBufferFragments > 0);
        CHECK(lStats.mRetentionBufferBytes > 0);
    }

    // =========================================================================
    // Test retention buffer is empty when disabled
    // =========================================================================
    TEST_CASE("Retention buffer is empty when disabled") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t>, uint8_t) {},
                                        efp::SubFragmentMode::SINGLE,
                                        0);  // 0 = disabled

        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 4;
        std::vector<uint8_t> lData(FRAME_SIZE);

        (void)lSender.send(lData, 0x01, 1000, 900, 42, 1);

        auto lStats = lSender.getStatistics();
        CHECK(lStats.mRetentionBufferFragments == 0);
        CHECK(lStats.mRetentionBufferBytes == 0);
    }

}

