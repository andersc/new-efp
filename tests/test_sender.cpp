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
        std::vector<std::vector<uint8_t>> lFragments;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lFragments.emplace_back(aData.begin(), aData.end());
        });

        std::vector<uint8_t> lPayload(100);
        std::iota(lPayload.begin(), lPayload.end(), 0);

        auto lResult = lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        CHECK(lResult == efp::Result::OK);
        REQUIRE(lFragments.size() == 1);
        CHECK((lFragments[0][0] & 0x0F) == (uint8_t)(efp::FrameType::TYPE2));
    }

    TEST_CASE("Packet exactly MTU - Type2 header size results in single Type2 frame") {
        std::vector<std::vector<uint8_t>> lFragments;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lFragments.emplace_back(aData.begin(), aData.end());
        });

        size_t lMaxPayload = MTU - sizeof(efp::FrameType2);
        std::vector<uint8_t> lPayload(lMaxPayload);

        auto lResult = lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        CHECK(lResult == efp::Result::OK);
        REQUIRE(lFragments.size() == 1);
        CHECK((lFragments[0][0] & 0x0F) == (uint8_t)(efp::FrameType::TYPE2));
        CHECK(lFragments[0].size() == MTU);
    }

    TEST_CASE("Large packet results in Type1 + Type2 fragments") {
        std::vector<std::vector<uint8_t>> lFragments;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lFragments.emplace_back(aData.begin(), aData.end());
        });

        // Payload that requires 2 fragments
        size_t lType1Payload = MTU - sizeof(efp::FrameType1);
        std::vector<uint8_t> lPayload(lType1Payload + 100);
        std::iota(lPayload.begin(), lPayload.end(), 0);

        auto lResult = lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        CHECK(lResult == efp::Result::OK);
        REQUIRE(lFragments.size() == 2);

        // First fragment should be Type1
        CHECK((lFragments[0][0] & 0x0F) == (uint8_t)(efp::FrameType::TYPE1));
        // Last fragment should be Type2
        CHECK((lFragments[1][0] & 0x0F) == (uint8_t)(efp::FrameType::TYPE2));
    }

    TEST_CASE("Multiple large packets get sequential superframe numbers") {
        std::vector<uint16_t> lSuperFrameNos;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            if ((aData[0] & 0x0F) == (uint8_t)(efp::FrameType::TYPE2)) {
                auto* lpHeader = (const efp::FrameType2*)(aData.data());
                lSuperFrameNos.push_back(lpHeader->mSuperFrameNo);
            }
        });

        std::vector<uint8_t> lPayload(100);

        for (int lI = 0; lI < 5; lI++) {
            (void)lSender.send(lPayload, 0x01, 1000 + lI, 1000 + lI, 0, 1);
        }

        REQUIRE(lSuperFrameNos.size() == 5);
        for (size_t lI = 1; lI < lSuperFrameNos.size(); lI++) {
            CHECK(lSuperFrameNos[lI] == lSuperFrameNos[lI-1] + 1);
        }
    }

    TEST_CASE("Flags are properly set in fragments") {
        uint8_t lCapturedFlags = 0;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lCapturedFlags = aData[0] & 0xF0;
        });

        std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1, efp::Flags::INLINE_PAYLOAD);

        CHECK(lCapturedFlags == efp::Flags::INLINE_PAYLOAD);
    }

    TEST_CASE("DTS-PTS difference is calculated correctly") {
        uint32_t lCapturedDiff = 0;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            auto* lpHeader = (const efp::FrameType2*)(aData.data());
            lCapturedDiff = lpHeader->mDtsPtsDiff;
        });

        std::vector<uint8_t> lPayload(100);

        SUBCASE("PTS == DTS results in diff of 0") {
            (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);
            CHECK(lCapturedDiff == 0);
        }

        SUBCASE("PTS > DTS results in correct diff") {
            (void)lSender.send(lPayload, 0x01, 1500, 1000, 0, 1);
            CHECK(lCapturedDiff == 500);
        }

        SUBCASE("No DTS results in UINT32_MAX") {
            (void)lSender.send(lPayload, 0x01, 1000, UINT64_MAX, 0, 1);
            CHECK(lCapturedDiff == UINT32_MAX);
        }
    }

    TEST_CASE("Stream ID is passed through correctly") {
        uint8_t lCapturedStreamId = 0;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t>, uint8_t aStreamId) {
            lCapturedStreamId = aStreamId;
        });

        std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 42);

        CHECK(lCapturedStreamId == 42);
    }

    TEST_CASE("Minimum MTU is enforced") {
        std::vector<std::vector<uint8_t>> lFragments;
        auto lSender = efp::makeSender(100, [&](std::span<const uint8_t> aData, uint8_t) {
            lFragments.emplace_back(aData.begin(), aData.end());
        });

        std::vector<uint8_t> lPayload(50);
        auto lResult = lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        CHECK(lResult == efp::Result::OK);
        // Should still work with enforced minimum MTU
        CHECK(!lFragments.empty());
    }

    TEST_CASE("Empty payload returns error") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t>, uint8_t) {});

        std::vector<uint8_t> lEmpty;
        auto lResult = lSender.send(lEmpty, 0x01, 1000, 1000, 0, 1);
        CHECK(lResult == efp::Result::INVALID_PARAMETER);
    }

}

