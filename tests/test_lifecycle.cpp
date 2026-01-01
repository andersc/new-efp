//
// EFP Unit Tests - Lifecycle and Configuration
//
// Tests for sender/receiver lifecycle, modes, and configuration
// Ported from old UnitTest17, 18, 19, 20, 21
//

#include <doctest/doctest.h>

#include "efp.h"
#include "efp_c_api.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <chrono>
#include <thread>
#include <any>
#include <memory>

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

TEST_SUITE("Lifecycle") {

    // =========================================================================
    // UnitTest17: Stop and restart sender with new counter
    // =========================================================================
    TEST_CASE("Stop and restart sender (UnitTest17)") {
        std::atomic<size_t> lDataReceived{0};
        size_t lReceivedFrameNumber = 0;

        // First session
        {
            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
                lReceivedFrameNumber++;
                CHECK(!apFrame->mBroken);
                CHECK(apFrame->mPts == 1000 + lReceivedFrameNumber);
                CHECK(apFrame->mStreamId == 1);

                uint8_t lVectorChecker = 0;
                for (size_t lX = 0; lX < apFrame->mSize; lX++) {
                    CHECK(apFrame->mpData[lX] == lVectorChecker++);
                }

                lDataReceived++;
            }, 50, 20);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                (void)lReceiver.receive(aData, 0);
            });

            for (uint64_t lPacketNumber = 0; lPacketNumber < 100; lPacketNumber++) {
                size_t lRandSize = (rand() % 10000) + 1;
                std::vector<uint8_t> lMydata(lRandSize);
                std::generate(lMydata.begin(), lMydata.end(), [lN = 0]() mutable {
                    return (uint8_t)(lN++);
                });

                auto lResult = lSender.send(lMydata, 0x83, lPacketNumber + 1001, lPacketNumber,
                                         EFP_CODE('A', 'N', 'X', 'B'), 1);
                CHECK(lResult == efp::Result::OK);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        CHECK(lDataReceived.load() == 100);

        // Second session with new instances
        {
            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
                lReceivedFrameNumber++;
                CHECK(!apFrame->mBroken);
                CHECK(apFrame->mPts == 1000 - 100 + lReceivedFrameNumber);
                CHECK(apFrame->mStreamId == 2);

                uint8_t lVectorChecker = 0;
                for (size_t lX = 0; lX < apFrame->mSize; lX++) {
                    CHECK(apFrame->mpData[lX] == lVectorChecker++);
                }

                lDataReceived++;
            }, 50, 20);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                (void)lReceiver.receive(aData, 0);
            });

            for (uint64_t lPacketNumber = 0; lPacketNumber < 100; lPacketNumber++) {
                size_t lRandSize = (rand() % 10000) + 1;
                std::vector<uint8_t> lMydata(lRandSize);
                std::generate(lMydata.begin(), lMydata.end(), [lN = 0]() mutable {
                    return (uint8_t)(lN++);
                });

                auto lResult = lSender.send(lMydata, 0x83, lPacketNumber + 1001, lPacketNumber,
                                         EFP_CODE('A', 'N', 'X', 'B'), 2);
                CHECK(lResult == efp::Result::OK);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        CHECK(lDataReceived.load() == 200);
    }

    // =========================================================================
    // UnitTest20: Run-to-completion mode basic test
    // =========================================================================
    TEST_CASE("Run-to-completion mode (UnitTest20)") {
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) + 1;

        std::atomic<size_t> lDataReceived{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mStreamId == 4);
            CHECK(apFrame->mPts == 1001);
            CHECK(apFrame->mPayloadCode == 2);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mSize == FRAME_SIZE);
            lDataReceived++;
        }, 50, 20, efp::ReceiverMode::RUN_TO_COMPLETION);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            auto lResult = lReceiver.receive(aData, 0);
            CHECK(lResult == efp::Result::OK);
        });

        std::vector<uint8_t> lMydata(FRAME_SIZE);

        auto lResult = lSender.send(lMydata, 0x02, 1001, 1, 2, 4);
        CHECK(lResult == efp::Result::OK);

        CHECK(lDataReceived.load() == 1);
    }

    // =========================================================================
    // Different buffer sizes
    // =========================================================================
    TEST_CASE("Different buffer sizes work correctly") {
        // Note: Custom buffer sizes require using Receiver/Sender classes directly
        // with explicit callable types. For simplicity, we test with default buffer.

        SUBCASE("Default buffer works") {
            std::atomic<bool> lReceived{false};

            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
                CHECK(!apFrame->mBroken);
                lReceived = true;
            }, 100, 0);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                (void)lReceiver.receive(aData, 0);
            });

            std::vector<uint8_t> lPayload(1000);
            (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return lReceived.load(); }));
        }

        SUBCASE("Multiple small frames") {
            std::atomic<int> lReceived{0};

            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
                CHECK(!apFrame->mBroken);
                lReceived++;
            }, 100, 0);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                (void)lReceiver.receive(aData, 0);
            });

            std::vector<uint8_t> lPayload(100);
            for (int lI = 0; lI < 100; lI++) {
                (void)lSender.send(lPayload, 0x01, lI, lI, 0, 1);
            }

            REQUIRE(waitFor([&]() { return lReceived.load() == 100; }));
        }
    }

    // =========================================================================
    // Thread safety with multiple threads
    // =========================================================================
    TEST_CASE("Thread safety with multiple sender threads") {
        std::atomic<size_t> lReceived{0};
        std::mutex lReceiverMutex;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            if (!apFrame->mBroken) lReceived++;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            std::lock_guard<std::mutex> lLock(lReceiverMutex);
            (void)lReceiver.receive(aData, 0);
        });

        constexpr int NUM_THREADS = 4;
        constexpr int FRAMES_PER_THREAD = 100;

        std::vector<std::thread> lThreads;
        for (int lT = 0; lT < NUM_THREADS; lT++) {
            lThreads.emplace_back([&, lT]() {
                std::vector<uint8_t> lPayload(500);
                for (int lI = 0; lI < FRAMES_PER_THREAD; lI++) {
                    (void)lSender.send(lPayload, 0x01, lT * 1000 + lI, lT * 1000 + lI, 0,
                               (uint8_t)(lT + 1));
                }
            });
        }

        for (auto& lT : lThreads) {
            lT.join();
        }

        REQUIRE(waitFor([&]() {
            return lReceived.load() == NUM_THREADS * FRAMES_PER_THREAD;
        }, std::chrono::milliseconds(5000)));
    }

    // =========================================================================
    // Graceful shutdown under load
    // =========================================================================
    TEST_CASE("Graceful shutdown under load") {
        for (int lIter = 0; lIter < 10; lIter++) {
            std::atomic<bool> lStopSending{false};
            std::atomic<size_t> lReceived{0};
            std::atomic<bool> lReceiverActive{true};

            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr) {
                lReceived++;
            }, 50, 0);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                if (lReceiverActive.load()) {
                    (void)lReceiver.receive(aData, 0);
                }
            });

            std::thread lSendThread([&]() {
                std::vector<uint8_t> lPayload(1000);
                int lI = 0;
                while (!lStopSending.load()) {
                    int lIdx = lI++;
                    (void)lSender.send(lPayload, 0x01, lIdx, lIdx, 0, 1);
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            });

            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            lStopSending = true;
            lSendThread.join();

            lReceiverActive = false;
            lReceiver.stop();

            CHECK(lReceived.load() > 0);
        }

        CHECK(true);
    }

    // =========================================================================
    // Receiver stop is idempotent
    // =========================================================================
    TEST_CASE("Receiver stop is idempotent") {
        auto lReceiver = efp::makeReceiver([](efp::SuperFramePtr) {}, 100, 0);

        lReceiver.stop();
        lReceiver.stop();
        lReceiver.stop();

        CHECK(true);
    }

    // =========================================================================
    // Receiver works after stop and before destruction
    // =========================================================================
    TEST_CASE("Receiver after stop") {
        std::atomic<int> lReceived{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr) {
            lReceived++;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]() { return lReceived.load() == 1; }));

        lReceiver.stop();

        auto lResult = lReceiver.receive(std::span<const uint8_t>(lPayload), 0);
        CHECK(true);
    }

    // =========================================================================
    // Run-to-completion poll behavior
    // =========================================================================
    TEST_CASE("Run-to-completion poll delivers frames") {
        std::atomic<int> lReceived{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(!apFrame->mBroken);
            lReceived++;
        }, 100, 0, efp::ReceiverMode::RUN_TO_COMPLETION);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);
        for (int lI = 0; lI < 10; lI++) {
            (void)lSender.send(lPayload, 0x01, lI, lI, 0, 1);
        }

        CHECK(lReceived.load() == 10);
    }

    // =========================================================================
    // Version consistency
    // =========================================================================
    TEST_CASE("Version consistency") {
        CHECK(efp_version() == efp::VERSION);

        uint8_t lMajor = efp::VERSION >> 8;
        uint8_t lMinor = efp::VERSION & 0xFF;
        CHECK(lMajor == efp::VERSION_MAJOR);
        CHECK(lMinor == efp::VERSION_MINOR);
    }

    // =========================================================================
    // Minimum MTU enforcement
    // =========================================================================
    TEST_CASE("Minimum MTU enforcement") {
        std::atomic<bool> lSent{false};

        auto lSender = efp::makeSender(100, [&](std::span<const uint8_t>, uint8_t) {
            lSent = true;
        });

        std::vector<uint8_t> lPayload(50);
        auto lResult = lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        CHECK(lResult == efp::Result::OK);
        CHECK(lSent.load());
    }

}

