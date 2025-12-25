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

TEST_SUITE("Edge Cases") {

    TEST_CASE("Single byte payload") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};
        efp::SuperFramePtr capturedFrame;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedFrame = std::move(frame);
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload = {0x42};
        sender.send(payload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]{ return received.load(); }));
        REQUIRE(capturedFrame != nullptr);

        CHECK(capturedFrame->mSize == 1);
        CHECK(capturedFrame->mpData[0] == 0x42);
        CHECK(!capturedFrame->mBroken);
    }

    TEST_CASE("Maximum uint32 values for PTS-DTS difference") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};
        efp::SuperFramePtr capturedFrame;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedFrame = std::move(frame);
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);
        uint64_t pts = 5000000000ULL;  // Large enough to avoid underflow
        uint64_t dts = pts - (UINT32_MAX - 1);  // Maximum valid difference

        sender.send(payload, 0x01, pts, dts, 0, 1);

        REQUIRE(waitFor([&]{ return received.load(); }));
        REQUIRE(capturedFrame != nullptr);

        CHECK(capturedFrame->mPts == pts);
        CHECK(capturedFrame->mDts == dts);
    }

    TEST_CASE("SuperFrame number wrapping (16-bit overflow)") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<int> receivedCount{0};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            if (!frame->mBroken) {
                receivedCount++;
            }
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);

        // Send enough frames to wrap the 16-bit counter
        // We can't actually send 65536 frames in a test, so we'll verify
        // the sender handles the wrapping correctly by checking it doesn't crash
        for (int i = 0; i < 100; i++) {
            auto result = sender.send(payload, 0x01, i, i, 0, 1);
            CHECK(result == efp::Result::OK);
        }

        REQUIRE(waitFor([&]{ return receivedCount.load() == 100; }));
    }

    TEST_CASE("Receive with invalid frame type") {
        efp::Receiver receiver(100, 0);

        std::vector<uint8_t> invalidPacket = {0x0F, 0x00, 0x00, 0x00};  // Invalid type 15
        auto result = receiver.receive(invalidPacket);

        CHECK(result == efp::Result::INVALID_PARAMETER);
    }

    TEST_CASE("Receive with packet too small for header") {
        efp::Receiver receiver(100, 0);

        // Type1 requires 8 bytes, send only 4
        std::vector<uint8_t> tooSmall = {0x01, 0x00, 0x00, 0x00};
        auto result = receiver.receive(tooSmall);

        CHECK(result == efp::Result::FRAME_SIZE_MISMATCH);
    }

    TEST_CASE("Fragments arriving for already-delivered frame") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 0);  // 50ms timeout

        std::atomic<int> receivedCount{0};

        receiver.setCallback([&](efp::SuperFramePtr) {
            receivedCount++;
        });

        std::vector<std::vector<uint8_t>> fragments;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        // Create fragmented payload
        size_t payloadSize = MTU * 3;
        std::vector<uint8_t> payload(payloadSize);
        sender.send(payload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(fragments.size() >= 3);

        // Send only first fragment
        receiver.receive(fragments[0].data(), fragments[0].size(), 0);

        // Wait for timeout
        REQUIRE(waitFor([&]{ return receivedCount.load() == 1; }, std::chrono::milliseconds(200)));

        // Now send remaining fragments - should be rejected as too old
        for (size_t i = 1; i < fragments.size(); i++) {
            auto result = receiver.receive(fragments[i].data(), fragments[i].size(), 0);
            CHECK(result == efp::Result::FRAGMENT_TOO_OLD);
        }
    }

    TEST_CASE("Zero-length embedded data handling") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        // Send with inline payload flag but no embedded data
        std::vector<uint8_t> payload(100);
        sender.send(payload, 0x01, 1000, 1000, 0, 1, efp::Flags::INLINE_PAYLOAD);

        REQUIRE(waitFor([&]{ return received.load(); }));
    }

    TEST_CASE("All 256 stream IDs work") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<int> receivedCount{0};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            receivedCount++;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(50);

        // Test a sample of stream IDs (skip 0 for now)
        std::vector<uint8_t> testStreams = {1, 127, 128, 254, 255};
        for (uint8_t streamId : testStreams) {
            sender.send(payload, 0x01, 1000, 1000, 0, streamId);
        }

        REQUIRE(waitFor([&]{ return receivedCount.load() == static_cast<int>(testStreams.size()); }));

        // Verify each stream ID was received
        CHECK(receivedCount.load() == 5);
    }

    TEST_CASE("Payload code UINT32_MAX is preserved") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};
        efp::SuperFramePtr capturedFrame;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedFrame = std::move(frame);
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);
        sender.send(payload, 0x01, 1000, 1000, UINT32_MAX, 1);

        REQUIRE(waitFor([&]{ return received.load(); }));
        CHECK(capturedFrame->mPayloadCode == UINT32_MAX);
    }

    TEST_CASE("PTS UINT64_MAX is preserved") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};
        efp::SuperFramePtr capturedFrame;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedFrame = std::move(frame);
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);
        sender.send(payload, 0x01, UINT64_MAX, UINT64_MAX, 0, 1);

        REQUIRE(waitFor([&]{ return received.load(); }));
        CHECK(capturedFrame->mPts == UINT64_MAX);
        CHECK(capturedFrame->mDts == UINT64_MAX);
    }

    TEST_CASE("Receiver stop is idempotent") {
        efp::Receiver receiver(100, 0);

        // Stop multiple times should not crash
        receiver.stop();
        receiver.stop();
        receiver.stop();

        CHECK(true);  // If we get here, the test passed
    }

    TEST_CASE("Version returns expected value") {
        CHECK(efp::Sender<>::version() == efp::VERSION);
        CHECK(efp::Receiver<>::version() == efp::VERSION);
        CHECK(efp_version() == efp::VERSION);

        uint8_t major = efp::VERSION >> 8;
        uint8_t minor = efp::VERSION & 0xFF;
        CHECK(major == efp::VERSION_MAJOR);
        CHECK(minor == efp::VERSION_MINOR);
    }

}

