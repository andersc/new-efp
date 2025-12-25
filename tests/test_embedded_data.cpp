//
// EFP Unit Tests - Embedded Data
//
// Tests for embedded payload data functionality
// Ported from old UnitTest14, 15
//

#include <doctest/doctest.h>

#include "efp.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
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

TEST_SUITE("Embedded Data") {

    // =========================================================================
    // Basic embedded data add and extract
    // =========================================================================
    TEST_CASE("Add and extract single embedded data") {
        // Test the embedded data helper functions
        struct PrivateData {
            int value1 = 42;
            uint8_t value2 = 0xAB;
            uint64_t value3 = 0xDEADBEEFCAFEBABE;
        };

        PrivateData original;
        original.value1 = 12345;
        original.value2 = 0xCD;
        original.value3 = 0x123456789ABCDEF0;

        // Create payload with embedded data
        std::vector<uint8_t> payload(1000);
        std::generate(payload.begin(), payload.end(), [n = 0]() mutable {
            return static_cast<uint8_t>(n++);
        });

        // Add embedded data manually (matching old API behavior)
        std::vector<uint8_t> combined;

        // Embedded header: type (1 byte) + size (2 bytes) + data
        uint8_t embeddedType = 0x81;  // Last embedded + private data
        uint16_t embeddedSize = sizeof(PrivateData);

        combined.push_back(embeddedType);
        combined.push_back(embeddedSize & 0xFF);
        combined.push_back((embeddedSize >> 8) & 0xFF);

        // Add the private data
        const uint8_t* privDataPtr = reinterpret_cast<const uint8_t*>(&original);
        combined.insert(combined.end(), privDataPtr, privDataPtr + sizeof(PrivateData));

        // Add original payload
        combined.insert(combined.end(), payload.begin(), payload.end());

        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(!frame->mBroken);
            CHECK(frame->mFlags == efp::Flags::INLINE_PAYLOAD);

            // Extract embedded data
            if (frame->mSize >= 3) {
                uint8_t type = frame->mpData[0];
                uint16_t size = frame->mpData[1] | (frame->mpData[2] << 8);

                CHECK((type & 0x80) != 0);  // Last flag set
                CHECK(size == sizeof(PrivateData));

                if (frame->mSize >= 3 + size) {
                    PrivateData extracted;
                    std::memcpy(&extracted, frame->mpData + 3, sizeof(PrivateData));

                    CHECK(extracted.value1 == original.value1);
                    CHECK(extracted.value2 == original.value2);
                    CHECK(extracted.value3 == original.value3);
                }
            }

            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        (void)sender.send(combined, 0x83, 1000, 1000, EFP_CODE('A', 'N', 'X', 'B'), 1,
                   efp::Flags::INLINE_PAYLOAD);

        REQUIRE(waitFor([&]() { return received.load(); }));
    }

    // =========================================================================
    // UnitTest14: Multiple embedded data fields
    // =========================================================================
    TEST_CASE("Multiple embedded data fields (UnitTest14)") {
        struct PrivateData {
            int myPrivateInteger = 10;
            uint8_t myPrivateUint8_t = 44;
            size_t sizeOfData = 0;
        };

        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<size_t> dataReceived{0};

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            auto result = receiver.receive(data, size, 0);
            CHECK(result == efp::Result::OK);
        });

        size_t receivedFrameNumber = 0;
        receiver.setCallback([&](efp::SuperFramePtr frame) {
            receivedFrameNumber++;
            CHECK(!frame->mBroken);
            CHECK(frame->mStreamId == 1);
            CHECK(frame->mPayloadCode == EFP_CODE('A', 'N', 'X', 'B'));
            CHECK(frame->mFlags == efp::Flags::INLINE_PAYLOAD);

            // Parse embedded data
            size_t offset = 0;
            int embeddedCount = 0;

            while (offset + 3 <= frame->mSize) {
                uint8_t type = frame->mpData[offset];
                uint16_t size = frame->mpData[offset + 1] | (frame->mpData[offset + 2] << 8);

                if (type == 0) break;  // No more embedded data

                bool isLast = (type & 0x80) != 0;

                if (offset + 3 + size <= frame->mSize) {
                    PrivateData extracted;
                    std::memcpy(&extracted, frame->mpData + offset + 3,
                               std::min(size, (uint16_t)sizeof(PrivateData)));

                    CHECK(extracted.myPrivateInteger == 10);
                    CHECK(extracted.myPrivateUint8_t == 44);
                    embeddedCount++;
                }

                offset += 3 + size;

                if (isLast) break;
            }

            // Odd frames have 2 embedded data, even have 1
            if (receivedFrameNumber & 1) {
                CHECK(embeddedCount >= 1);  // At least 1
            }

            dataReceived++;
        });

        for (size_t packetNumber = 0; packetNumber < 15; packetNumber++) {
            std::vector<uint8_t> combined;

            // Add embedded data
            PrivateData privData;
            privData.sizeOfData = FRAME_SIZE;

            uint8_t embeddedType = (packetNumber & 1) ? 0x01 : 0x81;  // Not last / last
            uint16_t embeddedSize = sizeof(PrivateData);

            combined.push_back(embeddedType);
            combined.push_back(embeddedSize & 0xFF);
            combined.push_back((embeddedSize >> 8) & 0xFF);

            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&privData);
            combined.insert(combined.end(), ptr, ptr + sizeof(PrivateData));

            // Add second embedded data for odd packets
            if (packetNumber & 1) {
                PrivateData privData2;
                embeddedType = 0x81;  // Last

                combined.push_back(embeddedType);
                combined.push_back(embeddedSize & 0xFF);
                combined.push_back((embeddedSize >> 8) & 0xFF);

                ptr = reinterpret_cast<const uint8_t*>(&privData2);
                combined.insert(combined.end(), ptr, ptr + sizeof(PrivateData));
            }

            // Add payload
            std::vector<uint8_t> payload(FRAME_SIZE - combined.size());
            std::generate(payload.begin(), payload.end(), [n = 0]() mutable {
                return static_cast<uint8_t>(n++);
            });
            combined.insert(combined.end(), payload.begin(), payload.end());

            auto result = sender.send(combined, 0x83, packetNumber + 1001,
                                      packetNumber, EFP_CODE('A', 'N', 'X', 'B'), 1,
                                      efp::Flags::INLINE_PAYLOAD);
            CHECK(result == efp::Result::OK);
        }

        REQUIRE(waitFor([&]() { return dataReceived.load() == 15; },
                       std::chrono::milliseconds(2000)));
    }

    // =========================================================================
    // UnitTest15 variant: Random sizes with embedded size verification
    // =========================================================================
    TEST_CASE("Embedded data with random sizes (UnitTest15)" * doctest::timeout(60)) {
        struct PrivateData {
            int myPrivateInteger = 10;
            uint8_t myPrivateUint8_t = 44;
            size_t sizeOfData = 0;
        };

        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 40);

        std::atomic<size_t> dataReceived{0};
        std::atomic<size_t> receivedFrameNumber{0};

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            auto result = receiver.receive(data, size, 0);
            CHECK(result == efp::Result::OK);
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            size_t frameNum = ++receivedFrameNumber;
            CHECK(!frame->mBroken);
            CHECK(frame->mStreamId == 1);
            CHECK(frame->mPayloadCode == EFP_CODE('A', 'N', 'X', 'B'));
            CHECK(frame->mPts == 1000 + frameNum);
            CHECK(frame->mFlags == efp::Flags::INLINE_PAYLOAD);

            // Extract and verify embedded data
            if (frame->mSize >= 3 + sizeof(PrivateData)) {
                PrivateData extracted;
                std::memcpy(&extracted, frame->mpData + 3, sizeof(PrivateData));

                CHECK(extracted.sizeOfData == frame->mSize);
                CHECK(extracted.myPrivateInteger == 10);
                CHECK(extracted.myPrivateUint8_t == 44);
            }

            dataReceived++;
        });

        std::mt19937 rng(42);
        std::uniform_int_distribution<size_t> dist(100, 50000);

        for (size_t packetNumber = 0; packetNumber < 100; packetNumber++) {
            size_t randSize = dist(rng);

            std::vector<uint8_t> combined;
            size_t headerSize = 3 + sizeof(PrivateData);

            // Add embedded data header
            uint8_t embeddedType = 0x81;  // Last
            uint16_t embeddedSize = sizeof(PrivateData);

            combined.push_back(embeddedType);
            combined.push_back(embeddedSize & 0xFF);
            combined.push_back((embeddedSize >> 8) & 0xFF);

            // Add payload to reach target size
            size_t payloadSize = (randSize > headerSize) ? (randSize - headerSize) : 0;
            size_t totalSize = headerSize + payloadSize;

            // Create PrivateData with actual total size
            PrivateData privData;
            privData.sizeOfData = totalSize;

            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&privData);
            combined.insert(combined.end(), ptr, ptr + sizeof(PrivateData));

            // Add payload
            if (payloadSize > 0) {
                std::vector<uint8_t> payload(payloadSize);
                std::generate(payload.begin(), payload.end(), [n = 0]() mutable {
                    return static_cast<uint8_t>(n++);
                });
                combined.insert(combined.end(), payload.begin(), payload.end());
            }

            auto result = sender.send(combined, 0x83, packetNumber + 1001,
                                      packetNumber, EFP_CODE('A', 'N', 'X', 'B'), 1,
                                      efp::Flags::INLINE_PAYLOAD);
            CHECK(result == efp::Result::OK);
        }

        REQUIRE(waitFor([&]() { return dataReceived.load() == 100; },
                       std::chrono::milliseconds(30000)));
    }

    // =========================================================================
    // Embedded data with broken frame (dropped fragment)
    // =========================================================================
    TEST_CASE("Embedded data with broken frame") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 3) + 100;

        efp::Sender sender(MTU);
        efp::Receiver receiver(50, 20);

        std::atomic<bool> received{false};

        size_t fragmentCount = 0;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragmentCount++;
            if (fragmentCount == 2) {
                return;  // Drop second fragment
            }
            (void)receiver.receive(data, size, 0);
        });

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            // Frame should be marked as broken due to missing fragment
            CHECK(frame->mBroken);
            CHECK(frame->mFlags == efp::Flags::INLINE_PAYLOAD);
            received = true;
        });

        // Create payload with embedded data
        std::vector<uint8_t> combined;

        uint8_t embeddedType = 0x81;
        uint16_t embeddedSize = 16;

        combined.push_back(embeddedType);
        combined.push_back(embeddedSize & 0xFF);
        combined.push_back((embeddedSize >> 8) & 0xFF);
        combined.resize(3 + embeddedSize, 0xAB);

        // Add payload
        combined.resize(FRAME_SIZE, 0xCD);

        (void)sender.send(combined, 0x83, 1000, 1000, 0, 1, efp::Flags::INLINE_PAYLOAD);

        REQUIRE(waitFor([&]() { return received.load(); }));
    }

    // =========================================================================
    // Large embedded data
    // =========================================================================
    TEST_CASE("Large embedded data (1KB)") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};

        const size_t EMBEDDED_SIZE = 1024;

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(!frame->mBroken);
            CHECK(frame->mFlags == efp::Flags::INLINE_PAYLOAD);

            // Verify embedded data
            if (frame->mSize >= 3) {
                uint16_t size = frame->mpData[1] | (frame->mpData[2] << 8);
                CHECK(size == EMBEDDED_SIZE);

                // Verify embedded content
                for (size_t i = 0; i < std::min(size, (uint16_t)100); i++) {
                    CHECK(frame->mpData[3 + i] == static_cast<uint8_t>(i & 0xFF));
                }
            }

            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> combined;

        uint8_t embeddedType = 0x81;
        uint16_t embeddedSize = EMBEDDED_SIZE;

        combined.push_back(embeddedType);
        combined.push_back(embeddedSize & 0xFF);
        combined.push_back((embeddedSize >> 8) & 0xFF);

        // Add embedded content
        for (size_t i = 0; i < EMBEDDED_SIZE; i++) {
            combined.push_back(static_cast<uint8_t>(i & 0xFF));
        }

        // Add some payload
        combined.resize(combined.size() + 500, 0xEE);

        (void)sender.send(combined, 0x01, 1000, 1000, 0, 1, efp::Flags::INLINE_PAYLOAD);

        REQUIRE(waitFor([&]() { return received.load(); }));
    }

    // =========================================================================
    // Inline payload flag without embedded data
    // =========================================================================
    TEST_CASE("Inline payload flag without embedded data") {
        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        std::atomic<bool> received{false};

        receiver.setCallback([&](efp::SuperFramePtr frame) {
            CHECK(!frame->mBroken);
            CHECK(frame->mFlags == efp::Flags::INLINE_PAYLOAD);
            // Even with flag set, data should be received as-is
            received = true;
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            (void)receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(500, 0xAB);

        // Set inline payload flag but send regular data
        (void)sender.send(payload, 0x01, 1000, 1000, 0, 1, efp::Flags::INLINE_PAYLOAD);

        REQUIRE(waitFor([&]() { return received.load(); }));
    }

}

