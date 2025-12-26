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

TEST_SUITE("Lifecycle") {

    // =========================================================================
    // UnitTest17: Stop and restart sender with new counter
    // =========================================================================
    TEST_CASE("Stop and restart sender (UnitTest17)") {
        std::atomic<size_t> dataReceived{0};
        size_t receivedFrameNumber = 0;

        // First session
        {
            efp::Sender sender(MTU);
            efp::Receiver receiver(50, 20);

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                (void)receiver.receive(data, size, 0);
            });

            receiver.setCallback([&](efp::SuperFramePtr frame) {
                receivedFrameNumber++;
                CHECK(!frame->mBroken);
                CHECK(frame->mPts == 1000 + receivedFrameNumber);
                CHECK(frame->mStreamId == 1);

                // Verify data integrity - sequential bytes
                uint8_t lVectorChecker = 0;
                for (size_t lX = 0; lX < frame->mSize; lX++) {
                    CHECK(frame->mpData[lX] == lVectorChecker++);
                }

                dataReceived++;
            });

            for (uint64_t packetNumber = 0; packetNumber < 100; packetNumber++) {
                size_t randSize = (rand() % 10000) + 1;
                std::vector<uint8_t> mydata(randSize);
                std::generate(mydata.begin(), mydata.end(), [lN = 0]() mutable {
                    return static_cast<uint8_t>(lN++);
                });

                auto result = sender.send(mydata, 0x83, packetNumber + 1001, packetNumber,
                                         EFP_CODE('A', 'N', 'X', 'B'), 1);
                CHECK(result == efp::Result::OK);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        // sender and receiver destroyed here

        CHECK(dataReceived.load() == 100);

        // Second session with new instances
        {
            efp::Sender sender(MTU);
            efp::Receiver receiver(50, 20);

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                (void)receiver.receive(data, size, 0);
            });

            receiver.setCallback([&](efp::SuperFramePtr frame) {
                receivedFrameNumber++;
                CHECK(!frame->mBroken);
                CHECK(frame->mPts == 1000 - 100 + receivedFrameNumber);
                CHECK(frame->mStreamId == 2);

                // Verify data integrity - sequential bytes
                uint8_t lVectorChecker = 0;
                for (size_t lX = 0; lX < frame->mSize; lX++) {
                    CHECK(frame->mpData[lX] == lVectorChecker++);
                }

                dataReceived++;
            });

            for (uint64_t packetNumber = 0; packetNumber < 100; packetNumber++) {
                size_t randSize = (rand() % 10000) + 1;
                std::vector<uint8_t> mydata(randSize);
                std::generate(mydata.begin(), mydata.end(), [lN = 0]() mutable {
                    return static_cast<uint8_t>(lN++);
                });

                auto result = sender.send(mydata, 0x83, packetNumber + 1001, packetNumber,
                                         EFP_CODE('A', 'N', 'X', 'B'), 2);
                CHECK(result == efp::Result::OK);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        CHECK(dataReceived.load() == 200);
    }

    // =========================================================================
    // UnitTest20: Run-to-completion mode basic test
    // =========================================================================
    TEST_CASE("Run-to-completion mode (UnitTest20)") {
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) + 1;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20, efp::ReceiverMode::RUN_TO_COMPLETION);

        std::atomic<size_t> dataReceived{0};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->mStreamId == 4);
            CHECK(frame->mPts == 1001);
            CHECK(frame->mPayloadCode == 2);
            CHECK(!frame->mBroken);
            CHECK(frame->mSize == FRAME_SIZE);
            dataReceived++;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            auto result = receiver.receive(data, size, 0);
            CHECK(result == efp::Result::OK);
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);

        auto result = sender.send(mydata, 0x02, 1001, 1, 2, 4);
        CHECK(result == efp::Result::OK);

        // In run-to-completion mode, data should be delivered synchronously
        // after all fragments are received
        CHECK(dataReceived.load() == 1);
    }

    // =========================================================================
    // Different buffer sizes
    // =========================================================================
    TEST_CASE("Different buffer sizes work correctly") {
        SUBCASE("Buffer size 1023") {
            efp::Sender<1023> sender(MTU);
            efp::Receiver<1023> receiver(100, 0);

            std::atomic<bool> received{false};
            receiver.setCallback([&](efp::SuperFramePtr frame) {
                CHECK(!frame->mBroken);
                received = true;
            });

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                (void)receiver.receive(data, size, 0);
            });

            std::vector<uint8_t> payload(1000);
            (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return received.load(); }));
        }

        SUBCASE("Buffer size 4095") {
            efp::Sender<4095> sender(MTU);
            efp::Receiver<4095> receiver(100, 0);

            std::atomic<bool> received{false};
            receiver.setCallback([&](efp::SuperFramePtr frame) {
                CHECK(!frame->mBroken);
                received = true;
            });

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                (void)receiver.receive(data, size, 0);
            });

            std::vector<uint8_t> payload(1000);
            (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return received.load(); }));
        }

        SUBCASE("Buffer size 16383") {
            efp::Sender<16383> sender(MTU);
            efp::Receiver<16383> receiver(100, 0);

            std::atomic<bool> received{false};
            receiver.setCallback([&](efp::SuperFramePtr frame) {
                CHECK(!frame->mBroken);
                received = true;
            });

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                (void)receiver.receive(data, size, 0);
            });

            std::vector<uint8_t> payload(1000);
            (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return received.load(); }));
        }
    }

    // =========================================================================
    // Thread safety with multiple threads
    // =========================================================================
    TEST_CASE("Thread safety with multiple sender threads") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<size_t> received{0};
        std::mutex receiverMutex;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            if (!frame->mBroken) received++;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            std::lock_guard<std::mutex> lock(receiverMutex);
            (void)receiver.receive(data, size, 0);
        });

        constexpr int numThreads = 4;
        constexpr int framesPerThread = 100;

        std::vector<std::thread> threads;
        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&, t]() {
                std::vector<uint8_t> payload(500);
                for (int i = 0; i < framesPerThread; i++) {
                    (void)sender.send(payload, 0x01, t * 1000 + i, t * 1000 + i, 0,
                               static_cast<uint8_t>(t + 1));
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        REQUIRE(waitFor([&]() {
            return received.load() == numThreads * framesPerThread;
        }, std::chrono::milliseconds(5000)));
    }

    // =========================================================================
    // Graceful shutdown under load
    // =========================================================================
    TEST_CASE("Graceful shutdown under load") {
        for (int iter = 0; iter < 10; iter++) {
            efp::Sender sender(MTU);
            auto receiver = std::make_unique<efp::Receiver<>>(50, 0);

            std::atomic<bool> stopSending{false};
            std::atomic<size_t> received{0};

            receiver->setCallback([&](efp::SuperFramePtr) {
                received++;
            });

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                (void)receiver->receive(data, size, 0);
            });

            // Start sending in background
            std::thread sendThread([&]() {
                std::vector<uint8_t> payload(1000);
                int i = 0;
                while (!stopSending.load()) {
                    int idx = i++;
                    (void)sender.send(payload, 0x01, idx, idx, 0, 1);
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            });

            // Let it run for a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Stop and destroy receiver while still sending
            stopSending = true;
            sendThread.join();

            receiver->stop();
            receiver.reset();  // Destroy receiver

            CHECK(received.load() > 0);  // Should have received some frames
        }

        CHECK(true);  // If we get here without crash, success
    }

    // =========================================================================
    // Receiver stop is idempotent
    // =========================================================================
    TEST_CASE("Receiver stop is idempotent") {
        efp::Receiver receiver(100, 0);

        // Stop multiple times should not crash
        receiver.stop();
        receiver.stop();
        receiver.stop();

        CHECK(true);
    }

    // =========================================================================
    // Receiver works after stop and before destruction
    // =========================================================================
    TEST_CASE("Receiver after stop") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<int> received{0};

        receiver.setCallback([&](efp::SuperFramePtr) {
            received++;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);
        (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]() { return received.load() == 1; }));

        receiver.stop();

        // After stop, new data should still be accepted but may not be delivered
        auto result = receiver.receive(payload.data(), payload.size(), 0);
        // The result depends on implementation - shouldn't crash
        CHECK(true);
    }

    // =========================================================================
    // Run-to-completion poll behavior
    // =========================================================================
    TEST_CASE("Run-to-completion poll delivers frames") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0, efp::ReceiverMode::RUN_TO_COMPLETION);

        std::atomic<int> received{0};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(!frame->mBroken);
            received++;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        // Send multiple frames
        std::vector<uint8_t> payload(100);
        for (int i = 0; i < 10; i++) {
            (void)sender.send(payload, 0x01, i, i, 0, 1);
        }

        // All frames should be delivered synchronously in run-to-completion mode
        // since each receive() call processes and delivers complete frames
        CHECK(received.load() == 10);
    }

    // =========================================================================
    // Version consistency
    // =========================================================================
    TEST_CASE("Version consistency") {
        CHECK(efp::Sender<>::version() == efp::VERSION);
        CHECK(efp::Receiver<>::version() == efp::VERSION);
        CHECK(efp_version() == efp::VERSION);

        uint8_t major = efp::VERSION >> 8;
        uint8_t minor = efp::VERSION & 0xFF;
        CHECK(major == efp::VERSION_MAJOR);
        CHECK(minor == efp::VERSION_MINOR);
    }

    // =========================================================================
    // Minimum MTU enforcement
    // =========================================================================
    TEST_CASE("Minimum MTU enforcement") {
        // MTU below 256 should be increased to 256
        efp::Sender sender(100);  // Too small

        std::atomic<bool> sent{false};
        sender.setCallback([&](const uint8_t*, size_t, uint8_t) {
            sent = true;
        });

        std::vector<uint8_t> payload(50);
        auto result = sender.send(payload, 0x01, 1000, 1000, 0, 1);

        CHECK(result == efp::Result::OK);
        CHECK(sent.load());
    }

}

