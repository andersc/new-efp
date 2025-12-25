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
bool waitFor(Predicate pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > timeout) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

TEST_SUITE("Integration") {

    TEST_CASE("Send and receive multiple frames sequentially") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<int> receivedCount{0};
        std::vector<efp::SuperFramePtr> receivedFrames;
        std::mutex framesMutex;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            std::lock_guard<std::mutex> lock(framesMutex);
            receivedFrames.push_back(std::move(frame));
            receivedCount++;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        constexpr int numFrames = 10;
        for (int i = 0; i < numFrames; i++) {
            std::vector<uint8_t> payload(100 + i * 50);
            std::fill(payload.begin(), payload.end(), static_cast<uint8_t>(i));

            (void)sender.send(payload, 0x01, 1000 + i, 1000 + i, 0, 1);
        }

        REQUIRE(waitFor([&]{ return receivedCount.load() == numFrames; }));

        std::lock_guard<std::mutex> lock(framesMutex);
        CHECK(receivedFrames.size() == numFrames);

        for (int i = 0; i < numFrames; i++) {
            CHECK(receivedFrames[i]->mPts == static_cast<uint64_t>(1000 + i));
            CHECK(!receivedFrames[i]->mBroken);
        }
    }

    TEST_CASE("Multiple streams simultaneously") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::map<uint8_t, std::vector<efp::SuperFramePtr>> framesByStream;
        std::mutex framesMutex;
        std::atomic<int> totalReceived{0};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            std::lock_guard<std::mutex> lock(framesMutex);
            framesByStream[frame->mStreamId].push_back(std::move(frame));
            totalReceived++;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t streamId) {
            (void)receiver.receive(data, size, 0);
        });

        // Send frames on 3 different streams
        for (int stream = 1; stream <= 3; stream++) {
            for (int i = 0; i < 5; i++) {
                std::vector<uint8_t> payload(200);
                (void)sender.send(payload, 0x01, 1000 + i, 1000 + i, 0, static_cast<uint8_t>(stream));
            }
        }

        REQUIRE(waitFor([&]{ return totalReceived.load() == 15; }));

        std::lock_guard<std::mutex> lock(framesMutex);
        CHECK(framesByStream[1].size() == 5);
        CHECK(framesByStream[2].size() == 5);
        CHECK(framesByStream[3].size() == 5);
    }

    TEST_CASE("Random payload sizes") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<int> receivedCount{0};
        std::vector<size_t> receivedSizes;
        std::mutex mutex;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            std::lock_guard<std::mutex> lock(mutex);
            receivedSizes.push_back(frame->mSize);
            receivedCount++;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        std::mt19937 rng(42);
        std::uniform_int_distribution<size_t> dist(1, MTU * 10);

        std::vector<size_t> sentSizes;
        constexpr int numFrames = 20;

        for (int i = 0; i < numFrames; i++) {
            size_t payloadSize = dist(rng);
            sentSizes.push_back(payloadSize);

            std::vector<uint8_t> payload(payloadSize);
            (void)sender.send(payload, 0x01, i, i, 0, 1);
        }

        REQUIRE(waitFor([&]{ return receivedCount.load() == numFrames; }));

        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE(receivedSizes.size() == sentSizes.size());

        for (size_t i = 0; i < sentSizes.size(); i++) {
            CHECK(receivedSizes[i] == sentSizes[i]);
        }
    }

    TEST_CASE("Data integrity across fragmentation boundaries") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};
        efp::SuperFramePtr capturedFrame;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedFrame = std::move(frame);
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        // Payload that hits exact boundary
        size_t type1Payload = MTU - sizeof(efp::FrameType1);
        size_t payloadSize = type1Payload * 5;  // Exactly 5 full Type1 fragments

        std::vector<uint8_t> payload(payloadSize);
        for (size_t i = 0; i < payloadSize; i++) {
            payload[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);  // Pseudo-random pattern
        }

        (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]{ return received.load(); }));
        REQUIRE(capturedFrame != nullptr);

        CHECK(capturedFrame->mSize == payloadSize);
        CHECK(!capturedFrame->mBroken);

        for (size_t i = 0; i < payloadSize; i++) {
            CHECK(capturedFrame->mpData[i] == static_cast<uint8_t>((i * 7 + 13) & 0xFF));
        }
    }

    TEST_CASE("Using media types for H.264 video") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};
        efp::SuperFramePtr capturedFrame;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedFrame = std::move(frame);
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        // Simulated H.264 NAL unit
        std::vector<uint8_t> nalUnit(5000);

        (void)sender.send(nalUnit,
                    efp::media::PayloadType::H264,
                    90000,   // PTS at 90kHz
                    90000,
                    efp::media::PayloadCode::ANXB,  // Annex B framing
                    1);

        REQUIRE(waitFor([&]{ return received.load(); }));
        REQUIRE(capturedFrame != nullptr);

        CHECK(capturedFrame->mPayloadType == efp::media::PayloadType::H264);
        CHECK(capturedFrame->mPayloadCode == efp::media::PayloadCode::ANXB);
        CHECK(!capturedFrame->mBroken);
    }

    TEST_CASE("Custom buffer size template parameter") {
        // Use smaller buffer for testing
        efp::Sender<1023> sender(MTU);  // 2^10 - 1
        efp::Receiver<1023> receiver(100, 0);

        std::atomic<bool> received{false};

        receiver.setCallback([&](efp::SuperFramePtr) {
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);
        (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]{ return received.load(); }));
    }

    TEST_CASE("Stress test - rapid small frames") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<int> receivedCount{0};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            if (!frame->mBroken) {
                receivedCount++;
            }
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        constexpr int numFrames = 1000;
        std::vector<uint8_t> payload(50);

        for (int i = 0; i < numFrames; i++) {
            (void)sender.send(payload, 0x01, i, i, 0, 1);
        }

        REQUIRE(waitFor([&]{ return receivedCount.load() == numFrames; },
                        std::chrono::milliseconds(2000)));
    }

}

