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

TEST_SUITE("Fragment Ordering") {

    // =========================================================================
    // UnitTest7: Swap fragment order (1,3,2,4,5,6) and verify data integrity
    // =========================================================================
    TEST_CASE("Swap fragment order 1-3-2-4-5-6 (UnitTest7)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        std::atomic<bool> lDataReceived{false};
        size_t lPacketNumber = 0;
        std::vector<uint8_t> lSavedSubPacketNumber2;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mStreamId == 8);
            CHECK(apFrame->mPts == 1001);
            CHECK(apFrame->mPayloadCode == 2);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mSize == FRAME_SIZE);

            uint8_t lVectorChecker = 0;
            for (size_t lX = 0; lX < apFrame->mSize; lX++) {
                CHECK(apFrame->mpData[lX] == lVectorChecker++);
            }
            lDataReceived = true;
        }, 50, 20);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t aStreamId) {
            CHECK(aStreamId == 8);
            lPacketNumber++;

            std::vector<uint8_t> lPacket(aData.begin(), aData.end());

            if (lPacketNumber == 2) {
                lSavedSubPacketNumber2 = lPacket;
                return;
            } else if (lPacketNumber == 3) {
                auto lResult = lReceiver.receive(std::span<const uint8_t>(lPacket), 0);
                CHECK(lResult == efp::Result::OK);

                lResult = lReceiver.receive(std::span<const uint8_t>(lSavedSubPacketNumber2), 0);
                CHECK(lResult == efp::Result::OK);
                return;
            }

            auto lResult = lReceiver.receive(aData, 0);
            CHECK(lResult == efp::Result::OK);
        });

        std::vector<uint8_t> lMydata(FRAME_SIZE);
        std::generate(lMydata.begin(), lMydata.end(), [lN = 0]() mutable { return (uint8_t)(lN++); });

        auto lResult = lSender.send(lMydata, 0x02, 1001, 1, 2, 8);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest8: Receive Type2 first, then Type1 fragments
    // =========================================================================
    TEST_CASE("Receive Type2 first then Type1 (UnitTest8)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        std::atomic<bool> lDataReceived{false};
        std::vector<std::vector<uint8_t>> lDataKeptBack;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mStreamId == 8);
            CHECK(apFrame->mPts == 1001);
            CHECK(apFrame->mPayloadCode == 2);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mSize == FRAME_SIZE);

            uint8_t lVectorChecker = 0;
            for (size_t lX = 0; lX < apFrame->mSize; lX++) {
                CHECK(apFrame->mpData[lX] == lVectorChecker++);
            }
            lDataReceived = true;
        }, 50, 20);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            std::vector<uint8_t> lPacket(aData.begin(), aData.end());

            if ((aData[0] & 0x0f) == (uint8_t)(efp::FrameType::TYPE2)) {
                auto lResult = lReceiver.receive(aData, 0);
                CHECK(lResult == efp::Result::OK);

                std::swap(lDataKeptBack[0], lDataKeptBack[1]);
                for (auto& lX : lDataKeptBack) {
                    lResult = lReceiver.receive(std::span<const uint8_t>(lX), 0);
                    CHECK(lResult == efp::Result::OK);
                }
            } else {
                lDataKeptBack.push_back(lPacket);
            }
        });

        std::vector<uint8_t> lMydata(FRAME_SIZE);
        std::generate(lMydata.begin(), lMydata.end(), [lN = 0]() mutable { return (uint8_t)(lN++); });

        auto lResult = lSender.send(lMydata, 0x02, 1001, 1, 2, 8);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest10: Send two Type2 packets out of order, receive in order
    // =========================================================================
    TEST_CASE("Two Type2 frames out of order (UnitTest10)") {
        const size_t FRAME_SIZE = MTU - sizeof(efp::FrameType2);

        std::atomic<size_t> lDataReceived{0};
        size_t lPacketNumber = 0;
        std::vector<uint8_t> lFragmentKeptBack;
        size_t lReceivedFrameNumber = 0;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lReceivedFrameNumber++;

            if (lReceivedFrameNumber == 1) {
                CHECK(apFrame->mPts == 1001);
            }
            if (lReceivedFrameNumber == 2) {
                CHECK(apFrame->mPts == 1002);
            }

            CHECK(apFrame->mStreamId == 1);
            CHECK(apFrame->mPayloadCode == 0);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mSize == FRAME_SIZE);

            lDataReceived++;
        }, 50, 20);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            std::vector<uint8_t> lPacket(aData.begin(), aData.end());
            lPacketNumber++;

            if (lPacketNumber == 1) {
                lFragmentKeptBack = lPacket;
            } else if (lPacketNumber == 2) {
                auto lResult = lReceiver.receive(aData, 0);
                CHECK(lResult == efp::Result::OK);

                lResult = lReceiver.receive(std::span<const uint8_t>(lFragmentKeptBack), 0);
                CHECK(lResult == efp::Result::OK);
            }

            CHECK(lPacketNumber <= 2);
        });

        std::vector<uint8_t> lMydata(FRAME_SIZE);

        auto lResult = lSender.send(lMydata, 0x83, 1001, 1, 0, 1);
        CHECK(lResult == efp::Result::OK);

        lResult = lSender.send(lMydata, 0x83, 1002, 2, 0, 1);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load() == 2; }));
    }

    // =========================================================================
    // Extended: Extremely shuffled fragments
    // =========================================================================
    TEST_CASE("Extremely shuffled fragments (random order)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 10) + 50;

        std::atomic<bool> lDataReceived{false};
        std::vector<std::vector<uint8_t>> lAllFragments;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mSize == FRAME_SIZE);
            CHECK(!apFrame->mBroken);

            uint8_t lVectorChecker = 0;
            for (size_t lX = 0; lX < apFrame->mSize; lX++) {
                CHECK(apFrame->mpData[lX] == lVectorChecker++);
            }
            lDataReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lAllFragments.emplace_back(aData.begin(), aData.end());
        });

        std::vector<uint8_t> lMydata(FRAME_SIZE);
        std::generate(lMydata.begin(), lMydata.end(), [lN = 0]() mutable { return (uint8_t)(lN++); });

        auto lResult = lSender.send(lMydata, 0x01, 1000, 1000, 0, 1);
        CHECK(lResult == efp::Result::OK);

        std::mt19937 lRng(42);
        std::shuffle(lAllFragments.begin(), lAllFragments.end(), lRng);

        for (auto& lFrag : lAllFragments) {
            (void)lReceiver.receive(std::span<const uint8_t>(lFrag), 0);
        }

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest11: 5 packets in reversed order, drop middle
    // =========================================================================
    TEST_CASE("Five packets reversed order drop middle (UnitTest11)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        std::atomic<size_t> lDataReceived{0};
        size_t lSentSuperFrameNumber = 0;
        std::vector<std::vector<uint8_t>> lKeptBackFragments;
        std::vector<std::vector<std::vector<uint8_t>>> lKeptBackSuperFrames;
        size_t lReceivedFrameNumber = 0;
        int64_t lNextExpectedPts = 1001;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lReceivedFrameNumber++;
            CHECK(apFrame->mPts == lNextExpectedPts);
            lNextExpectedPts += (lNextExpectedPts == 1002 ? 2 : 1);

            CHECK(apFrame->mStreamId == 1);
            CHECK(apFrame->mPayloadCode == 0);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mSize == FRAME_SIZE);

            lDataReceived++;
        }, 50, 20);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            std::vector<uint8_t> lPacket(aData.begin(), aData.end());

            if ((aData[0] & 0x0f) == (uint8_t)(efp::FrameType::TYPE2)) {
                lSentSuperFrameNumber++;
                lKeptBackFragments.push_back(lPacket);
                lKeptBackSuperFrames.push_back(lKeptBackFragments);

                if (lSentSuperFrameNumber == 5) {
                    for (size_t lSuperFrame = lKeptBackSuperFrames.size(); lSuperFrame > 0; lSuperFrame--) {
                        if (lSuperFrame == 3) continue;

                        for (auto& lX : lKeptBackSuperFrames[lSuperFrame - 1]) {
                            auto lResult = lReceiver.receive(std::span<const uint8_t>(lX), 0);
                            CHECK(lResult == efp::Result::OK);
                        }
                    }
                }
                lKeptBackFragments.clear();
            } else {
                lKeptBackFragments.push_back(lPacket);
            }
        });

        std::vector<uint8_t> lMydata(FRAME_SIZE);

        for (size_t lPacketNumber = 0; lPacketNumber < 5; lPacketNumber++) {
            auto lResult = lSender.send(lMydata, 0x83, lPacketNumber + 1001, lPacketNumber + 1, 0, 1);
            REQUIRE(lResult == efp::Result::OK);
        }

        REQUIRE(waitFor([&]() { return lDataReceived.load() == 4; }));
    }

    // =========================================================================
    // UnitTest12: Reversed order + reversed fragment order within each
    // =========================================================================
    TEST_CASE("Reversed order with reversed fragments (UnitTest12)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        std::atomic<size_t> lDataReceived{0};
        size_t lSentSuperFrameNumber = 0;
        std::vector<std::vector<uint8_t>> lKeptBackFragments;
        std::vector<std::vector<std::vector<uint8_t>>> lKeptBackSuperFrames;
        size_t lReceivedFrameNumber = 0;
        int64_t lNextExpectedPts = 1001;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lReceivedFrameNumber++;
            CHECK(apFrame->mPts == lNextExpectedPts);
            lNextExpectedPts += (lNextExpectedPts == 1002 ? 2 : 1);

            CHECK(apFrame->mStreamId == 1);
            CHECK(apFrame->mPayloadCode == 0);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mSize == FRAME_SIZE);

            lDataReceived++;
        }, 100, 40);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            std::vector<uint8_t> lPacket(aData.begin(), aData.end());

            if ((aData[0] & 0x0f) == (uint8_t)(efp::FrameType::TYPE2)) {
                lSentSuperFrameNumber++;
                lKeptBackFragments.push_back(lPacket);
                lKeptBackSuperFrames.push_back(lKeptBackFragments);

                if (lSentSuperFrameNumber == 5) {
                    for (size_t lItem = lKeptBackSuperFrames.size(); lItem > 0; lItem--) {
                        if (lItem == 3) continue;
                        auto& lSuperFrame = lKeptBackSuperFrames[lItem - 1];
                        for (size_t lFragment = lSuperFrame.size(); lFragment > 0; lFragment--) {
                            auto lResult = lReceiver.receive(
                                std::span<const uint8_t>(lSuperFrame[lFragment - 1]), 0);
                            CHECK(lResult == efp::Result::OK);
                        }
                    }
                }
                lKeptBackFragments.clear();
                return;
            }
            lKeptBackFragments.push_back(lPacket);
        });

        std::vector<uint8_t> lMydata(FRAME_SIZE);

        for (size_t lPacketNumber = 0; lPacketNumber < 5; lPacketNumber++) {
            auto lResult = lSender.send(lMydata, 0x83, lPacketNumber + 1001, lPacketNumber + 1, 0, 1);
            REQUIRE(lResult == efp::Result::OK);
        }

        REQUIRE(waitFor([&]() { return lDataReceived.load() == 4; }));
    }

    // =========================================================================
    // UnitTest23: 10 packets, drop 4&5, deliver rest reversed with reversed fragments
    // =========================================================================
    TEST_CASE("Ten packets drop 4 and 5 reversed (UnitTest23)") {
        const size_t FRAME_SIZE = ((MTU - sizeof(efp::FrameType1)) * 5) + 12;

        std::atomic<size_t> lDataReceived{0};
        size_t lSentSuperFrameNumber = 0;
        std::vector<std::vector<uint8_t>> lKeptBackFragments;
        std::vector<std::vector<std::vector<uint8_t>>> lKeptBackSuperFrames;
        size_t lReceivedFrameNumber = 0;
        int64_t lNextExpectedPts = 1001;
        uint16_t lLastReceivedSuperFrame = 0;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lReceivedFrameNumber++;
            CHECK(apFrame->mPts == lNextExpectedPts);
            lNextExpectedPts += (lNextExpectedPts == 1003 ? 3 : 1);

            CHECK(apFrame->mStreamId == 1);
            CHECK(apFrame->mPayloadCode == 0);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mSize == FRAME_SIZE);

            if (apFrame->mPts == 1006) {
                CHECK(lLastReceivedSuperFrame + 3 == apFrame->mSuperFrameNo);
            } else if (apFrame->mPts != 1001) {
                CHECK(lLastReceivedSuperFrame + 1 == apFrame->mSuperFrameNo);
            }
            lLastReceivedSuperFrame = apFrame->mSuperFrameNo;

            lDataReceived++;
        }, 100, 40);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            std::vector<uint8_t> lPacket(aData.begin(), aData.end());

            if ((aData[0] & 0x0f) == (uint8_t)(efp::FrameType::TYPE2)) {
                lSentSuperFrameNumber++;
                lKeptBackFragments.push_back(lPacket);
                lKeptBackSuperFrames.push_back(lKeptBackFragments);

                if (lSentSuperFrameNumber == 10) {
                    for (size_t lItem = lKeptBackSuperFrames.size(); lItem > 0; lItem--) {
                        if (lItem == 4 || lItem == 5) continue;
                        auto& lSuperFrame = lKeptBackSuperFrames[lItem - 1];
                        for (size_t lFragment = lSuperFrame.size(); lFragment > 0; lFragment--) {
                            auto lResult = lReceiver.receive(
                                std::span<const uint8_t>(lSuperFrame[lFragment - 1]), 0);
                            CHECK(lResult == efp::Result::OK);
                        }
                    }
                }
                lKeptBackFragments.clear();
                return;
            }
            lKeptBackFragments.push_back(lPacket);
        });

        std::vector<uint8_t> lMydata(FRAME_SIZE);

        for (size_t lPacketNumber = 0; lPacketNumber < 10; lPacketNumber++) {
            auto lResult = lSender.send(lMydata, 0x83, lPacketNumber + 1001, lPacketNumber + 1, 0, 1);
            REQUIRE(lResult == efp::Result::OK);
        }

        REQUIRE(waitFor([&]() { return lDataReceived.load() == 8; }));
    }

}
