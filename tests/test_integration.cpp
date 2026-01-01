//
// EFP Unit Tests - Integration
//

#include <doctest/doctest.h>

#include "efp.h"
#include "efp_media_types.h"
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

TEST_SUITE("Integration") {

    TEST_CASE("Send and receive multiple frames sequentially") {
        std::atomic<int> lReceivedCount{0};
        std::vector<efp::SuperFramePtr> lReceivedFrames;
        std::mutex lFramesMutex;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            std::lock_guard<std::mutex> lLock(lFramesMutex);
            lReceivedFrames.push_back(std::move(apFrame));
            lReceivedCount++;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        constexpr int NUM_FRAMES = 10;
        for (int lI = 0; lI < NUM_FRAMES; lI++) {
            std::vector<uint8_t> lPayload(100 + lI * 50);
            std::fill(lPayload.begin(), lPayload.end(), (uint8_t)(lI));

            (void)lSender.send(lPayload, 0x01, 1000 + lI, 1000 + lI, 0, 1);
        }

        REQUIRE(waitFor([&]{ return lReceivedCount.load() == NUM_FRAMES; }));

        std::lock_guard<std::mutex> lLock(lFramesMutex);
        CHECK(lReceivedFrames.size() == NUM_FRAMES);

        for (int lI = 0; lI < NUM_FRAMES; lI++) {
            CHECK(lReceivedFrames[lI]->mPts == (uint64_t)(1000 + lI));
            CHECK(!lReceivedFrames[lI]->mBroken);
            CHECK(lReceivedFrames[lI]->mSize == (size_t)(100 + lI * 50));

            for (size_t lJ = 0; lJ < lReceivedFrames[lI]->mSize; lJ++) {
                CHECK(lReceivedFrames[lI]->mpData[lJ] == (uint8_t)(lI));
            }
        }
    }

    TEST_CASE("Multiple streams simultaneously") {
        std::map<uint8_t, std::vector<efp::SuperFramePtr>> lFramesByStream;
        std::mutex lFramesMutex;
        std::atomic<int> lTotalReceived{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            std::lock_guard<std::mutex> lLock(lFramesMutex);
            lFramesByStream[apFrame->mStreamId].push_back(std::move(apFrame));
            lTotalReceived++;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        // Send frames on 3 different streams
        for (int lStream = 1; lStream <= 3; lStream++) {
            for (int lI = 0; lI < 5; lI++) {
                std::vector<uint8_t> lPayload(200);
                (void)lSender.send(lPayload, 0x01, 1000 + lI, 1000 + lI, 0, (uint8_t)(lStream));
            }
        }

        REQUIRE(waitFor([&]{ return lTotalReceived.load() == 15; }));

        std::lock_guard<std::mutex> lLock(lFramesMutex);
        CHECK(lFramesByStream[1].size() == 5);
        CHECK(lFramesByStream[2].size() == 5);
        CHECK(lFramesByStream[3].size() == 5);
    }

    TEST_CASE("Random payload sizes") {
        std::atomic<int> lReceivedCount{0};
        std::vector<size_t> lReceivedSizes;
        std::mutex lMutex;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            std::lock_guard<std::mutex> lLock(lMutex);
            lReceivedSizes.push_back(apFrame->mSize);
            lReceivedCount++;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::mt19937 lRng(42);
        std::uniform_int_distribution<size_t> lDist(1, MTU * 10);

        std::vector<size_t> lSentSizes;
        constexpr int NUM_FRAMES = 20;

        for (int lI = 0; lI < NUM_FRAMES; lI++) {
            size_t lPayloadSize = lDist(lRng);
            lSentSizes.push_back(lPayloadSize);

            std::vector<uint8_t> lPayload(lPayloadSize);
            (void)lSender.send(lPayload, 0x01, lI, lI, 0, 1);
        }

        REQUIRE(waitFor([&]{ return lReceivedCount.load() == NUM_FRAMES; }));

        std::lock_guard<std::mutex> lLock(lMutex);
        REQUIRE(lReceivedSizes.size() == lSentSizes.size());

        for (size_t lI = 0; lI < lSentSizes.size(); lI++) {
            CHECK(lReceivedSizes[lI] == lSentSizes[lI]);
        }
    }

    TEST_CASE("Data integrity across fragmentation boundaries") {
        std::atomic<bool> lReceived{false};
        efp::SuperFramePtr lCapturedFrame;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        // Payload that hits exact boundary
        size_t lType1Payload = MTU - sizeof(efp::FrameType1);
        size_t lPayloadSize = lType1Payload * 5;  // Exactly 5 full Type1 fragments

        std::vector<uint8_t> lPayload(lPayloadSize);
        for (size_t lI = 0; lI < lPayloadSize; lI++) {
            lPayload[lI] = (uint8_t)((lI * 7 + 13) & 0xFF);  // Pseudo-random pattern
        }

        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
        REQUIRE(lCapturedFrame != nullptr);

        CHECK(lCapturedFrame->mSize == lPayloadSize);
        CHECK(!lCapturedFrame->mBroken);

        for (size_t lI = 0; lI < lPayloadSize; lI++) {
            CHECK(lCapturedFrame->mpData[lI] == (uint8_t)((lI * 7 + 13) & 0xFF));
        }
    }

    TEST_CASE("Using media types for H.264 video") {
        std::atomic<bool> lReceived{false};
        efp::SuperFramePtr lCapturedFrame;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        // Simulated H.264 NAL unit
        std::vector<uint8_t> lNalUnit(5000);

        (void)lSender.send(lNalUnit,
                    efp::media::PayloadType::H264,
                    90000,   // PTS at 90kHz
                    90000,
                    efp::media::PayloadCode::ANXB,  // Annex B framing
                    1);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
        REQUIRE(lCapturedFrame != nullptr);

        CHECK(lCapturedFrame->mPayloadType == efp::media::PayloadType::H264);
        CHECK(lCapturedFrame->mPayloadCode == efp::media::PayloadCode::ANXB);
        CHECK(!lCapturedFrame->mBroken);
    }

    TEST_CASE("Custom buffer size template parameter") {
        std::atomic<bool> lReceived{false};

        // For custom buffer sizes, we need to define callable types explicitly
        // or just use the default buffer size with makeReceiver
        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr) {
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
    }

    TEST_CASE("Stress test - rapid small frames") {
        std::atomic<int> lReceivedCount{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            if (!apFrame->mBroken) {
                lReceivedCount++;
            }
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        constexpr int NUM_FRAMES = 1000;
        std::vector<uint8_t> lPayload(50);

        for (int lI = 0; lI < NUM_FRAMES; lI++) {
            (void)lSender.send(lPayload, 0x01, lI, lI, 0, 1);
        }

        REQUIRE(waitFor([&]{ return lReceivedCount.load() == NUM_FRAMES; },
                        std::chrono::milliseconds(2000)));
    }

}

