//
// EFP Unit Tests - Packet Loss and Corruption
//
// Tests for fragment loss, corruption, and timeout handling
// Ported from old UnitTest6, 9, 16, 22, 24
//

#include <doctest/doctest.h>

#include "efp.h"
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

TEST_SUITE("Packet Loss") {

    // =========================================================================
    // UnitTest6: Drop first Type1 fragment, verify broken + partial data
    // =========================================================================
    TEST_CASE("Drop first Type1 fragment (UnitTest6)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 2) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> dataReceived{false};

        size_t packetNumber = 0;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            packetNumber++;
            if (packetNumber == 1) {
                return;  // Drop the first packet
            }
            auto result = receiver.receive(data, size, 0);
            CHECK(result == efp::Result::OK);
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->mStreamId == 1);
            CHECK(frame->mPts == 1001);
            CHECK(frame->mPayloadCode == 2);
            CHECK(frame->mBroken);  // Must be marked as broken

            // One block of MTU is gone, but size should still be correct
            CHECK(frame->mSize == FRAME_SIZE);

            // Verify remaining data starting from second fragment
            size_t type1PayloadSize = MTU - sizeof(efp::FrameType1);
            uint8_t vectorChecker = static_cast<uint8_t>(type1PayloadSize % 256);
            for (size_t x = type1PayloadSize; x < frame->mSize; x++) {
                CHECK(frame->mpData[x] == vectorChecker++);
            }
            dataReceived = true;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);
        std::generate(mydata.begin(), mydata.end(), [n = 0]() mutable { return static_cast<uint8_t>(n++); });

        auto result = sender.send(mydata, 0x02, 1001, 1, 2, 1);
        CHECK(result == efp::Result::OK);

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest9: Drop Type2 packet, verify PTS=MAX, broken=true
    // =========================================================================
    TEST_CASE("Drop Type2 packet (UnitTest9)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> dataReceived{false};

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            if ((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::TYPE2)) {
                return;  // Drop the Type2 packet
            }
            auto result = receiver.receive(data, size, 0);
            CHECK(result == efp::Result::OK);
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->mStreamId == 1);
            CHECK(frame->mPts == UINT64_MAX);  // No PTS without Type2
            CHECK(frame->mPayloadCode == UINT32_MAX);  // No code without Type2
            CHECK(frame->mBroken);  // Must be marked as broken

            // When Type2 is dropped, the receiver allocates based on ofFragmentNo + 1 fragments
            // of type1PayloadSize each (since it doesn't know Type2's actual size)
            size_t type1PayloadSize = MTU - sizeof(efp::FrameType1);
            CHECK(frame->mSize == type1PayloadSize * 6);  // 6 fragments allocated (0-5), each type1PayloadSize

            // Verify data from Type1 fragments (first 5 * type1PayloadSize bytes are valid)
            uint8_t vectorChecker = 0;
            for (size_t x = 0; x < type1PayloadSize * 5; x++) {
                CHECK(frame->mpData[x] == vectorChecker++);
            }
            dataReceived = true;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);
        std::generate(mydata.begin(), mydata.end(), [n = 0]() mutable { return static_cast<uint8_t>(n++); });

        auto result = sender.send(mydata, 0x02, 1001, 1, 2, 1);
        CHECK(result == efp::Result::OK);

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest22: HOL blocking with delayed fragment after timeout
    // =========================================================================
    TEST_CASE("HOL blocking delayed fragment (UnitTest22)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(20, 20, efp::ReceiverMode::RUN_TO_COMPLETION);

        std::atomic<size_t> dataReceived{0};

        size_t sentFragmentNumber = 0;
        std::vector<uint8_t> savedFragment;

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            std::vector<uint8_t> packet(data, data + size);
            sentFragmentNumber++;

            if (sentFragmentNumber == 8) {
                // Skip fragment 8 (second fragment in second super frame)
                return;
            }

            if (sentFragmentNumber == 11) {
                // Save fragment 11 for later
                savedFragment = packet;
                return;
            }

            auto result = receiver.receive(packet.data(), packet.size(), 0);
            CHECK(result == efp::Result::OK);

            if (sentFragmentNumber == 18) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                receiver.poll();  // Trigger timeout
                // Send saved fragment - it may be Ok or FragmentTooOld depending on timing
                result = receiver.receive(savedFragment.data(), savedFragment.size(), 0);
                CHECK((result == efp::Result::OK || result == efp::Result::FRAGMENT_TOO_OLD));
                return;
            }

            if (sentFragmentNumber == 24) {
                // Send saved old fragment again
                result = receiver.receive(savedFragment.data(), savedFragment.size(), 0);
                // The fragment is too old, should be signaled as such
                CHECK(result == efp::Result::FRAGMENT_TOO_OLD);
                return;
            }
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            if (frame->mPts == 1002) {
                CHECK(frame->mBroken);  // Frame 2 should be broken
            } else {
                CHECK(!frame->mBroken);

                uint8_t vectorChecker = 0;
                for (size_t x = 0; x < FRAME_SIZE; x++) {
                    CHECK(frame->mpData[x] == vectorChecker++);
                }
            }
            dataReceived++;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);
        std::generate(mydata.begin(), mydata.end(), [n = 0]() mutable { return static_cast<uint8_t>(n++); });

        for (size_t packetNumber = 0; packetNumber < 4; packetNumber++) {
            auto result = sender.send(mydata, 0x83, packetNumber + 1001, packetNumber + 1, 0, 1);
            REQUIRE(result == efp::Result::OK);
        }

        // Final poll to ensure all frames are delivered
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        receiver.poll();

        REQUIRE(waitFor([&]() { return dataReceived.load() == 4; }, std::chrono::milliseconds(1000)));
    }

    // =========================================================================
    // UnitTest24: Fuzz test with 10,000 garbage packets
    // =========================================================================
    TEST_CASE("Fuzz test garbage packets (UnitTest24)" * doctest::timeout(60)) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<unsigned int> dis(0, 255);
        std::uniform_int_distribution<size_t> sizeDis(1, 10000);

        efp::Receiver receiver(50, 20);
        efp::Sender sender(MTU);

        // Receiver should not crash with garbage data
        receiver.setCallback([&](efp::SuperFramePtr) {
            // May or may not receive anything - that's ok
        });

        for (int i = 0; i < 10000; i++) {
            // Generate random garbage
            size_t garbageSize = sizeDis(gen);
            std::vector<uint8_t> garbage(garbageSize);
            std::generate(garbage.begin(), garbage.end(), [&]() {
                return static_cast<uint8_t>(dis(gen));
            });

            // Should not crash
            (void)receiver.receive(garbage.data(), garbage.size(), 0);
        }

        CHECK(true);  // If we get here without crash, success
    }

    // =========================================================================
    // UnitTest16 variant: Random loss, broken frames, and reordering
    // =========================================================================
    TEST_CASE("Random loss and reordering (UnitTest16)" * doctest::timeout(30)) {
        constexpr uint32_t LOSS_RATE = 2;      // 2% lost frames
        constexpr uint32_t BROKEN_RATE = 2;    // 2% broken frames
        constexpr uint32_t REORDER_RATE = 10;  // 10% reordered
        constexpr uint32_t NUM_PACKETS = 200;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<size_t> totalReceived{0};
        std::atomic<size_t> brokenReceived{0};

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dis(0, 99);

        uint64_t brokenCounter = 0;
        std::vector<std::vector<uint8_t>> reorderBuffer;

        bool currentLoss = false;
        bool currentBroken = false;
        bool currentReorder = false;

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            std::vector<uint8_t> packet(data, data + size);

            if (currentLoss) {
                return;  // Drop entire superframe
            }

            if (currentBroken) {
                if (!(brokenCounter % 5)) {
                    brokenCounter++;
                    return;  // Drop some fragments
                }
                brokenCounter++;
            }

            if (currentReorder) {
                if ((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::TYPE1)) {
                    reorderBuffer.push_back(packet);
                    return;
                } else if ((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::TYPE2)) {
                    reorderBuffer.push_back(packet);
                    std::shuffle(reorderBuffer.begin(), reorderBuffer.end(), gen);
                    for (auto& p : reorderBuffer) {
                        (void)receiver.receive(p.data(), p.size(), 0);
                    }
                    reorderBuffer.clear();
                    return;
                } else if ((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::TYPE3)) {
                    reorderBuffer.push_back(packet);
                    return;
                }
            }

            (void)receiver.receive(packet.data(), packet.size(), 0);
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            totalReceived++;
            if (frame->mBroken) {
                brokenReceived++;
            }
        });

        std::uniform_int_distribution<size_t> sizeDis(1, 10000);

        for (uint32_t i = 0; i < NUM_PACKETS; i++) {
            // Determine behavior for this packet
            uint32_t roll = dis(gen);
            currentLoss = (roll < LOSS_RATE);
            currentBroken = (roll >= LOSS_RATE && roll < LOSS_RATE + BROKEN_RATE);
            currentReorder = (roll >= LOSS_RATE + BROKEN_RATE &&
                             roll < LOSS_RATE + BROKEN_RATE + REORDER_RATE);

            size_t randSize = sizeDis(gen);
            std::vector<uint8_t> mydata(randSize);
            std::generate(mydata.begin(), mydata.end(), [n = 0]() mutable {
                return static_cast<uint8_t>(n++);
            });

            (void)sender.send(mydata, 0x83, i + 1000, i, 0, 1);
        }

        REQUIRE(waitFor([&]() {
            // We should receive most packets (minus losses)
            return totalReceived.load() >= NUM_PACKETS * 0.9;  // At least 90%
        }, std::chrono::milliseconds(5000)));
    }

    // =========================================================================
    // Drop middle fragments
    // =========================================================================
    TEST_CASE("Drop middle fragments") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> dataReceived{false};

        size_t fragmentNumber = 0;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragmentNumber++;
            // Drop fragments 2 and 3
            if (fragmentNumber == 2 || fragmentNumber == 3) {
                return;
            }
            (void)receiver.receive(data, size, 0);
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->mBroken);  // Must be broken due to missing fragments
            dataReceived = true;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);

        auto result = sender.send(mydata, 0x01, 1000, 1000, 0, 1);
        CHECK(result == efp::Result::OK);

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

    // =========================================================================
    // Drop all but last fragment (Type2 only)
    // =========================================================================
    TEST_CASE("Drop all but Type2 fragment") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> dataReceived{false};

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            // Only send Type2
            if ((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::TYPE2)) {
                (void)receiver.receive(data, size, 0);
            }
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->mBroken);  // Must be broken - missing all Type1
            CHECK(frame->mPts == 1000);  // Type2 carries metadata
            dataReceived = true;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);

        auto result = sender.send(mydata, 0x01, 1000, 1000, 0, 1);
        CHECK(result == efp::Result::OK);

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

    // =========================================================================
    // Duplicate fragment handling
    // =========================================================================
    TEST_CASE("Duplicate fragment handling") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(!frame->mBroken);
            received = true;
        });

        std::vector<std::vector<uint8_t>> fragments;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        // Large payload to generate multiple fragments
        std::vector<uint8_t> payload(MTU * 3);
        (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

        // Send each fragment twice
        for (auto& frag : fragments) {
            auto result1 = receiver.receive(frag.data(), frag.size(), 0);
            CHECK(result1 == efp::Result::OK);

            auto result2 = receiver.receive(frag.data(), frag.size(), 0);
            CHECK(result2 == efp::Result::DUPLICATE_FRAGMENT);
        }

        REQUIRE(waitFor([&]() { return received.load(); }));
    }

    // =========================================================================
    // Timeout triggers broken frame delivery
    // =========================================================================
    TEST_CASE("Timeout delivers broken frame") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(30, 0);  // 30ms timeout

        std::atomic<bool> received{false};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->mBroken);
            received = true;
        });

        std::vector<std::vector<uint8_t>> fragments;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        std::vector<uint8_t> payload(MTU * 3);
        (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

        // Only send first fragment
        (void)receiver.receive(fragments[0].data(), fragments[0].size(), 0);

        // Wait for timeout
        REQUIRE(waitFor([&]() { return received.load(); }, std::chrono::milliseconds(200)));
    }

    // =========================================================================
    // Late fragment after delivery (tooOldFragment)
    // =========================================================================
    TEST_CASE("Fragment after frame delivered") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(30, 0);  // 30ms timeout

        std::atomic<int> receivedCount{0};

        receiver.setCallback([&](efp::SuperFramePtr) {
            receivedCount++;
        });

        std::vector<std::vector<uint8_t>> fragments;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        std::vector<uint8_t> payload(MTU * 3);
        (void)sender.send(payload, 0x01, 1000, 1000, 0, 1);

        // Send only first fragment
        (void)receiver.receive(fragments[0].data(), fragments[0].size(), 0);

        // Wait for timeout delivery
        REQUIRE(waitFor([&]() { return receivedCount.load() == 1; }, std::chrono::milliseconds(200)));

        // Now send remaining fragments - should be rejected as too old
        for (size_t i = 1; i < fragments.size(); i++) {
            auto result = receiver.receive(fragments[i].data(), fragments[i].size(), 0);
            CHECK(result == efp::Result::FRAGMENT_TOO_OLD);
        }
    }

}

