//
// EFP Unit Tests - Sender
//

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "efp.h"
#include <vector>
#include <algorithm>
#include <numeric>

constexpr uint16_t MTU = 1456;  // SRT max payload

TEST_SUITE("Sender") {

    TEST_CASE("Small packet results in single Type2 frame") {
        efp::Sender sender(MTU);

        std::vector<std::vector<uint8_t>> fragments;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        std::vector<uint8_t> payload(100);
        std::iota(payload.begin(), payload.end(), 0);

        auto result = sender.send(payload, 0x01, 1000, 1000, 0, 1);

        CHECK(result == efp::Result::Ok);
        REQUIRE(fragments.size() == 1);
        CHECK((fragments[0][0] & 0x0F) == static_cast<uint8_t>(efp::FrameType::Type2));
    }

    TEST_CASE("Packet exactly MTU - Type2 header size results in single Type2 frame") {
        efp::Sender sender(MTU);

        std::vector<std::vector<uint8_t>> fragments;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        size_t maxPayload = MTU - sizeof(efp::FrameType2);
        std::vector<uint8_t> payload(maxPayload);

        auto result = sender.send(payload, 0x01, 1000, 1000, 0, 1);

        CHECK(result == efp::Result::Ok);
        REQUIRE(fragments.size() == 1);
        CHECK((fragments[0][0] & 0x0F) == static_cast<uint8_t>(efp::FrameType::Type2));
        CHECK(fragments[0].size() == MTU);
    }

    TEST_CASE("Large packet results in Type1 + Type2 fragments") {
        efp::Sender sender(MTU);

        std::vector<std::vector<uint8_t>> fragments;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        // Payload that requires 2 fragments
        size_t type1Payload = MTU - sizeof(efp::FrameType1);
        std::vector<uint8_t> payload(type1Payload + 100);
        std::iota(payload.begin(), payload.end(), 0);

        auto result = sender.send(payload, 0x01, 1000, 1000, 0, 1);

        CHECK(result == efp::Result::Ok);
        REQUIRE(fragments.size() == 2);

        // First fragment should be Type1
        CHECK((fragments[0][0] & 0x0F) == static_cast<uint8_t>(efp::FrameType::Type1));
        // Last fragment should be Type2
        CHECK((fragments[1][0] & 0x0F) == static_cast<uint8_t>(efp::FrameType::Type2));
    }

    TEST_CASE("Multiple large packets get sequential superframe numbers") {
        efp::Sender sender(MTU);

        std::vector<uint16_t> superFrameNos;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            if ((data[0] & 0x0F) == static_cast<uint8_t>(efp::FrameType::Type2)) {
                auto* header = reinterpret_cast<const efp::FrameType2*>(data);
                superFrameNos.push_back(header->superFrameNo);
            }
        });

        std::vector<uint8_t> payload(100);

        for (int i = 0; i < 5; i++) {
            sender.send(payload, 0x01, 1000 + i, 1000 + i, 0, 1);
        }

        REQUIRE(superFrameNos.size() == 5);
        for (size_t i = 1; i < superFrameNos.size(); i++) {
            CHECK(superFrameNos[i] == superFrameNos[i-1] + 1);
        }
    }

    TEST_CASE("Flags are properly set in fragments") {
        efp::Sender sender(MTU);

        uint8_t capturedFlags = 0;
        sender.setCallback([&](const uint8_t* data, size_t, uint8_t) {
            capturedFlags = data[0] & 0xF0;
        });

        std::vector<uint8_t> payload(100);
        sender.send(payload, 0x01, 1000, 1000, 0, 1, efp::Flags::InlinePayload);

        CHECK(capturedFlags == efp::Flags::InlinePayload);
    }

    TEST_CASE("DTS-PTS difference is calculated correctly") {
        efp::Sender sender(MTU);

        uint32_t capturedDiff = 0;
        sender.setCallback([&](const uint8_t* data, size_t, uint8_t) {
            auto* header = reinterpret_cast<const efp::FrameType2*>(data);
            capturedDiff = header->dtsPtsDiff;
        });

        std::vector<uint8_t> payload(100);

        SUBCASE("PTS == DTS results in diff of 0") {
            sender.send(payload, 0x01, 1000, 1000, 0, 1);
            CHECK(capturedDiff == 0);
        }

        SUBCASE("PTS > DTS results in correct diff") {
            sender.send(payload, 0x01, 1500, 1000, 0, 1);
            CHECK(capturedDiff == 500);
        }

        SUBCASE("No DTS results in UINT32_MAX") {
            sender.send(payload, 0x01, 1000, UINT64_MAX, 0, 1);
            CHECK(capturedDiff == UINT32_MAX);
        }
    }

    TEST_CASE("Stream ID is passed through correctly") {
        efp::Sender sender(MTU);

        uint8_t capturedStreamId = 0;
        sender.setCallback([&](const uint8_t*, size_t, uint8_t streamId) {
            capturedStreamId = streamId;
        });

        std::vector<uint8_t> payload(100);
        sender.send(payload, 0x01, 1000, 1000, 0, 42);

        CHECK(capturedStreamId == 42);
    }

    TEST_CASE("Minimum MTU is enforced") {
        efp::Sender sender(100);  // Below minimum

        std::vector<std::vector<uint8_t>> fragments;
        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragments.emplace_back(data, data + size);
        });

        std::vector<uint8_t> payload(50);
        auto result = sender.send(payload, 0x01, 1000, 1000, 0, 1);

        CHECK(result == efp::Result::Ok);
        // Should still work with enforced minimum MTU
        CHECK(!fragments.empty());
    }

    TEST_CASE("Empty payload returns error") {
        efp::Sender sender(MTU);

        auto result = sender.send(nullptr, 0, 0x01, 1000, 1000, 0, 1);
        CHECK(result == efp::Result::InvalidParameter);

        std::vector<uint8_t> empty;
        result = sender.send(empty, 0x01, 1000, 1000, 0, 1);
        CHECK(result == efp::Result::InvalidParameter);
    }

}

