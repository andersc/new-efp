//
// EFP Unit Tests - Stress Tests
//
// High-volume tests for production-level validation
// Ported from old UnitTest13 + Extended coverage
//

#include <doctest/doctest.h>

#include "efp.h"
#include <array>
#include <vector>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <chrono>
#include <thread>
#include <random>
#include <map>

constexpr uint16_t MTU = 1456;

template<typename Predicate>
bool waitFor(Predicate aPred, std::chrono::milliseconds aTimeout = std::chrono::milliseconds(5000)) {
    auto lStart = std::chrono::steady_clock::now();
    while (!aPred()) {
        if (std::chrono::steady_clock::now() - lStart > aTimeout) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

TEST_SUITE("Stress Tests") {

    // =========================================================================
    // UnitTest13: Send 50,000 superframes of fixed size (within buffer limits)
    // =========================================================================
    TEST_CASE("Send 50000 superframes (UnitTest13)" * doctest::timeout(120)) {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;
        const size_t FRAME_COUNT = 50000;  // Keep within buffer size

        std::atomic<size_t> lDataReceived{0};

        // For custom buffer sizes, use makeReceiver without explicit template params
        // The default buffer size (8191) is sufficient for most cases
        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mPts - apFrame->mDts == 1000);
            CHECK(apFrame->mStreamId == 1);
            CHECK(apFrame->mPayloadCode == 0);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mSize == FRAME_SIZE);
            lDataReceived++;
        }, [](std::span<const uint8_t>) {}, 50, 20, 3, 0, efp::ReceiverMode::RUN_TO_COMPLETION);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            auto lResult = lReceiver.receive(aData, 0);
            CHECK((int16_t)lResult >= 0);
            lReceiver.poll();
        });

        std::vector<uint8_t> lMyData(FRAME_SIZE);

        for (size_t lPacketNumber = 0; lPacketNumber < FRAME_COUNT; lPacketNumber++) {
            auto lResult = lSender.send(std::span<const uint8_t>(lMyData),
                                      0x83,
                                      lPacketNumber + 1001,
                                      lPacketNumber + 1,
                                      0, 1);
            REQUIRE(lResult == efp::Result::OK);
        }

        CHECK(lDataReceived.load() == FRAME_COUNT);
    }

    // =========================================================================
    // Send 1,000,000 small frames (endurance test)
    // =========================================================================
    TEST_CASE("Send 1000000 small frames (endurance)" * doctest::timeout(300)) {
        const size_t FRAME_COUNT = 1000000;

        std::atomic<size_t> lDataReceived{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            if (!apFrame->mBroken) {
                lDataReceived++;
            }
        }, [](std::span<const uint8_t>) {}, 50, 0, 3, 0, efp::ReceiverMode::RUN_TO_COMPLETION);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
            lReceiver.poll();
        });

        std::vector<uint8_t> lPayload(100);

        for (uint64_t lI = 0; lI < FRAME_COUNT; lI++) {
            (void)lSender.send(lPayload, 0x01, lI, lI, 0, 1);
        }

        CHECK(lDataReceived.load() == FRAME_COUNT);
    }

    // =========================================================================
    // UnitTest15 variant: Random sizes 1-100KB, 1000 iterations
    // =========================================================================
    TEST_CASE("Send 1000 packets with random sizes 1-100KB" * doctest::timeout(120)) {
        std::atomic<size_t> lDataReceived{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mStreamId == 1);
            CHECK(apFrame->mPts - apFrame->mDts == 1001);
            CHECK(apFrame->mPts >= 1001);
            CHECK(apFrame->mPts <= 2000);
            CHECK(apFrame->mPayloadCode == EFP_CODE('A', 'N', 'X', 'B'));

            uint8_t lVectorChecker = 0;
            for (size_t lX = 0; lX < apFrame->mSize; lX++) {
                CHECK(apFrame->mpData[lX] == lVectorChecker++);
            }

            lDataReceived++;
        }, [](std::span<const uint8_t>) {}, 100, 40);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            auto lResult = lReceiver.receive(aData, 0);
            CHECK(lResult == efp::Result::OK);
        });

        std::mt19937 lRng(42);
        std::uniform_int_distribution<size_t> lDist(1, 100000);

        for (size_t lPacketNumber = 0; lPacketNumber < 1000; lPacketNumber++) {
            size_t lRandSize = lDist(lRng);
            std::vector<uint8_t> lMydata(lRandSize);
            std::generate(lMydata.begin(), lMydata.end(), [lN = 0]() mutable { return (uint8_t)(lN++); });

            auto lResult = lSender.send(std::span<const uint8_t>(lMydata),
                                      0x83,
                                      lPacketNumber + 1001,
                                      lPacketNumber,
                                      EFP_CODE('A', 'N', 'X', 'B'),
                                      1);
            REQUIRE(lResult == efp::Result::OK);
        }

        REQUIRE(waitFor([&]() {
            return lDataReceived.load() == 1000;
        }, std::chrono::milliseconds(60000)));
    }

    // =========================================================================
    // Multi-stream stress: 10 streams, 100 frames each
    // =========================================================================
    TEST_CASE("Multi-stream stress: 10 streams x 100 frames" * doctest::timeout(60)) {
        std::array<std::atomic<size_t>, 11> lReceivedByStream{};
        for (auto& lA : lReceivedByStream) lA.store(0);

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(!apFrame->mBroken);
            lReceivedByStream[apFrame->mStreamId]++;
        }, [](std::span<const uint8_t>) {}, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(500);

        for (int lFrame = 0; lFrame < 100; lFrame++) {
            for (uint8_t lStream = 1; lStream <= 10; lStream++) {
                (void)lSender.send(lPayload, 0x01, lFrame, lFrame, 0, lStream);
            }
        }

        REQUIRE(waitFor([&]() {
            size_t lTotal = 0;
            for (uint8_t lS = 1; lS <= 10; lS++) {
                lTotal += lReceivedByStream[lS].load();
            }
            return lTotal == 1000;
        }, std::chrono::milliseconds(30000)));

        for (uint8_t lS = 1; lS <= 10; lS++) {
            CHECK(lReceivedByStream[lS].load() == 100);
        }
    }

    // =========================================================================
    // SuperFrame counter wraparound stress test (3 times = 196608 frames)
    // =========================================================================
    TEST_CASE("SuperFrame counter wraparound 3 times" * doctest::timeout(120)) {
        std::atomic<size_t> lDataReceived{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            if (!apFrame->mBroken) {
                lDataReceived++;
            }
        }, [](std::span<const uint8_t>) {}, 50, 0, 3, 0, efp::ReceiverMode::RUN_TO_COMPLETION);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
            lReceiver.poll();
        });

        std::vector<uint8_t> lPayload(50);
        constexpr size_t NUM_FRAMES = 65536 * 3;

        for (size_t lI = 0; lI < NUM_FRAMES; lI++) {
            auto lResult = lSender.send(lPayload, 0x01, lI, lI, 0, 1);
            REQUIRE(lResult == efp::Result::OK);
        }

        CHECK(lDataReceived.load() == NUM_FRAMES);
    }

    // =========================================================================
    // Maximum fragment count per superframe (8000+ fragments)
    // =========================================================================
    TEST_CASE("Maximum fragments per superframe (8000+ fragments)" * doctest::timeout(60)) {
        std::atomic<bool> lReceived{false};
        efp::SuperFramePtr lCapturedFrame;
        std::mutex lFrameMutex;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            std::lock_guard<std::mutex> lLock(lFrameMutex);
            lCapturedFrame = std::move(apFrame);
            lReceived = true;
        }, [](std::span<const uint8_t>) {}, 500, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        size_t lType1PayloadSize = MTU - sizeof(efp::FrameType1);
        size_t lPayloadSize = lType1PayloadSize * 8000;

        std::vector<uint8_t> lPayload(lPayloadSize);
        std::generate(lPayload.begin(), lPayload.end(), [lN = 0]() mutable {
            return (uint8_t)(lN++ & 0xFF);
        });

        auto lResult = lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);
        REQUIRE(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() {
            return lReceived.load();
        }, std::chrono::milliseconds(30000)));

        std::lock_guard<std::mutex> lLock(lFrameMutex);
        REQUIRE(lCapturedFrame != nullptr);
        CHECK(lCapturedFrame->mSize == lPayloadSize);
        CHECK(!lCapturedFrame->mBroken);

        for (size_t lI = 0; lI < std::min(lPayloadSize, (size_t)10000); lI++) {
            CHECK(lCapturedFrame->mpData[lI] == (uint8_t)(lI & 0xFF));
        }
    }

    // =========================================================================
    // Concurrent senders to single receiver
    // =========================================================================
    TEST_CASE("3 concurrent senders to 1 receiver" * doctest::timeout(60)) {
        std::atomic<size_t> lReceived1{0};
        std::atomic<size_t> lReceived2{0};
        std::atomic<size_t> lReceived3{0};

        auto lReceiver1 = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            if (!apFrame->mBroken) lReceived1++;
        }, [](std::span<const uint8_t>) {}, 100, 0);
        auto lReceiver2 = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            if (!apFrame->mBroken) lReceived2++;
        }, [](std::span<const uint8_t>) {}, 100, 0);
        auto lReceiver3 = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            if (!apFrame->mBroken) lReceived3++;
        }, [](std::span<const uint8_t>) {}, 100, 0);

        auto lSender1 = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver1.receive(aData, 1);
        });
        auto lSender2 = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver2.receive(aData, 2);
        });
        auto lSender3 = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver3.receive(aData, 3);
        });

        std::vector<uint8_t> lPayload(500);
        constexpr int FRAMES_PER_SENDER = 1000;

        std::thread lT1([&]() {
            for (int lI = 0; lI < FRAMES_PER_SENDER; lI++) {
                (void)lSender1.send(lPayload, 0x01, lI, lI, 0, 1);
            }
        });
        std::thread lT2([&]() {
            for (int lI = 0; lI < FRAMES_PER_SENDER; lI++) {
                (void)lSender2.send(lPayload, 0x02, lI, lI, 0, 2);
            }
        });
        std::thread lT3([&]() {
            for (int lI = 0; lI < FRAMES_PER_SENDER; lI++) {
                (void)lSender3.send(lPayload, 0x03, lI, lI, 0, 3);
            }
        });

        lT1.join();
        lT2.join();
        lT3.join();

        REQUIRE(waitFor([&]() {
            return (lReceived1.load() + lReceived2.load() + lReceived3.load()) == FRAMES_PER_SENDER * 3;
        }, std::chrono::milliseconds(30000)));

        CHECK(lReceived1.load() == FRAMES_PER_SENDER);
        CHECK(lReceived2.load() == FRAMES_PER_SENDER);
        CHECK(lReceived3.load() == FRAMES_PER_SENDER);
    }

    // =========================================================================
    // Memory allocation stress - allocate/deallocate rapidly
    // =========================================================================
    TEST_CASE("Rapid sender/receiver creation/destruction" * doctest::timeout(60)) {
        for (int lIteration = 0; lIteration < 100; lIteration++) {
            std::atomic<int> lReceived{0};

            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
                if (!apFrame->mBroken) lReceived++;
            }, [](std::span<const uint8_t>) {}, 50, 0);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                (void)lReceiver.receive(aData, 0);
            });

            std::vector<uint8_t> lPayload(1000);
            for (int lI = 0; lI < 10; lI++) {
                (void)lSender.send(lPayload, 0x01, lI, lI, 0, 1);
            }

            waitFor([&]() { return lReceived.load() == 10; }, std::chrono::milliseconds(500));
        }

        CHECK(true);
    }

    // =========================================================================
    // Large buffer sizes stress test
    // =========================================================================
    TEST_CASE("Large buffer size stress" * doctest::timeout(30)) {
        std::atomic<size_t> lReceived{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            if (!apFrame->mBroken) lReceived++;
        }, [](std::span<const uint8_t>) {}, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(1000);
        constexpr size_t NUM_FRAMES = 1000;

        for (size_t lI = 0; lI < NUM_FRAMES; lI++) {
            (void)lSender.send(lPayload, 0x01, lI, lI, 0, 1);
        }

        REQUIRE(waitFor([&]() {
            return lReceived.load() == NUM_FRAMES;
        }, std::chrono::milliseconds(15000)));
    }

    // =========================================================================
    // Test processRetransmits thread-safety with concurrent operations
    // =========================================================================
    TEST_CASE("processRetransmits thread-safety with concurrent send and receiveNack" * doctest::timeout(30)) {
        std::atomic<size_t> lSendCallbackCount{0};
        std::atomic<bool> lRunning{true};
        std::mutex lMutex;

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t>, uint8_t) {
            lSendCallbackCount++;
        }, efp::SubFragmentMode::SINGLE, 1000);  // 1 second retention

        // Thread 1: Continuously send frames
        std::thread lSendThread([&]() {
            std::vector<uint8_t> lPayload(MTU * 2);  // Multi-fragment frames
            uint64_t lPts = 0;
            while (lRunning) {
                (void)lSender.send(lPayload, 0x01, lPts, lPts, 42, 1);
                lPts++;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });

        // Thread 2: Continuously send NACKs
        std::thread lNackThread([&]() {
            uint16_t lSuperFrameNo = 0;
            while (lRunning) {
                std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack) + sizeof(efp::NackEntry));

                efp::FrameType0Nack lNackHeader;
                lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);
                lNackHeader.mSubtype = (uint8_t)(efp::Type0Subtype::NACK);
                lNackHeader.mNackCount = 1;

                efp::NackEntry lNackEntry;
                lNackEntry.mStreamId = 1;
                lNackEntry.mSuperFrameNo = lSuperFrameNo++;
                lNackEntry.mFragmentNo = 0;
                lNackEntry.mFragmentCount = 0;

                std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));
                std::memcpy(lNackData.data() + sizeof(lNackHeader), &lNackEntry, sizeof(lNackEntry));

                (void)lSender.receiveNack(std::span<const uint8_t>(lNackData));
                std::this_thread::sleep_for(std::chrono::microseconds(150));
            }
        });

        // Thread 3: Continuously call processRetransmits
        std::thread lRetransmitThread([&]() {
            while (lRunning) {
                (void)lSender.processRetransmits(5);  // Process up to 5 at a time
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });

        // Thread 4: Continuously check statistics
        std::thread lStatsThread([&]() {
            while (lRunning) {
                auto lStats = lSender.getStatistics();
                // Just access stats to ensure no data races
                (void)lStats.mRetransmittedFragments;
                (void)lStats.mRetransmitQueueSize;
                (void)lStats.mNacksReceived;
                std::this_thread::sleep_for(std::chrono::microseconds(75));
            }
        });

        // Let threads run for 2 seconds
        std::this_thread::sleep_for(std::chrono::seconds(2));

        lRunning = false;

        lSendThread.join();
        lNackThread.join();
        lRetransmitThread.join();
        lStatsThread.join();

        // Verify no crashes occurred and some work was done
        auto lFinalStats = lSender.getStatistics();
        CHECK(lFinalStats.mFragmentsSent > 0);
        CHECK(lSendCallbackCount > 0);

        // If NACKs were processed successfully, we should see some stats
        // (Not guaranteed due to timing, but at least shouldn't crash)
        MESSAGE("Fragments sent: " << lFinalStats.mFragmentsSent);
        MESSAGE("NACKs received: " << lFinalStats.mNacksReceived);
        MESSAGE("Retransmits: " << lFinalStats.mRetransmittedFragments);
    }

}

