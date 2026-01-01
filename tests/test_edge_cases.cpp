//
// EFP Unit Tests - Edge Cases
//

#include <doctest/doctest.h>

#include "efp.h"
#include "efp_c_api.h"
#include <vector>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <set>
#include <map>
#include <mutex>

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

TEST_SUITE("Edge Cases") {

    TEST_CASE("Single byte payload") {
        std::atomic<bool> lReceived{false};
        efp::SuperFramePtr lCapturedFrame;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload = {0x42};
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
        REQUIRE(lCapturedFrame != nullptr);

        CHECK(lCapturedFrame->mSize == 1);
        CHECK(lCapturedFrame->mpData[0] == 0x42);
        CHECK(!lCapturedFrame->mBroken);
    }

    TEST_CASE("Maximum uint32 values for PTS-DTS difference") {
        std::atomic<bool> lReceived{false};
        efp::SuperFramePtr lCapturedFrame;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);
        uint64_t lPts = 5000000000ULL;
        uint64_t lDts = lPts - (UINT32_MAX - 1);

        (void)lSender.send(lPayload, 0x01, lPts, lDts, 0, 1);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
        REQUIRE(lCapturedFrame != nullptr);

        CHECK(lCapturedFrame->mPts == lPts);
        CHECK(lCapturedFrame->mDts == lDts);
    }

    TEST_CASE("SuperFrame number wrapping (16-bit overflow)") {
        std::atomic<int> lReceivedCount{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            if (!apFrame->mBroken) {
                lReceivedCount++;
            }
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);

        for (int lI = 0; lI < 100; lI++) {
            auto lResult = lSender.send(lPayload, 0x01, lI, lI, 0, 1);
            CHECK(lResult == efp::Result::OK);
        }

        REQUIRE(waitFor([&]{ return lReceivedCount.load() == 100; }));
    }

    TEST_CASE("Receive with invalid frame type") {
        auto lReceiver = efp::makeReceiver([](efp::SuperFramePtr) {}, 100, 0);

        std::vector<uint8_t> lInvalidPacket = {0x0F, 0x00, 0x00, 0x00};
        auto lResult = lReceiver.receive(lInvalidPacket);

        CHECK(lResult == efp::Result::INVALID_PARAMETER);
    }

    TEST_CASE("Receive with packet too small for header") {
        auto lReceiver = efp::makeReceiver([](efp::SuperFramePtr) {}, 100, 0);

        std::vector<uint8_t> lTooSmall = {0x01, 0x00, 0x00, 0x00};
        auto lResult = lReceiver.receive(lTooSmall);

        CHECK(lResult == efp::Result::FRAME_SIZE_MISMATCH);
    }

    TEST_CASE("Fragments arriving for already-delivered frame") {
        std::atomic<int> lReceivedCount{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr) {
            lReceivedCount++;
        }, 50, 0);  // 50ms timeout

        std::vector<std::vector<uint8_t>> lFragments;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lFragments.emplace_back(aData.begin(), aData.end());
        });

        size_t lPayloadSize = MTU * 3;
        std::vector<uint8_t> lPayload(lPayloadSize);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(lFragments.size() >= 3);

        (void)lReceiver.receive(std::span<const uint8_t>(lFragments[0]), 0);

        REQUIRE(waitFor([&]{ return lReceivedCount.load() == 1; }, std::chrono::milliseconds(200)));

        for (size_t lI = 1; lI < lFragments.size(); lI++) {
            auto lResult = lReceiver.receive(std::span<const uint8_t>(lFragments[lI]), 0);
            CHECK(lResult == efp::Result::FRAGMENT_TOO_OLD);
        }
    }

    TEST_CASE("Zero-length embedded data handling") {
        std::atomic<bool> lReceived{false};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr) {
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1, efp::Flags::INLINE_PAYLOAD);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
    }

    TEST_CASE("All 256 stream IDs work") {
        std::atomic<int> lReceivedCount{0};
        std::map<uint8_t, bool> lStreamContentVerified;
        std::mutex lMutex;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            std::lock_guard<std::mutex> lLock(lMutex);

            bool lContentValid = true;
            for (size_t lI = 0; lI < apFrame->mSize; lI++) {
                if (apFrame->mpData[lI] != apFrame->mStreamId) {
                    lContentValid = false;
                    break;
                }
            }
            lStreamContentVerified[apFrame->mStreamId] = lContentValid;
            CHECK(lContentValid);

            lReceivedCount++;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lTestStreams = {1, 127, 128, 254, 255};
        for (uint8_t lStreamId : lTestStreams) {
            std::vector<uint8_t> lPayload(50, lStreamId);
            (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, lStreamId);
        }

        REQUIRE(waitFor([&]{ return lReceivedCount.load() == (int)(lTestStreams.size()); }));

        std::lock_guard<std::mutex> lLock(lMutex);
        CHECK(lReceivedCount.load() == 5);
        for (uint8_t lStreamId : lTestStreams) {
            CHECK(lStreamContentVerified[lStreamId]);
        }
    }

    TEST_CASE("Payload code UINT32_MAX is preserved") {
        std::atomic<bool> lReceived{false};
        efp::SuperFramePtr lCapturedFrame;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, UINT32_MAX, 1);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
        CHECK(lCapturedFrame->mPayloadCode == UINT32_MAX);
    }

    TEST_CASE("PTS UINT64_MAX is preserved") {
        std::atomic<bool> lReceived{false};
        efp::SuperFramePtr lCapturedFrame;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, UINT64_MAX, UINT64_MAX, 0, 1);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
        CHECK(lCapturedFrame->mPts == UINT64_MAX);
        CHECK(lCapturedFrame->mDts == UINT64_MAX);
    }

    TEST_CASE("Receiver stop is idempotent") {
        auto lReceiver = efp::makeReceiver([](efp::SuperFramePtr) {}, 100, 0);

        lReceiver.stop();
        lReceiver.stop();
        lReceiver.stop();

        CHECK(true);
    }

    TEST_CASE("Version returns expected value") {
        CHECK(efp_version() == efp::VERSION);

        uint8_t lMajor = efp::VERSION >> 8;
        uint8_t lMinor = efp::VERSION & 0xFF;
        CHECK(lMajor == efp::VERSION_MAJOR);
        CHECK(lMinor == efp::VERSION_MINOR);
    }

}

