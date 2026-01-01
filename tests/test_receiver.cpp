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

TEST_SUITE("Receiver") {

    TEST_CASE("Receive single Type2 frame") {
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
        std::iota(lPayload.begin(), lPayload.end(), 0);

        (void)lSender.send(lPayload, 0x42, 1000, 900, 0xDEADBEEF, 5);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
        REQUIRE(lCapturedFrame != nullptr);

        CHECK(lCapturedFrame->mSize == 100);
        CHECK(lCapturedFrame->mPayloadType == 0x42);
        CHECK(lCapturedFrame->mPts == 1000);
        CHECK(lCapturedFrame->mDts == 900);
        CHECK(lCapturedFrame->mPayloadCode == 0xDEADBEEF);
        CHECK(lCapturedFrame->mStreamId == 5);
        CHECK(!lCapturedFrame->mBroken);

        // Verify data integrity
        for (size_t lI = 0; lI < 100; lI++) {
            CHECK(lCapturedFrame->mpData[lI] == (uint8_t)(lI));
        }
    }

    TEST_CASE("Receive fragmented frame (Type1 + Type2)") {
        std::atomic<bool> lReceived{false};
        efp::SuperFramePtr lCapturedFrame;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        // Large payload requiring multiple fragments
        size_t lPayloadSize = MTU * 3;
        std::vector<uint8_t> lPayload(lPayloadSize);
        std::generate(lPayload.begin(), lPayload.end(), [lN = 0]() mutable {
            return (uint8_t)(lN++ & 0xFF);
        });

        (void)lSender.send(lPayload, 0x01, 2000, 1900, 0, 1);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
        REQUIRE(lCapturedFrame != nullptr);

        CHECK(lCapturedFrame->mSize == lPayloadSize);
        CHECK(!lCapturedFrame->mBroken);

        // Verify data integrity
        for (size_t lI = 0; lI < lPayloadSize; lI++) {
            CHECK(lCapturedFrame->mpData[lI] == (uint8_t)(lI & 0xFF));
        }
    }

    TEST_CASE("Receive fragments out of order") {
        std::atomic<bool> lReceived{false};
        efp::SuperFramePtr lCapturedFrame;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, 100, 0);

        // Collect fragments first, then send in reverse order
        std::vector<std::vector<uint8_t>> lFragments;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lFragments.emplace_back(aData.begin(), aData.end());
        });

        size_t lPayloadSize = MTU * 3;
        std::vector<uint8_t> lPayload(lPayloadSize);
        std::generate(lPayload.begin(), lPayload.end(), [lN = 0]() mutable {
            return (uint8_t)(lN++ & 0xFF);
        });

        (void)lSender.send(lPayload, 0x01, 2000, 2000, 0, 1);

        // Send in reverse order
        for (auto lIt = lFragments.rbegin(); lIt != lFragments.rend(); ++lIt) {
            (void)lReceiver.receive(std::span<const uint8_t>(*lIt), 0);
        }

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
        REQUIRE(lCapturedFrame != nullptr);

        CHECK(lCapturedFrame->mSize == lPayloadSize);
        CHECK(!lCapturedFrame->mBroken);

        // Verify data integrity
        for (size_t lI = 0; lI < lPayloadSize; lI++) {
            CHECK(lCapturedFrame->mpData[lI] == (uint8_t)(lI & 0xFF));
        }
    }

    TEST_CASE("Duplicate fragments are detected") {
        // Use RunToCompletion mode so frames aren't delivered by background thread
        auto lReceiver = efp::makeReceiver([](efp::SuperFramePtr) {}, 100, 0,
                                            efp::ReceiverMode::RUN_TO_COMPLETION);

        std::vector<std::vector<uint8_t>> lFragments;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lFragments.emplace_back(aData.begin(), aData.end());
        });

        // Use a large payload that requires multiple fragments (Type1 + Type2)
        std::vector<uint8_t> lPayload(MTU * 2);  // Large enough for fragmentation
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        // Should have at least 2 fragments
        REQUIRE(lFragments.size() >= 2);

        // Receive first fragment
        auto lResult1 = lReceiver.receive(std::span<const uint8_t>(lFragments[0]), 0);
        CHECK(lResult1 == efp::Result::OK);

        // Send same fragment again - should be detected as duplicate
        auto lResult2 = lReceiver.receive(std::span<const uint8_t>(lFragments[0]), 0);
        CHECK(lResult2 == efp::Result::DUPLICATE_FRAGMENT);
    }

    TEST_CASE("Frame timeout marks frame as broken") {
        std::atomic<bool> lReceived{false};
        efp::SuperFramePtr lCapturedFrame;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, 50, 0);  // 50ms timeout

        // Collect fragments but only send some
        std::vector<std::vector<uint8_t>> lFragments;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lFragments.emplace_back(aData.begin(), aData.end());
        });

        size_t lPayloadSize = MTU * 3;
        std::vector<uint8_t> lPayload(lPayloadSize);

        (void)lSender.send(lPayload, 0x01, 2000, 2000, 0, 1);

        // Only send first fragment
        if (!lFragments.empty()) {
            (void)lReceiver.receive(std::span<const uint8_t>(lFragments[0]), 0);
        }

        // Wait for timeout
        REQUIRE(waitFor([&]{ return lReceived.load(); }, std::chrono::milliseconds(200)));
        REQUIRE(lCapturedFrame != nullptr);

        CHECK(lCapturedFrame->mBroken);  // Should be marked as broken
    }

    TEST_CASE("Source ID is passed through") {
        std::atomic<bool> lReceived{false};
        uint8_t lCapturedSourceId = 0;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedSourceId = apFrame->mSourceId;
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 123);  // Source ID = 123
        });

        std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]{ return lReceived.load(); }));
        CHECK(lCapturedSourceId == 123);
    }

    TEST_CASE("Run-to-completion mode works without threads") {
        bool lReceived = false;
        efp::SuperFramePtr lCapturedFrame;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, 100, 0, efp::ReceiverMode::RUN_TO_COMPLETION);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);
        std::iota(lPayload.begin(), lPayload.end(), 0);

        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        // Must call poll() to process in run-to-completion mode
        lReceiver.poll();

        CHECK(lReceived);
        REQUIRE(lCapturedFrame != nullptr);
        CHECK(lCapturedFrame->mSize == 100);
        CHECK(!lCapturedFrame->mBroken);
    }

}

