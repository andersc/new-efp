//
// EFP Unit Tests - Receiver
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

// Helper to wait for async operations
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

TEST_SUITE("Receiver") {

    TEST_CASE("Receive single Type2 frame") {
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

        std::vector<uint8_t> payload(100);
        std::iota(payload.begin(), payload.end(), 0);

        (void)sender.send(payload, 0x42, 1000, 900, 0xDEADBEEF, 5);

        REQUIRE(waitFor([&]{ return received.load(); }));
        REQUIRE(capturedFrame != nullptr);

        CHECK(capturedFrame->mSize == 100);
        CHECK(capturedFrame->mPayloadType == 0x42);
        CHECK(capturedFrame->mPts == 1000);
        CHECK(capturedFrame->mDts == 900);
        CHECK(capturedFrame->mPayloadCode == 0xDEADBEEF);
        CHECK(capturedFrame->mStreamId == 5);
        CHECK(!capturedFrame->mBroken);

        // Verify data integrity
        for (size_t i = 0; i < 100; i++) {
            CHECK(capturedFrame->mpData[i] == static_cast<uint8_t>(i));
        }
    }

    TEST_CASE("Receive fragmented frame (Type1 + Type2)") {
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

        // Large payload requiring multiple fragments
        size_t payloadSize = MTU * 3;
        std::vector<uint8_t> payload(payloadSize);
        std::generate(payload.begin(), payload.end(), [n = 0]() mutable {
            return static_cast<uint8_t>(n++ & 0xFF);
        });

        (void)sender.send(payload, 0x01, 2000, 1900, 0, 1);

        REQUIRE(waitFor([&]{ return received.load(); }));
        REQUIRE(capturedFrame != nullptr);

        CHECK(capturedFrame->mSize == payloadSize);
        CHECK(!capturedFrame->mBroken);

        // Verify data integrity
        for (size_t i = 0; i < payloadSize; i++) {
            CHECK(capturedFrame->mpData[i] == static_cast<uint8_t>(i & 0xFF));
        }
    }

    TEST_CASE("Receive fragments out of order") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};
        efp::SuperFramePtr capturedFrame;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedFrame = std::move(frame);
            received = true;
        });

        // Collect fragments first, then send in reverse order
        std::vector<std::vector<uint8_t>> fragments;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        size_t payloadSize = MTU * 3;
        std::vector<uint8_t> payload(payloadSize);
        std::generate(payload.begin(), payload.end(), [n = 0]() mutable {
            return static_cast<uint8_t>(n++ & 0xFF);
        });

        (void)sender.send(payload, 0x01, 2000, 2000, 0, 1);

        // Send in reverse order
        for (auto it = fragments.rbegin(); it != fragments.rend(); ++it) {
            (void)receiver.receive(it->data(), it->size(), 0);
        }

        REQUIRE(waitFor([&]{ return received.load(); }));
        REQUIRE(capturedFrame != nullptr);

        CHECK(capturedFrame->mSize == payloadSize);
        CHECK(!capturedFrame->mBroken);

        // Verify data integrity
        for (size_t i = 0; i < payloadSize; i++) {
            CHECK(capturedFrame->mpData[i] == static_cast<uint8_t>(i & 0xFF));
        }
    }

    TEST_CASE("Duplicate fragments are detected") {
        efp::Sender sender(MTU);
        // Use RunToCompletion mode so frames aren't delivered by background thread
        efp::Receiver receiver(100, 0, efp::ReceiverMode::RUN_TO_COMPLETION);

        std::vector<std::vector<uint8_t>> fragments;

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        // Use a large payload that requires multiple fragments (Type1 + Type2)
        std::vector<uint8_t> payload(MTU * 2);  // Large enough for fragmentation
        (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

        // Should have at least 2 fragments
        REQUIRE(fragments.size() >= 2);

        // Receive first fragment
        auto result1 = receiver.receive(fragments[0].data(), fragments[0].size(), 0);
        CHECK(result1 == efp::Result::OK);

        // Send same fragment again - should be detected as duplicate
        auto result2 = receiver.receive(fragments[0].data(), fragments[0].size(), 0);
        CHECK(result2 == efp::Result::DUPLICATE_FRAGMENT);
    }

    TEST_CASE("Frame timeout marks frame as broken") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 0);  // 50ms timeout

        std::atomic<bool> received{false};
        efp::SuperFramePtr capturedFrame;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedFrame = std::move(frame);
            received = true;
        });

        // Collect fragments but only send some
        std::vector<std::vector<uint8_t>> fragments;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        size_t payloadSize = MTU * 3;
        std::vector<uint8_t> payload(payloadSize);

        (void)sender.send(payload, 0x01, 2000, 2000, 0, 1);

        // Only send first fragment
        if (!fragments.empty()) {
            (void)receiver.receive(fragments[0].data(), fragments[0].size(), 0);
        }

        // Wait for timeout
        REQUIRE(waitFor([&]{ return received.load(); }, std::chrono::milliseconds(200)));
        REQUIRE(capturedFrame != nullptr);

        CHECK(capturedFrame->mBroken);  // Should be marked as broken
    }

    TEST_CASE("Source ID is passed through") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};
        uint8_t capturedSourceId = 0;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedSourceId = frame->mSourceId;
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 123);  // Source ID = 123
        });

        std::vector<uint8_t> payload(100);
        (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]{ return received.load(); }));
        CHECK(capturedSourceId == 123);
    }

    TEST_CASE("Run-to-completion mode works without threads") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0, efp::ReceiverMode::RUN_TO_COMPLETION);

        bool received = false;
        efp::SuperFramePtr capturedFrame;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedFrame = std::move(frame);
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);
        std::iota(payload.begin(), payload.end(), 0);

        (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

        // Must call poll() to process in run-to-completion mode
        receiver.poll();

        CHECK(received);
        REQUIRE(capturedFrame != nullptr);
        CHECK(capturedFrame->mSize == 100);
        CHECK(!capturedFrame->mBroken);
    }

}

