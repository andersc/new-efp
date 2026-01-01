//
// EFP Unit Tests - Embedded Data
//
// Tests for embedded payload data functionality
//

#include <doctest/doctest.h>

#include "efp.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
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

TEST_SUITE("Embedded Data") {

    // =========================================================================
    // Basic embedded data add and extract
    // =========================================================================
    TEST_CASE("Add and extract single embedded data") {
        struct PrivateData {
            int value1 = 42;
            uint8_t value2 = 0xAB;
            uint64_t value3 = 0xDEADBEEFCAFEBABE;
        };

        PrivateData lOriginal;
        lOriginal.value1 = 12345;
        lOriginal.value2 = 0xCD;
        lOriginal.value3 = 0x123456789ABCDEF0;

        std::vector<uint8_t> lPayload(1000);
        std::generate(lPayload.begin(), lPayload.end(), [lN = 0]() mutable {
            return (uint8_t)(lN++);
        });

        std::vector<uint8_t> lCombined;

        uint8_t lEmbeddedType = 0x81;
        uint16_t lEmbeddedSize = sizeof(PrivateData);

        lCombined.push_back(lEmbeddedType);
        lCombined.push_back(lEmbeddedSize & 0xFF);
        lCombined.push_back((lEmbeddedSize >> 8) & 0xFF);

        const uint8_t* lpPrivDataPtr = (const uint8_t*)(&lOriginal);
        lCombined.insert(lCombined.end(), lpPrivDataPtr, lpPrivDataPtr + sizeof(PrivateData));

        lCombined.insert(lCombined.end(), lPayload.begin(), lPayload.end());

        std::atomic<bool> lReceived{false};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mFlags == efp::Flags::INLINE_PAYLOAD);

            if (apFrame->mSize >= 3) {
                uint8_t lType = apFrame->mpData[0];
                uint16_t lSize = apFrame->mpData[1] | (apFrame->mpData[2] << 8);

                CHECK((lType & 0x80) != 0);
                CHECK(lSize == sizeof(PrivateData));

                if (apFrame->mSize >= 3 + lSize) {
                    PrivateData lExtracted;
                    std::memcpy(&lExtracted, apFrame->mpData + 3, sizeof(PrivateData));

                    CHECK(lExtracted.value1 == lOriginal.value1);
                    CHECK(lExtracted.value2 == lOriginal.value2);
                    CHECK(lExtracted.value3 == lOriginal.value3);
                }
            }

            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        (void)lSender.send(lCombined, 0x83, 1000, 1000, EFP_CODE('A', 'N', 'X', 'B'), 1,
                   efp::Flags::INLINE_PAYLOAD);

        REQUIRE(waitFor([&]() { return lReceived.load(); }));
    }

    // =========================================================================
    // Embedded data with fragmentation
    // =========================================================================
    TEST_CASE("Embedded data with fragmented payload") {
        const size_t FRAME_SIZE = MTU * 3;

        std::atomic<bool> lReceived{false};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mFlags == efp::Flags::INLINE_PAYLOAD);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(FRAME_SIZE);
        std::generate(lPayload.begin(), lPayload.end(), [lN = 0]() mutable {
            return (uint8_t)(lN++);
        });

        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1, efp::Flags::INLINE_PAYLOAD);

        REQUIRE(waitFor([&]() { return lReceived.load(); }));
    }

    // =========================================================================
    // Multiple frames with embedded data
    // =========================================================================
    TEST_CASE("Multiple frames with embedded data") {
        std::atomic<int> lReceivedCount{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mFlags == efp::Flags::INLINE_PAYLOAD);
            lReceivedCount++;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        for (int lI = 0; lI < 10; lI++) {
            std::vector<uint8_t> lPayload(500);
            (void)lSender.send(lPayload, 0x01, lI, lI, 0, 1, efp::Flags::INLINE_PAYLOAD);
        }

        REQUIRE(waitFor([&]() { return lReceivedCount.load() == 10; }));
    }

}

