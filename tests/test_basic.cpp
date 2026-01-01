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

TEST_SUITE("Basic Functionality") {

    // =========================================================================
    // UnitTest1: Small packet results in Type2 frame only
    // =========================================================================
    TEST_CASE("Small packet Type2 only (UnitTest1)") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t> aData, uint8_t) {
            CHECK((aData[0] & 0x0f) == (uint8_t)(efp::FrameType::TYPE2));
        });

        std::vector<uint8_t> lMyData(MTU - sizeof(efp::FrameType2));

        auto lResult = lSender.send(lMyData, 0x02, 1001, 1, 2, 1);
        CHECK(lResult == efp::Result::OK);
    }

    // =========================================================================
    // UnitTest2: Type2 frame send/receive with metadata verification
    // =========================================================================
    TEST_CASE("Type2 frame roundtrip (UnitTest2)") {
        const size_t FRAME_SIZE = MTU - sizeof(efp::FrameType2);

        std::atomic<bool> lDataReceived{false};
        size_t lDataSent = 0;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mPts == 1001);
            CHECK(apFrame->mPayloadCode == 2);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mPayloadType == 0x02);
            CHECK(apFrame->mSize == FRAME_SIZE);
            lDataReceived = true;
        }, 50, 20);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            auto lResult = lReceiver.receive(aData, 0);
            CHECK(lResult == efp::Result::OK);
            lDataSent++;
            CHECK(lDataSent <= 1);  // Should only be one fragment
        });

        std::vector<uint8_t> lMyData(FRAME_SIZE);

        auto lResult = lSender.send(lMyData, 0x02, 1001, 1, 2, 1);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest3: Single byte payload (0xAA)
    // =========================================================================
    TEST_CASE("Single byte payload (UnitTest3)") {
        const size_t FRAME_SIZE = 1;

        std::atomic<bool> lDataReceived{false};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mPts == 1001);
            CHECK(apFrame->mPayloadCode == 2);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mPayloadType == 0x02);
            CHECK(apFrame->mSize == FRAME_SIZE);
            CHECK(apFrame->mpData[0] == 0xaa);
            lDataReceived = true;
        }, 50, 20);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            auto lResult = lReceiver.receive(aData, 0);
            CHECK(lResult == efp::Result::OK);
        });

        std::vector<uint8_t> lMyData(FRAME_SIZE);
        lMyData[0] = 0xaa;

        auto lResult = lSender.send(lMyData, 0x02, 1001, 1, 2, 1);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest4: Packet requiring Type1 + Type2
    // =========================================================================
    TEST_CASE("Type1 and Type2 combined (UnitTest4)") {
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) + 1;

        std::atomic<bool> lDataReceived{false};
        size_t lPacketNumber = 0;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mStreamId == 4);
            CHECK(apFrame->mPts == 1001);
            CHECK(apFrame->mPayloadCode == 2);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mSize == FRAME_SIZE);
            lDataReceived = true;
        }, 50, 20);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            if (lPacketNumber == 0) {
                // First should be Type1
                CHECK((aData[0] & 0x0f) == (uint8_t)(efp::FrameType::TYPE1));
                CHECK(aData.size() == MTU);
            } else if (lPacketNumber == 1) {
                // Second should be Type2
                CHECK((aData[0] & 0x0f) == (uint8_t)(efp::FrameType::TYPE2));
                CHECK(aData.size() == sizeof(efp::FrameType2) + 1);
            }
            CHECK(lPacketNumber < 2);
            lPacketNumber++;

            auto lResult = lReceiver.receive(aData, 0);
            CHECK(lResult == efp::Result::OK);
        });

        std::vector<uint8_t> lMyData(FRAME_SIZE);

        auto lResult = lSender.send(lMyData, 0x02, 1001, 1, 2, 4);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
    }

    // =========================================================================
    // UnitTest5: Linear vector over multiple fragments with data integrity
    // =========================================================================
    TEST_CASE("Linear vector multiple fragments (UnitTest5)") {
        const size_t FRAME_SIZE = (MTU * 5) + (MTU / 2);

        std::atomic<bool> lDataReceived{false};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mStreamId == 1);
            CHECK(apFrame->mPts == 1001);
            CHECK(apFrame->mPayloadCode == 2);
            CHECK(!apFrame->mBroken);
            CHECK(apFrame->mSize == FRAME_SIZE);

            // Verify linear data
            uint8_t lVectorChecker = 0;
            for (size_t lX = 0; lX < apFrame->mSize; lX++) {
                CHECK(apFrame->mpData[lX] == lVectorChecker++);
            }
            lDataReceived = true;
        }, 50, 20);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            auto lResult = lReceiver.receive(aData, 0);
            CHECK(lResult == efp::Result::OK);
        });

        std::vector<uint8_t> lMyData(FRAME_SIZE);
        std::generate(lMyData.begin(), lMyData.end(), [lN = 0]() mutable {
            return (uint8_t)(lN++);
        });

        auto lResult = lSender.send(lMyData, 0x02, 1001, 1, 2, 1);
        CHECK(lResult == efp::Result::OK);

        REQUIRE(waitFor([&]() { return lDataReceived.load(); }));
    }

    // =========================================================================
    // All payload types (0-255)
    // =========================================================================
    TEST_CASE("All 256 payload types") {
        std::atomic<int> lReceived{0};
        uint8_t lLastPayloadType = 0;
        std::mutex lMutex;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            std::lock_guard<std::mutex> lLock(lMutex);
            lLastPayloadType = apFrame->mPayloadType;
            lReceived++;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(50);

        // Test all 256 payload types
        for (int lI = 0; lI < 256; lI++) {
            (void)lSender.send(lPayload, (uint8_t)(lI), lI, lI, 0, 1);
        }

        REQUIRE(waitFor([&]() { return lReceived.load() == 256; }, std::chrono::milliseconds(5000)));
    }

    // =========================================================================
    // All stream IDs (1-255, 0 reserved)
    // =========================================================================
    TEST_CASE("All stream IDs 1-255") {
        std::atomic<int> lReceived{0};
        std::set<uint8_t> lReceivedStreams;
        std::mutex lMutex;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            std::lock_guard<std::mutex> lLock(lMutex);
            lReceivedStreams.insert(apFrame->mStreamId);
            lReceived++;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(50);

        // Test stream IDs 1-255 (0 is reserved)
        for (int lI = 1; lI <= 255; lI++) {
            (void)lSender.send(lPayload, 0x01, lI, lI, 0, (uint8_t)(lI));
        }

        REQUIRE(waitFor([&]() { return lReceived.load() == 255; }, std::chrono::milliseconds(5000)));

        std::lock_guard<std::mutex> lLock(lMutex);
        CHECK(lReceivedStreams.size() == 255);
    }

    // =========================================================================
    // FOURCC code generation and preservation
    // =========================================================================
    TEST_CASE("FOURCC code generation") {
        std::atomic<bool> lReceived{false};
        uint32_t lCapturedCode = 0;

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lCapturedCode = apFrame->mPayloadCode;
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);

        // Test various FOURCC codes
        auto lAnxbCode = EFP_CODE('A', 'N', 'X', 'B');
        CHECK(lAnxbCode == 0x414E5842);  // 'A'<<24 | 'N'<<16 | 'X'<<8 | 'B'

        (void)lSender.send(lPayload, 0x83, 1000, 1000, lAnxbCode, 1);

        REQUIRE(waitFor([&]() { return lReceived.load(); }));
        CHECK(lCapturedCode == lAnxbCode);
    }

    // =========================================================================
    // Media types from efp_media_types.h
    // =========================================================================
    TEST_CASE("Media types usage") {
        std::atomic<bool> lReceived{false};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mPayloadType == efp::media::PayloadType::H264);
            CHECK(apFrame->mPayloadCode == efp::media::PayloadCode::ANXB);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(1000);

        (void)lSender.send(lPayload,
                   efp::media::PayloadType::H264,
                   90000, 90000,
                   efp::media::PayloadCode::ANXB,
                   1);

        REQUIRE(waitFor([&]() { return lReceived.load(); }));
    }

    // =========================================================================
    // PTS/DTS handling
    // =========================================================================
    TEST_CASE("PTS/DTS handling") {
        SUBCASE("PTS == DTS") {
            std::atomic<bool> lReceived{false};

            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
                CHECK(apFrame->mPts == 1000);
                CHECK(apFrame->mDts == 1000);
                lReceived = true;
            }, 100, 0);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                (void)lReceiver.receive(aData, 0);
            });

            std::vector<uint8_t> lPayload(100);
            (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return lReceived.load(); }));
        }

        SUBCASE("PTS > DTS") {
            std::atomic<bool> lReceived{false};

            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
                CHECK(apFrame->mPts == 2000);
                CHECK(apFrame->mDts == 1000);
                lReceived = true;
            }, 100, 0);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                (void)lReceiver.receive(aData, 0);
            });

            std::vector<uint8_t> lPayload(100);
            (void)lSender.send(lPayload, 0x01, 2000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return lReceived.load(); }));
        }

        SUBCASE("No DTS (UINT64_MAX)") {
            std::atomic<bool> lReceived{false};

            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
                CHECK(apFrame->mPts == 1000);
                CHECK(apFrame->mDts == UINT64_MAX);
                lReceived = true;
            }, 100, 0);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                (void)lReceiver.receive(aData, 0);
            });

            std::vector<uint8_t> lPayload(100);
            (void)lSender.send(lPayload, 0x01, 1000, UINT64_MAX, 0, 1);

            REQUIRE(waitFor([&]() { return lReceived.load(); }));
        }
    }

    // =========================================================================
    // Empty payload error
    // =========================================================================
    TEST_CASE("Empty payload returns error") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t>, uint8_t) {});

        std::vector<uint8_t> lEmpty;
        auto lResult = lSender.send(lEmpty, 0x01, 1000, 1000, 0, 1);
        CHECK(lResult == efp::Result::INVALID_PARAMETER);
    }

    // =========================================================================
    // Flags preservation
    // =========================================================================
    TEST_CASE("Flags preservation") {
        std::atomic<bool> lReceived{false};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mFlags == efp::Flags::INLINE_PAYLOAD);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1, efp::Flags::INLINE_PAYLOAD);

        REQUIRE(waitFor([&]() { return lReceived.load(); }));
    }

    // =========================================================================
    // Source ID passthrough
    // =========================================================================
    TEST_CASE("Source ID passthrough") {
        std::atomic<bool> lReceived{false};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            CHECK(apFrame->mSourceId == 42);
            lReceived = true;
        }, 100, 0);

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 42);  // Pass source ID
        });

        std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(waitFor([&]() { return lReceived.load(); }));
    }

    // =========================================================================
    // Exact boundary payloads
    // =========================================================================
    TEST_CASE("Exact MTU boundary payloads") {
        SUBCASE("Exactly Type2 max payload") {
            std::atomic<bool> lReceived{false};
            size_t lCapturedSize = 0;

            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
                lCapturedSize = apFrame->mSize;
                CHECK(!apFrame->mBroken);
                lReceived = true;
            }, 100, 0);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                (void)lReceiver.receive(aData, 0);
            });

            auto lMaxType2Payload = MTU - sizeof(efp::FrameType2);
            std::vector<uint8_t> lPayload(lMaxType2Payload);
            (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return lReceived.load(); }));
            CHECK(lCapturedSize == lMaxType2Payload);
        }

        SUBCASE("One byte over Type2 max") {
            std::atomic<bool> lReceived{false};
            int lFragmentCount = 0;

            auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
                CHECK(!apFrame->mBroken);
                lReceived = true;
            }, 100, 0);

            auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
                lFragmentCount++;
                (void)lReceiver.receive(aData, 0);
            });

            auto lPayloadSize = MTU - sizeof(efp::FrameType2) + 1;
            std::vector<uint8_t> lPayload(lPayloadSize);
            (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

            REQUIRE(waitFor([&]() { return lReceived.load(); }));
            CHECK(lFragmentCount == 2);  // Should be Type1 + Type2
        }
    }

}

