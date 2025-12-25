//
// EFP Unit Tests - Fragment Ordering
//
// Tests for out-of-order fragment and packet delivery
// Ported from old UnitTest7, 8, 10, 11, 12, 23
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

TEST_SUITE("Fragment Ordering") {

    // =========================================================================
    // UnitTest7: Swap fragment order (1,3,2,4,5,6) and verify data integrity
    // =========================================================================
    TEST_CASE("Swap fragment order 1-3-2-4-5-6 (UnitTest7)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> dataReceived{false};

        size_t packetNumber = 0;
        std::vector<uint8_t> savedSubPacketNumber2;

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t streamId) {
            CHECK(streamId == 8);
            packetNumber++;

            std::vector<uint8_t> packet(data, data + size);

            if (packetNumber == 2) {
                // Hold packet number 2
                savedSubPacketNumber2 = packet;
                return;
            } else if (packetNumber == 3) {
                // First send packet number 3, then packet number 2
                auto result = receiver.receive(packet.data(), packet.size(), 0);
                CHECK(result == efp::Result::OK);

                result = receiver.receive(savedSubPacketNumber2.data(), savedSubPacketNumber2.size(), 0);
                CHECK(result == efp::Result::OK);
                return;
            }

            auto result = receiver.receive(packet.data(), packet.size(), 0);
            CHECK(result == efp::Result::OK);
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->mStreamId == 8);
            CHECK(frame->mPts == 1001);
            CHECK(frame->mPayloadCode == 2);
            CHECK(!frame->mBroken);
            CHECK(frame->mSize == FRAME_SIZE);

            // Verify data integrity
            uint8_t vectorChecker = 0;
            for (size_t x = 0; x < frame->mSize; x++) {
                CHECK(frame->mpData[x] == vectorChecker++);
            }
            dataReceived = true;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);
        std::generate(mydata.begin(), mydata.end(), [n = 0]() mutable { return static_cast<uint8_t>(n++); });

        auto result = sender.send(mydata, 0x02, 1001, 1, 2, 8);
        CHECK(result == efp::Result::OK);

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest8: Receive Type2 first, then Type1 fragments
    // =========================================================================
    TEST_CASE("Receive Type2 first then Type1 (UnitTest8)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> dataReceived{false};

        std::vector<std::vector<uint8_t>> dataKeptBack;

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            std::vector<uint8_t> packet(data, data + size);

            if ((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::TYPE2)) {
                // Type2 fragment => last fragment. Send it first
                auto result = receiver.receive(packet.data(), packet.size(), 0);
                CHECK(result == efp::Result::OK);

                // Send the rest of the fragments, swap first two
                std::swap(dataKeptBack[0], dataKeptBack[1]);
                for (auto& x : dataKeptBack) {
                    result = receiver.receive(x.data(), x.size(), 0);
                    CHECK(result == efp::Result::OK);
                }
            } else {
                // Not the last fragment, keep it back
                dataKeptBack.push_back(packet);
            }
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->mStreamId == 8);
            CHECK(frame->mPts == 1001);
            CHECK(frame->mPayloadCode == 2);
            CHECK(!frame->mBroken);
            CHECK(frame->mSize == FRAME_SIZE);

            // Verify data integrity
            uint8_t vectorChecker = 0;
            for (size_t x = 0; x < frame->mSize; x++) {
                CHECK(frame->mpData[x] == vectorChecker++);
            }
            dataReceived = true;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);
        std::generate(mydata.begin(), mydata.end(), [n = 0]() mutable { return static_cast<uint8_t>(n++); });

        auto result = sender.send(mydata, 0x02, 1001, 1, 2, 8);
        CHECK(result == efp::Result::OK);

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest10: Send two Type2 packets out of order, receive in order
    // =========================================================================
    TEST_CASE("Two Type2 frames out of order (UnitTest10)") {
        const size_t FRAME_SIZE = MTU - sizeof(efp::FrameType2);

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<size_t> dataReceived{0};

        size_t packetNumber = 0;
        std::vector<uint8_t> fragmentKeptBack;

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            std::vector<uint8_t> packet(data, data + size);
            packetNumber++;

            if (packetNumber == 1) {
                fragmentKeptBack = packet;
            } else if (packetNumber == 2) {
                // Send packet 2 first
                auto result = receiver.receive(packet.data(), packet.size(), 0);
                CHECK(result == efp::Result::OK);

                // Then send packet 1
                result = receiver.receive(fragmentKeptBack.data(), fragmentKeptBack.size(), 0);
                CHECK(result == efp::Result::OK);
            }

            CHECK(packetNumber <= 2);
        });

        size_t receivedFrameNumber = 0;
        receiver.setCallback([&](efp::SuperFramePtr frame) {
            receivedFrameNumber++;

            if (receivedFrameNumber == 1) {
                CHECK(frame->mPts == 1001);
            }
            if (receivedFrameNumber == 2) {
                CHECK(frame->mPts == 1002);
            }

            CHECK(frame->mStreamId == 1);
            CHECK(frame->mPayloadCode == 0);
            CHECK(!frame->mBroken);
            CHECK(frame->mSize == FRAME_SIZE);

            dataReceived++;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);

        auto result = sender.send(mydata, 0x83, 1001, 1, 0, 1);
        CHECK(result == efp::Result::OK);

        result = sender.send(mydata, 0x83, 1002, 2, 0, 1);
        CHECK(result == efp::Result::OK);

        REQUIRE(waitFor([&]() { return dataReceived.load() == 2; }));
    }

    // =========================================================================
    // UnitTest11: 5 packets in reversed order, drop middle
    // =========================================================================
    TEST_CASE("Five packets reversed order drop middle (UnitTest11)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<size_t> dataReceived{0};

        size_t sentSuperFrameNumber = 0;
        std::vector<std::vector<uint8_t>> keptBackFragments;
        std::vector<std::vector<std::vector<uint8_t>>> keptBackSuperFrames;

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            std::vector<uint8_t> packet(data, data + size);

            if ((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::TYPE2)) {
                sentSuperFrameNumber++;
                keptBackFragments.push_back(packet);
                keptBackSuperFrames.push_back(keptBackFragments);

                if (sentSuperFrameNumber == 5) {
                    // Deliver in reversed order, skip packet 3
                    for (size_t superFrame = keptBackSuperFrames.size(); superFrame > 0; superFrame--) {
                        if (superFrame == 3) {
                            continue;  // Drop packet number 3
                        }

                        for (auto& x : keptBackSuperFrames[superFrame - 1]) {
                            auto result = receiver.receive(x.data(), x.size(), 0);
                            CHECK(result == efp::Result::OK);
                        }
                    }
                }
                keptBackFragments.clear();
            } else {
                keptBackFragments.push_back(packet);
            }
        });

        size_t receivedFrameNumber = 0;
        int64_t nextExpectedPts = 1001;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            receivedFrameNumber++;
            CHECK(frame->mPts == nextExpectedPts);
            // Expect pts 1003 to be missing
            nextExpectedPts += (nextExpectedPts == 1002 ? 2 : 1);

            CHECK(frame->mStreamId == 1);
            CHECK(frame->mPayloadCode == 0);
            CHECK(!frame->mBroken);
            CHECK(frame->mSize == FRAME_SIZE);

            dataReceived++;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);

        for (size_t packetNumber = 0; packetNumber < 5; packetNumber++) {
            auto result = sender.send(mydata, 0x83, packetNumber + 1001, packetNumber + 1, 0, 1);
            REQUIRE(result == efp::Result::OK);
        }

        REQUIRE(waitFor([&]() { return dataReceived.load() == 4; }));
    }

    // =========================================================================
    // UnitTest12: Reversed order + reversed fragment order within each
    // =========================================================================
    TEST_CASE("Reversed order with reversed fragments (UnitTest12)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 40);

        std::atomic<size_t> dataReceived{0};

        size_t sentSuperFrameNumber = 0;
        std::vector<std::vector<uint8_t>> keptBackFragments;
        std::vector<std::vector<std::vector<uint8_t>>> keptBackSuperFrames;

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            std::vector<uint8_t> packet(data, data + size);

            if ((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::TYPE2)) {
                sentSuperFrameNumber++;
                keptBackFragments.push_back(packet);
                keptBackSuperFrames.push_back(keptBackFragments);

                if (sentSuperFrameNumber == 5) {
                    // Deliver superframes in reverse, and fragments in reverse
                    for (size_t item = keptBackSuperFrames.size(); item > 0; item--) {
                        if (item == 3) {
                            continue;  // Drop packet 3
                        }
                        std::vector<std::vector<uint8_t>> superFrame = keptBackSuperFrames[item - 1];
                        for (size_t fragment = superFrame.size(); fragment > 0; fragment--) {
                            auto result = receiver.receive(superFrame[fragment - 1].data(),
                                                          superFrame[fragment - 1].size(), 0);
                            CHECK(result == efp::Result::OK);
                        }
                    }
                }
                keptBackFragments.clear();
                return;
            }
            keptBackFragments.push_back(packet);
        });

        size_t receivedFrameNumber = 0;
        int64_t nextExpectedPts = 1001;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            receivedFrameNumber++;
            CHECK(frame->mPts == nextExpectedPts);
            // Expect pts 1003 to be missing
            nextExpectedPts += (nextExpectedPts == 1002 ? 2 : 1);

            CHECK(frame->mStreamId == 1);
            CHECK(frame->mPayloadCode == 0);
            CHECK(!frame->mBroken);
            CHECK(frame->mSize == FRAME_SIZE);

            dataReceived++;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);

        for (size_t packetNumber = 0; packetNumber < 5; packetNumber++) {
            auto result = sender.send(mydata, 0x83, packetNumber + 1001, packetNumber + 1, 0, 1);
            REQUIRE(result == efp::Result::OK);
        }

        REQUIRE(waitFor([&]() { return dataReceived.load() == 4; }));
    }

    // =========================================================================
    // UnitTest23: 10 packets, drop 4&5, deliver rest reversed with reversed fragments
    // =========================================================================
    TEST_CASE("Ten packets drop 4 and 5 reversed (UnitTest23)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 40);

        std::atomic<size_t> dataReceived{0};

        size_t sentSuperFrameNumber = 0;
        std::vector<std::vector<uint8_t>> keptBackFragments;
        std::vector<std::vector<std::vector<uint8_t>>> keptBackSuperFrames;

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            std::vector<uint8_t> packet(data, data + size);

            if ((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::TYPE2)) {
                sentSuperFrameNumber++;
                keptBackFragments.push_back(packet);
                keptBackSuperFrames.push_back(keptBackFragments);

                if (sentSuperFrameNumber == 10) {
                    for (size_t item = keptBackSuperFrames.size(); item > 0; item--) {
                        if (item == 4 || item == 5) {
                            continue;  // Drop packets 4 and 5
                        }
                        std::vector<std::vector<uint8_t>> superFrame = keptBackSuperFrames[item - 1];
                        for (size_t fragment = superFrame.size(); fragment > 0; fragment--) {
                            auto result = receiver.receive(superFrame[fragment - 1].data(),
                                                          superFrame[fragment - 1].size(), 0);
                            CHECK(result == efp::Result::OK);
                        }
                    }
                }
                keptBackFragments.clear();
                return;
            }
            keptBackFragments.push_back(packet);
        });

        size_t receivedFrameNumber = 0;
        int64_t nextExpectedPts = 1001;
        uint16_t lastReceivedSuperFrame = 0;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            receivedFrameNumber++;
            CHECK(frame->mPts == nextExpectedPts);
            // Expect pts 1004 and 1005 to be missing
            nextExpectedPts += (nextExpectedPts == 1003 ? 3 : 1);

            CHECK(frame->mStreamId == 1);
            CHECK(frame->mPayloadCode == 0);
            CHECK(!frame->mBroken);
            CHECK(frame->mSize == FRAME_SIZE);

            if (frame->mPts == 1006) {
                // Check that we know we lost 2 super frames here
                CHECK(lastReceivedSuperFrame + 3 == frame->mSuperFrameNo);
            } else if (frame->mPts != 1001) {
                CHECK(lastReceivedSuperFrame + 1 == frame->mSuperFrameNo);
            }
            lastReceivedSuperFrame = frame->mSuperFrameNo;

            dataReceived++;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);

        for (size_t packetNumber = 0; packetNumber < 10; packetNumber++) {
            auto result = sender.send(mydata, 0x83, packetNumber + 1001, packetNumber + 1, 0, 1);
            REQUIRE(result == efp::Result::OK);
        }

        REQUIRE(waitFor([&]() { return dataReceived.load() == 8; }));
    }

    // =========================================================================
    // Extended: Extremely shuffled fragments
    // =========================================================================
    TEST_CASE("Extremely shuffled fragments (random order)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 10) + 50;

        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> dataReceived{false};

        std::vector<std::vector<uint8_t>> allFragments;

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            allFragments.emplace_back(data, data + size);
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);
        std::generate(mydata.begin(), mydata.end(), [n = 0]() mutable { return static_cast<uint8_t>(n++); });

        auto result = sender.send(mydata, 0x01, 1000, 1000, 0, 1);
        CHECK(result == efp::Result::OK);

        // Shuffle fragments randomly
        std::mt19937 rng(42);
        std::shuffle(allFragments.begin(), allFragments.end(), rng);

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->mSize == FRAME_SIZE);
            CHECK(!frame->mBroken);

            // Verify data
            uint8_t vectorChecker = 0;
            for (size_t x = 0; x < frame->mSize; x++) {
                CHECK(frame->mpData[x] == vectorChecker++);
            }
            dataReceived = true;
        });

        // Send shuffled fragments
        for (auto& frag : allFragments) {
            (void)receiver.receive(frag.data(), frag.size(), 0);
        }

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

}

