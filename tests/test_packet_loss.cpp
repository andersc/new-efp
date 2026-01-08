//
// EFP Unit Tests - Packet Loss and Corruption
//
// Tests for fragment loss, corruption, and timeout handling
// Ported from old UnitTest6, 9, 16, 22, 24
//

#include <doctest/doctest.h>

#include "efp.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <chrono>
#include <thread>
#include <random>

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

TEST_SUITE("Packet Loss") {

    // =========================================================================
    // UnitTest6: Drop first Type1 fragment, verify broken + partial data
    // =========================================================================
    TEST_CASE("Drop first Type1 fragment (UnitTest6)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 2) + 12;

        std::atomic<bool> lDataReceived{false};
        size_t lPacketNumber = 0;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mStreamId == 1);
            CHECK(apFrame->mPts == 1001);
            CHECK(apFrame->mPayloadCode == 2);
            CHECK(apFrame->mBroken);

            CHECK(apFrame->mSize == FRAME_SIZE);

            size_t lType1PayloadSize = MTU - sizeof(efp::FrameType1);
            uint8_t lVectorChecker = (uint8_t)(lType1PayloadSize % 256);
            for (size_t lX = lType1PayloadSize; lX < apFrame->mSize; lX++) {
                CHECK(apFrame->mpData[lX] == lVectorChecker++);
            }
            lDataReceived = true;
        }, [](std::span<const uint8_t>) {}, 50, 20);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lPacketNumber++;
            if (lPacketNumber == 1) {
                return;  // Drop the first packet
            }
            auto lResult = lReceiver.receive(aData, 0);
            CHECK(lResult == efp::Result::OK);
        });

        std::vector<uint8_t> lMydata(FRAME_SIZE);
        std::generate(lMydata.begin(), lMydata.end(), [lN = 0]() mutable { return (uint8_t)(lN++); });

        auto lResult = lSender.send(lMydata, 0x02, 1001, 1, 2, 1);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest9: Drop Type2 packet, verify PTS=MAX, broken=true
    // =========================================================================
    TEST_CASE("Drop Type2 packet (UnitTest9)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        std::atomic<bool> lDataReceived{false};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mStreamId == 1);
            CHECK(apFrame->mPts == UINT64_MAX);
            CHECK(apFrame->mPayloadCode == UINT32_MAX);
            CHECK(apFrame->mBroken);

            size_t lType1PayloadSize = MTU - sizeof(efp::FrameType1);
            CHECK(apFrame->mSize == lType1PayloadSize * 6);

            uint8_t lVectorChecker = 0;
            for (size_t lX = 0; lX < lType1PayloadSize * 5; lX++) {
                CHECK(apFrame->mpData[lX] == lVectorChecker++);
            }
            lDataReceived = true;
        }, [](std::span<const uint8_t>) {}, 50, 20);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            if ((aData[0] & 0x0f) == (uint8_t)(efp::FrameType::TYPE2)) {
                return;  // Drop the Type2 packet
            }
            auto lResult = lReceiver.receive(aData, 0);
            CHECK(lResult == efp::Result::OK);
        });

        std::vector<uint8_t> lMydata(FRAME_SIZE);
        std::generate(lMydata.begin(), lMydata.end(), [lN = 0]() mutable { return (uint8_t)(lN++); });

        auto lResult = lSender.send(lMydata, 0x02, 1001, 1, 2, 1);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest24: Fuzz test with 10,000 garbage packets
    // =========================================================================
    TEST_CASE("Fuzz test garbage packets (UnitTest24)" * doctest::timeout(60)) {
        std::random_device lRd;
        std::mt19937 lGen(lRd());
        std::uniform_int_distribution<unsigned int> lDis(0, 255);
        std::uniform_int_distribution<size_t> lSizeDis(1, 10000);

        auto lReceiver = efp::makeReceiver([](efp::SuperFramePtr) {
            // May or may not receive anything - that's ok
        }, [](std::span<const uint8_t>) {}, 50, 20);

        for (int lI = 0; lI < 10000; lI++) {
            size_t lGarbageSize = lSizeDis(lGen);
            std::vector<uint8_t> lGarbage(lGarbageSize);
            std::generate(lGarbage.begin(), lGarbage.end(), [&]() {
                return (uint8_t)(lDis(lGen));
            });

            (void)lReceiver.receive(std::span<const uint8_t>(lGarbage), 0);
        }

        CHECK(true);
    }

    // =========================================================================
    // Random loss simulation
    // =========================================================================
    TEST_CASE("Random loss simulation" * doctest::timeout(30)) {
        constexpr uint32_t NUM_PACKETS = 100;

        std::atomic<size_t> lReceived{0};
        std::atomic<size_t> lBroken{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lReceived++;
            if (apFrame->mBroken) lBroken++;
        }, [](std::span<const uint8_t>) {}, 50, 20);

        std::mt19937 lRng(42);
        std::uniform_int_distribution<int> lLossDist(0, 99);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            // 5% loss rate
            if (lLossDist(lRng) < 5) {
                return;
            }
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(1000);

        for (uint32_t lI = 0; lI < NUM_PACKETS; lI++) {
            (void)lSender.send(lPayload, 0x01, lI, lI, 0, 1);
        }

        REQUIRE(waitFor([&]() { return lReceived.load() >= NUM_PACKETS - 10; },
                        std::chrono::milliseconds(5000)));

        CHECK(lReceived.load() > 0);
    }

}

