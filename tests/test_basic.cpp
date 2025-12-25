//
// EFP Unit Tests - Basic Functionality
//
// Basic tests covering fundamental send/receive operations
// Ported from old UnitTest1, 2, 3, 4, 5
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
#include <set>
#include <mutex>

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

TEST_SUITE("Basic Functionality") {

    // =========================================================================
    // UnitTest1: Small packet results in Type2 frame only
    // =========================================================================
    TEST_CASE("Small packet Type2 only (UnitTest1)") {
        efp::Sender sender(MTU);

        sender.setCallback([](const uint8_t* data, size_t, uint8_t) {
            CHECK((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::Type2));
        });

        std::vector<uint8_t> mydata(MTU - sizeof(efp::FrameType2));

        auto result = sender.send(mydata, 0x02, 1001, 1, 2, 1);
        CHECK(result == efp::Result::Ok);
    }

    // =========================================================================
    // UnitTest2: Type2 frame send/receive with metadata verification
    // =========================================================================
    TEST_CASE("Type2 frame roundtrip (UnitTest2)") {
        const size_t FRAME_SIZE = MTU - sizeof(efp::FrameType2);

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> dataReceived{false};

        size_t dataSent = 0;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            auto result = receiver.receive(data, size, 0);
            CHECK(result == efp::Result::Ok);
            dataSent++;
            CHECK(dataSent <= 1);  // Should only be one fragment
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->pts == 1001);
            CHECK(frame->payloadCode == 2);
            CHECK(!frame->broken);
            CHECK(frame->payloadType == 0x02);
            CHECK(frame->size == FRAME_SIZE);
            dataReceived = true;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);

        auto result = sender.send(mydata, 0x02, 1001, 1, 2, 1);
        CHECK(result == efp::Result::Ok);

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest3: Single byte payload (0xAA)
    // =========================================================================
    TEST_CASE("Single byte payload (UnitTest3)") {
        const size_t FRAME_SIZE = 1;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> dataReceived{false};

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            auto result = receiver.receive(data, size, 0);
            CHECK(result == efp::Result::Ok);
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->pts == 1001);
            CHECK(frame->payloadCode == 2);
            CHECK(!frame->broken);
            CHECK(frame->payloadType == 0x02);
            CHECK(frame->size == FRAME_SIZE);
            CHECK(frame->data[0] == 0xaa);
            dataReceived = true;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);
        mydata[0] = 0xaa;

        auto result = sender.send(mydata, 0x02, 1001, 1, 2, 1);
        CHECK(result == efp::Result::Ok);

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest4: Packet requiring Type1 + Type2
    // =========================================================================
    TEST_CASE("Type1 and Type2 combined (UnitTest4)") {
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) + 1;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> dataReceived{false};

        size_t packetNumber = 0;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            if (packetNumber == 0) {
                // First should be Type1
                CHECK((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::Type1));
                CHECK(size == MTU);
            } else if (packetNumber == 1) {
                // Second should be Type2
                CHECK((data[0] & 0x0f) == static_cast<uint8_t>(efp::FrameType::Type2));
                CHECK(size == sizeof(efp::FrameType2) + 1);
            }
            CHECK(packetNumber < 2);
            packetNumber++;

            auto result = receiver.receive(data, size, 0);
            CHECK(result == efp::Result::Ok);
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->streamId == 4);
            CHECK(frame->pts == 1001);
            CHECK(frame->payloadCode == 2);
            CHECK(!frame->broken);
            CHECK(frame->size == FRAME_SIZE);
            dataReceived = true;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);

        auto result = sender.send(mydata, 0x02, 1001, 1, 2, 4);
        CHECK(result == efp::Result::Ok);

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest5: Linear vector over multiple fragments with data integrity
    // =========================================================================
    TEST_CASE("Linear vector multiple fragments (UnitTest5)") {
        const size_t FRAME_SIZE = (MTU * 5) + (MTU / 2);

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> dataReceived{false};

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            auto result = receiver.receive(data, size, 0);
            CHECK(result == efp::Result::Ok);
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->streamId == 1);
            CHECK(frame->pts == 1001);
            CHECK(frame->payloadCode == 2);
            CHECK(!frame->broken);
            CHECK(frame->size == FRAME_SIZE);

            // Verify linear data
            uint8_t vectorChecker = 0;
            for (size_t x = 0; x < frame->size; x++) {
                CHECK(frame->data[x] == vectorChecker++);
            }
            dataReceived = true;
        });

        std::vector<uint8_t> mydata(FRAME_SIZE);
        std::generate(mydata.begin(), mydata.end(), [n = 0]() mutable {
            return static_cast<uint8_t>(n++);
        });

        auto result = sender.send(mydata, 0x02, 1001, 1, 2, 1);
        CHECK(result == efp::Result::Ok);

        REQUIRE(waitFor([&]() { return dataReceived.load(); }));
    }

    // =========================================================================
    // All payload types (0-255)
    // =========================================================================
    TEST_CASE("All 256 payload types") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<int> received{0};
        uint8_t lastPayloadType = 0;
        std::mutex mutex;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            std::lock_guard<std::mutex> lock(mutex);
            lastPayloadType = frame->payloadType;
            received++;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(50);

        // Test all 256 payload types
        for (int i = 0; i < 256; i++) {
            sender.send(payload, static_cast<uint8_t>(i), i, i, 0, 1);
        }

        REQUIRE(waitFor([&]() { return received.load() == 256; }, std::chrono::milliseconds(5000)));
    }

    // =========================================================================
    // All stream IDs (1-255, 0 reserved)
    // =========================================================================
    TEST_CASE("All stream IDs 1-255") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<int> received{0};
        std::set<uint8_t> receivedStreams;
        std::mutex mutex;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            std::lock_guard<std::mutex> lock(mutex);
            receivedStreams.insert(frame->streamId);
            received++;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(50);

        // Test stream IDs 1-255 (0 is reserved)
        for (int i = 1; i <= 255; i++) {
            sender.send(payload, 0x01, i, i, 0, static_cast<uint8_t>(i));
        }

        REQUIRE(waitFor([&]() { return received.load() == 255; }, std::chrono::milliseconds(5000)));

        std::lock_guard<std::mutex> lock(mutex);
        CHECK(receivedStreams.size() == 255);
    }

    // =========================================================================
    // FOURCC code generation and preservation
    // =========================================================================
    TEST_CASE("FOURCC code generation") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};
        uint32_t capturedCode = 0;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            capturedCode = frame->payloadCode;
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);

        // Test various FOURCC codes
        uint32_t anxbCode = EFP_CODE('A', 'N', 'X', 'B');
        CHECK(anxbCode == 0x414E5842);  // 'A'<<24 | 'N'<<16 | 'X'<<8 | 'B'

        sender.send(payload, 0x83, 1000, 1000, anxbCode, 1);

        REQUIRE(waitFor([&]() { return received.load(); }));
        CHECK(capturedCode == anxbCode);
    }

    // =========================================================================
    // Media types from efp_media_types.h
    // =========================================================================
    TEST_CASE("Media types usage") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->payloadType == efp::media::PayloadType::H264);
            CHECK(frame->payloadCode == efp::media::PayloadCode::ANXB);
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(1000);

        sender.send(payload,
                   efp::media::PayloadType::H264,
                   90000, 90000,
                   efp::media::PayloadCode::ANXB,
                   1);

        REQUIRE(waitFor([&]() { return received.load(); }));
    }

    // =========================================================================
    // PTS/DTS handling
    // =========================================================================
    TEST_CASE("PTS/DTS handling") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        SUBCASE("PTS == DTS") {
            std::atomic<bool> received{false};

            receiver.setCallback([&](efp::SuperFramePtr frame) {
                CHECK(frame->pts == 1000);
                CHECK(frame->dts == 1000);
                received = true;
            });

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                receiver.receive(data, size, 0);
            });

            std::vector<uint8_t> payload(100);
            sender.send(payload, 0x01, 1000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return received.load(); }));
        }

        SUBCASE("PTS > DTS") {
            std::atomic<bool> received{false};

            receiver.setCallback([&](efp::SuperFramePtr frame) {
                CHECK(frame->pts == 2000);
                CHECK(frame->dts == 1000);
                received = true;
            });

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                receiver.receive(data, size, 0);
            });

            std::vector<uint8_t> payload(100);
            sender.send(payload, 0x01, 2000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return received.load(); }));
        }

        SUBCASE("No DTS (UINT64_MAX)") {
            std::atomic<bool> received{false};

            receiver.setCallback([&](efp::SuperFramePtr frame) {
                CHECK(frame->pts == 1000);
                CHECK(frame->dts == UINT64_MAX);
                received = true;
            });

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                receiver.receive(data, size, 0);
            });

            std::vector<uint8_t> payload(100);
            sender.send(payload, 0x01, 1000, UINT64_MAX, 0, 1);

            REQUIRE(waitFor([&]() { return received.load(); }));
        }
    }

    // =========================================================================
    // Empty payload error
    // =========================================================================
    TEST_CASE("Empty payload returns error") {
        efp::Sender sender(MTU);

        auto result = sender.send(nullptr, 0, 0x01, 1000, 1000, 0, 1);
        CHECK(result == efp::Result::InvalidParameter);

        std::vector<uint8_t> empty;
        result = sender.send(empty, 0x01, 1000, 1000, 0, 1);
        CHECK(result == efp::Result::InvalidParameter);
    }

    // =========================================================================
    // Flags preservation
    // =========================================================================
    TEST_CASE("Flags preservation") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->flags == efp::Flags::InlinePayload);
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);
        sender.send(payload, 0x01, 1000, 1000, 0, 1, efp::Flags::InlinePayload);

        REQUIRE(waitFor([&]() { return received.load(); }));
    }

    // =========================================================================
    // Source ID passthrough
    // =========================================================================
    TEST_CASE("Source ID passthrough") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(frame->sourceId == 42);
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 42);  // Pass source ID
        });

        std::vector<uint8_t> payload(100);
        sender.send(payload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]() { return received.load(); }));
    }

    // =========================================================================
    // Exact boundary payloads
    // =========================================================================
    TEST_CASE("Exact MTU boundary payloads") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        SUBCASE("Exactly Type2 max payload") {
            std::atomic<bool> received{false};
            size_t capturedSize = 0;

            receiver.setCallback([&](efp::SuperFramePtr frame) {
                capturedSize = frame->size;
                CHECK(!frame->broken);
                received = true;
            });

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                receiver.receive(data, size, 0);
            });

            size_t maxType2Payload = MTU - sizeof(efp::FrameType2);
            std::vector<uint8_t> payload(maxType2Payload);
            sender.send(payload, 0x01, 1000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return received.load(); }));
            CHECK(capturedSize == maxType2Payload);
        }

        SUBCASE("One byte over Type2 max") {
            std::atomic<bool> received{false};
            int fragmentCount = 0;

            sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
                fragmentCount++;
                receiver.receive(data, size, 0);
            });

            receiver.setCallback([&](efp::SuperFramePtr frame) {
                CHECK(!frame->broken);
                received = true;
            });

            size_t payload_size = MTU - sizeof(efp::FrameType2) + 1;
            std::vector<uint8_t> payload(payload_size);
            sender.send(payload, 0x01, 1000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return received.load(); }));
            CHECK(fragmentCount == 2);  // Should be Type1 + Type2
        }
    }

}

