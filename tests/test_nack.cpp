//
// EFP Unit Tests - NACK (Negative Acknowledgment) and Retransmission
//
// Tests for Type0 NACK frames and retransmission mechanism
//

#include <doctest/doctest.h>

#include "efp.h"
#include <vector>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <chrono>
#include <thread>

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

TEST_SUITE("NACK and Retransmission") {

    // =========================================================================
    // Test NACK frame structure is correct
    // =========================================================================
    TEST_CASE("NACK frame structure sizes") {
        CHECK(sizeof(efp::FrameType0Nack) == 3);
        CHECK(sizeof(efp::NackEntry) == 6);
        CHECK(sizeof(efp::FrameType4) == 2);
    }

    // =========================================================================
    // Test Type0Subtype enum values
    // =========================================================================
    TEST_CASE("Type0Subtype enum values") {
        CHECK((uint8_t)(efp::Type0Subtype::RESERVED) == 0x00);
        CHECK((uint8_t)(efp::Type0Subtype::NACK) == 0x01);
    }

    // =========================================================================
    // Test sender processes valid NACK
    // =========================================================================
    TEST_CASE("Sender processes valid NACK and queues retransmit") {
        std::vector<std::span<const uint8_t>> lSentPackets;

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            // Store a copy of sent data
            lSentPackets.push_back(aData);
        }, efp::SubFragmentMode::SINGLE, 1000);  // 1 second retention

        // Send a frame that requires multiple fragments
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 4;
        std::vector<uint8_t> lData(FRAME_SIZE);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lSender.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);

        // Verify fragments were retained
        auto lStatsBefore = lSender.getStatistics();
        CHECK(lStatsBefore.mRetentionBufferFragments > 0);

        // Build a NACK for fragment 1
        std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack) + sizeof(efp::NackEntry));

        efp::FrameType0Nack lNackHeader;
        lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);
        lNackHeader.mSubtype = (uint8_t)(efp::Type0Subtype::NACK);
        lNackHeader.mNackCount = 1;

        efp::NackEntry lNackEntry;
        lNackEntry.mStreamId = 1;
        lNackEntry.mSuperFrameNo = 0;  // First superframe
        lNackEntry.mFragmentNo = 1;     // Request fragment 1
        lNackEntry.mFragmentCount = 0;  // Just this one fragment

        std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));
        std::memcpy(lNackData.data() + sizeof(lNackHeader), &lNackEntry, sizeof(lNackEntry));

        // Process the NACK
        lResult = lSender.receiveNack(std::span<const uint8_t>(lNackData));
        CHECK(lResult == efp::Result::OK);

        auto lStatsAfter = lSender.getStatistics();
        CHECK(lStatsAfter.mNacksReceived == 1);
        CHECK(lStatsAfter.mRetransmitQueueSize >= 1);
    }

    // =========================================================================
    // Test sender rejects invalid NACK (wrong type)
    // =========================================================================
    TEST_CASE("Sender rejects NACK with wrong frame type") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t>, uint8_t) {},
                                        efp::SubFragmentMode::SINGLE, 1000);

        // Build invalid NACK (wrong frame type)
        std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack));
        efp::FrameType0Nack lNackHeader;
        lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE1, 0);  // Wrong!
        lNackHeader.mSubtype = (uint8_t)(efp::Type0Subtype::NACK);
        lNackHeader.mNackCount = 0;

        std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));

        auto lResult = lSender.receiveNack(std::span<const uint8_t>(lNackData));
        CHECK(lResult == efp::Result::INVALID_PARAMETER);
    }

    // =========================================================================
    // Test sender rejects NACK with wrong subtype
    // =========================================================================
    TEST_CASE("Sender rejects NACK with wrong subtype") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t>, uint8_t) {},
                                        efp::SubFragmentMode::SINGLE, 1000);

        // Build invalid NACK (wrong subtype)
        std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack));
        efp::FrameType0Nack lNackHeader;
        lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);
        lNackHeader.mSubtype = 0xFF;  // Invalid subtype
        lNackHeader.mNackCount = 0;

        std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));

        auto lResult = lSender.receiveNack(std::span<const uint8_t>(lNackData));
        CHECK(lResult == efp::Result::INVALID_PARAMETER);
    }

    // =========================================================================
    // Test sender rejects NACK that's too small
    // =========================================================================
    TEST_CASE("Sender rejects NACK that's too small") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t>, uint8_t) {},
                                        efp::SubFragmentMode::SINGLE, 1000);

        // Too small for header
        std::vector<uint8_t> lNackData(1);
        lNackData[0] = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);

        auto lResult = lSender.receiveNack(std::span<const uint8_t>(lNackData));
        CHECK(lResult == efp::Result::FRAME_SIZE_MISMATCH);
    }

    // =========================================================================
    // Test NACK for non-existent fragment is ignored
    // =========================================================================
    TEST_CASE("NACK for non-existent fragment is ignored gracefully") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t>, uint8_t) {},
                                        efp::SubFragmentMode::SINGLE, 1000);

        // Don't send any data, just try to NACK a non-existent fragment
        std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack) + sizeof(efp::NackEntry));

        efp::FrameType0Nack lNackHeader;
        lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);
        lNackHeader.mSubtype = (uint8_t)(efp::Type0Subtype::NACK);
        lNackHeader.mNackCount = 1;

        efp::NackEntry lNackEntry;
        lNackEntry.mStreamId = 1;
        lNackEntry.mSuperFrameNo = 999;  // Non-existent
        lNackEntry.mFragmentNo = 0;
        lNackEntry.mFragmentCount = 0;

        std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));
        std::memcpy(lNackData.data() + sizeof(lNackHeader), &lNackEntry, sizeof(lNackEntry));

        auto lResult = lSender.receiveNack(std::span<const uint8_t>(lNackData));
        CHECK(lResult == efp::Result::OK);

        // Queue should be empty since fragment doesn't exist
        auto lStats = lSender.getStatistics();
        CHECK(lStats.mRetransmitQueueSize == 0);
    }

    // =========================================================================
    // Test batched NACK with multiple entries
    // =========================================================================
    TEST_CASE("Batched NACK with multiple entries") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t>, uint8_t) {},
                                        efp::SubFragmentMode::SINGLE, 1000);

        // Send a frame that requires multiple fragments
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 8;
        std::vector<uint8_t> lData(FRAME_SIZE);
        (void)lSender.send(lData, 0x01, 1000, 900, 42, 1);

        // Build NACK with multiple entries
        std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack) + 3 * sizeof(efp::NackEntry));

        efp::FrameType0Nack lNackHeader;
        lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);
        lNackHeader.mSubtype = (uint8_t)(efp::Type0Subtype::NACK);
        lNackHeader.mNackCount = 3;

        std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));

        // NACK fragments 1, 3, 5
        for (int lI = 0; lI < 3; lI++) {
            efp::NackEntry lNackEntry;
            lNackEntry.mStreamId = 1;
            lNackEntry.mSuperFrameNo = 0;
            lNackEntry.mFragmentNo = (uint16_t)(1 + lI * 2);
            lNackEntry.mFragmentCount = 0;
            std::memcpy(lNackData.data() + sizeof(lNackHeader) + lI * sizeof(efp::NackEntry),
                        &lNackEntry, sizeof(lNackEntry));
        }

        auto lResult = lSender.receiveNack(std::span<const uint8_t>(lNackData));
        CHECK(lResult == efp::Result::OK);

        auto lStats = lSender.getStatistics();
        CHECK(lStats.mNacksReceived == 1);
        // Should have 3 fragments queued for retransmit
        CHECK(lStats.mRetransmitQueueSize == 3);
    }

    // =========================================================================
    // Test NACK with consecutive fragment range
    // =========================================================================
    TEST_CASE("NACK with consecutive fragment range") {
        auto lSender = efp::makeSender(MTU, [](std::span<const uint8_t>, uint8_t) {},
                                        efp::SubFragmentMode::SINGLE, 1000);

        // Send a frame that requires multiple fragments
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 8;
        std::vector<uint8_t> lData(FRAME_SIZE);
        (void)lSender.send(lData, 0x01, 1000, 900, 42, 1);

        // Build NACK for consecutive range (fragments 2, 3, 4, 5)
        std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack) + sizeof(efp::NackEntry));

        efp::FrameType0Nack lNackHeader;
        lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);
        lNackHeader.mSubtype = (uint8_t)(efp::Type0Subtype::NACK);
        lNackHeader.mNackCount = 1;

        efp::NackEntry lNackEntry;
        lNackEntry.mStreamId = 1;
        lNackEntry.mSuperFrameNo = 0;
        lNackEntry.mFragmentNo = 2;
        lNackEntry.mFragmentCount = 3;  // 2, 3, 4, 5 = 4 fragments

        std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));
        std::memcpy(lNackData.data() + sizeof(lNackHeader), &lNackEntry, sizeof(lNackEntry));

        auto lResult = lSender.receiveNack(std::span<const uint8_t>(lNackData));
        CHECK(lResult == efp::Result::OK);

        auto lStats = lSender.getStatistics();
        CHECK(lStats.mRetransmitQueueSize == 4);
    }

    // =========================================================================
    // Test receiver statistics track complete/broken frames
    // =========================================================================
    TEST_CASE("Receiver statistics track complete and broken frames") {
        std::atomic<int> lFrameCount{0};

        auto lReceiver = efp::makeReceiver([&](efp::SuperFramePtr apFrame) {
            lFrameCount++;
        }, [](std::span<const uint8_t>) {}, 50);  // Short timeout

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            (void)lReceiver.receive(aData, 0);
        });

        // Send complete frame
        std::vector<uint8_t> lData(100);
        (void)lSender.send(lData, 0x01, 1000, 900, 42, 1);

        REQUIRE(waitFor([&]() { return lFrameCount.load() >= 1; }));

        auto lStats = lReceiver.getStatistics();
        CHECK(lStats.mCompleteFrames >= 1);
        CHECK(lStats.mFragmentsReceived >= 1);
    }

    // =========================================================================
    // Test receiver statistics track duplicates
    // =========================================================================
    TEST_CASE("Receiver statistics track duplicate fragments") {
        auto lReceiver = efp::makeReceiver([](efp::SuperFramePtr) {}, [](std::span<const uint8_t>) {}, 100);

        // Create a Type2 frame
        std::vector<uint8_t> lFrameData(sizeof(efp::FrameType2) + 100);
        efp::FrameType2 lHeader;
        lHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE2, 0);
        lHeader.mStreamId = 1;
        lHeader.mPayloadType = 0x01;
        lHeader.mSizeOfData = 100;
        lHeader.mSuperFrameNo = 0;
        lHeader.mOfFragmentNo = 0;
        lHeader.mType1PacketSize = 0;
        lHeader.mPts = 1000;
        lHeader.mDtsPtsDiff = 100;
        lHeader.mPayloadCode = 42;

        std::memcpy(lFrameData.data(), &lHeader, sizeof(lHeader));

        // Receive same frame twice
        auto lResult1 = lReceiver.receive(std::span<const uint8_t>(lFrameData), 0);
        CHECK(lResult1 == efp::Result::OK);

        auto lResult2 = lReceiver.receive(std::span<const uint8_t>(lFrameData), 0);
        CHECK(lResult2 == efp::Result::DUPLICATE_FRAGMENT);

        auto lStats = lReceiver.getStatistics();
        CHECK(lStats.mDuplicateFragments == 1);
    }

    // =========================================================================
    // Test receiver sends NACK for missing fragment after grace period
    // =========================================================================
    TEST_CASE("Receiver sends NACK for missing fragment") {
        std::atomic<int> lNackCount{0};
        std::vector<uint8_t> lLastNack;
        std::mutex lNackMutex;

        auto lReceiver = efp::makeReceiver(
            [](efp::SuperFramePtr) {},
            [&](std::span<const uint8_t> aData) {
                std::lock_guard<std::mutex> lLock(lNackMutex);
                lLastNack.assign(aData.begin(), aData.end());
                lNackCount++;
            },
            200,  // 200ms timeout
            0,    // No HOL timeout
            3,    // Max 3 NACK retries
            20    // 20ms NACK interval (manual override)
        );

        // Collect fragments but only send some
        std::vector<std::vector<uint8_t>> lFragments;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lFragments.emplace_back(aData.begin(), aData.end());
        });

        // Send a large frame that requires multiple fragments
        std::vector<uint8_t> lPayload(MTU * 3);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(lFragments.size() >= 3);

        // Only send first and last fragments (skip middle ones)
        (void)lReceiver.receive(std::span<const uint8_t>(lFragments[0]), 0);
        (void)lReceiver.receive(std::span<const uint8_t>(lFragments.back()), 0);

        // Wait for NACK to be sent
        REQUIRE(waitFor([&]() { return lNackCount.load() >= 1; }, std::chrono::milliseconds(100)));

        // Verify NACK was sent
        CHECK(lNackCount.load() >= 1);

        auto lStats = lReceiver.getStatistics();
        CHECK(lStats.mNacksSent >= 1);
    }

    // =========================================================================
    // Test receiver respects max NACK retry limit
    // =========================================================================
    TEST_CASE("Receiver respects max NACK retry limit") {
        std::atomic<int> lNackCount{0};

        auto lReceiver = efp::makeReceiver(
            [](efp::SuperFramePtr) {},
            [&](std::span<const uint8_t>) {
                lNackCount++;
            },
            150,  // 150ms timeout
            0,    // No HOL timeout
            2,    // Max 2 NACK retries
            10    // 10ms NACK interval
        );

        std::vector<std::vector<uint8_t>> lFragments;
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lFragments.emplace_back(aData.begin(), aData.end());
        });

        std::vector<uint8_t> lPayload(MTU * 3);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        REQUIRE(lFragments.size() >= 3);

        // Only send first fragment
        (void)lReceiver.receive(std::span<const uint8_t>(lFragments[0]), 0);

        // Wait for timeout (frame will be delivered as broken)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Should have sent at most 2 NACKs (max retries)
        CHECK(lNackCount.load() <= 2);
    }

    // =========================================================================
    // Test receiver throws on invalid timeout vs HOL config
    // =========================================================================
    TEST_CASE("Receiver throws on invalid timeout vs HOL config") {
        bool lThrew = false;
        try {
            auto lReceiver = efp::makeReceiver(
                [](efp::SuperFramePtr) {},
                [](std::span<const uint8_t>) {},
                100,  // 100ms timeout
                150   // 150ms HOL timeout > frame timeout = invalid!
            );
            (void)lReceiver;
        } catch (const std::invalid_argument&) {
            lThrew = true;
        }
        CHECK(lThrew);
    }

    // =========================================================================
    // Test receiver throws on invalid NACK budget vs timeout
    // =========================================================================
    TEST_CASE("Receiver throws on invalid NACK budget vs timeout") {
        bool lThrew = false;
        try {
            auto lReceiver = efp::makeReceiver(
                [](efp::SuperFramePtr) {},
                [](std::span<const uint8_t>) {},
                100,  // 100ms timeout
                0,    // No HOL timeout
                10,   // 10 retries
                20    // 20ms interval = 200ms budget > 100ms timeout = invalid!
            );
            (void)lReceiver;
        } catch (const std::invalid_argument&) {
            lThrew = true;
        }
        CHECK(lThrew);
    }

    // =========================================================================
    // Test HOL delivers old broken frame when newer complete blocked
    // =========================================================================
    TEST_CASE("HOL timeout delivers old broken frame blocking newer complete") {
        std::atomic<int> lCompleteCount{0};
        std::atomic<int> lBrokenCount{0};
        std::atomic<int> lTotalReceived{0};

        // Use RUN_TO_COMPLETION mode for deterministic behavior
        auto lReceiver = efp::makeReceiver(
            [&](efp::SuperFramePtr apFrame) {
                lTotalReceived++;
                if (apFrame->mBroken) {
                    lBrokenCount++;
                } else {
                    lCompleteCount++;
                }
            },
            [](std::span<const uint8_t>) {},
            2000, // 2000ms frame timeout
            100,  // 100ms HOL timeout
            0,    // Disable NACKs
            0,    // Auto NACK interval
            efp::ReceiverMode::RUN_TO_COMPLETION
        );

        // Use single sender to generate frames with sequential SuperFrameNos
        std::vector<std::vector<uint8_t>> lAllFragments;
        size_t lFrame0FragmentCount = 0;

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lAllFragments.emplace_back(aData.begin(), aData.end());
        });

        // Frame 0: large (multi-fragment)
        std::vector<uint8_t> lPayload0(MTU * 2);
        (void)lSender.send(lPayload0, 0x01, 1000, 1000, 0, 1);
        lFrame0FragmentCount = lAllFragments.size();
        REQUIRE(lFrame0FragmentCount >= 2);

        // Frame 1: small (single fragment)
        std::vector<uint8_t> lPayload1(100);
        (void)lSender.send(lPayload1, 0x01, 2000, 2000, 0, 1);
        REQUIRE(lAllFragments.size() == lFrame0FragmentCount + 1);

        // Send only FIRST fragment of frame 0 (incomplete)
        auto lResult0 = lReceiver.receive(std::span<const uint8_t>(lAllFragments[0]), 0);
        CHECK(lResult0 == efp::Result::OK);

        // Poll once - frame 0 should stay pending (incomplete)
        lReceiver.poll();
        CHECK(lTotalReceived.load() == 0);  // Nothing delivered yet

        // Send complete frame 1
        auto lResult1 = lReceiver.receive(std::span<const uint8_t>(lAllFragments.back()), 0);
        CHECK(lResult1 == efp::Result::OK);

        // Poll - frame 1 should be delivered immediately (complete)
        lReceiver.poll();
        CHECK(lCompleteCount.load() == 1);

        // Wait for HOL timeout
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // Poll again - frame 0 should be delivered as broken due to HOL timeout
        lReceiver.poll();

        CHECK(lCompleteCount.load() == 1);
        CHECK(lBrokenCount.load() == 1);
        CHECK(lTotalReceived.load() == 2);
    }

    // =========================================================================
    // Test full round-trip: NACK triggers retransmit and frame completes
    // =========================================================================
    TEST_CASE("Full round-trip: NACK triggers retransmit and frame completes") {
        std::atomic<bool> lCompleteReceived{false};
        std::vector<uint8_t> lReceivedNack;
        std::mutex lNackMutex;

        // Sender with retention for retransmit
        std::vector<std::vector<uint8_t>> lSentFragments;
        auto lSender = efp::makeSender(MTU,
            [&](std::span<const uint8_t> aData, uint8_t) {
                lSentFragments.emplace_back(aData.begin(), aData.end());
            },
            efp::SubFragmentMode::SINGLE,
            1000  // 1 second retention
        );

        auto lReceiver = efp::makeReceiver(
            [&](efp::SuperFramePtr apFrame) {
                if (!apFrame->mBroken) {
                    lCompleteReceived = true;
                }
            },
            [&](std::span<const uint8_t> aData) {
                std::lock_guard<std::mutex> lLock(lNackMutex);
                lReceivedNack.assign(aData.begin(), aData.end());

                // Process NACK and retransmit
                auto lResult = lSender.receiveNack(aData);
                CHECK(lResult == efp::Result::OK);
            },
            300,  // 300ms timeout
            0,    // No HOL
            3,    // 3 retries
            20    // 20ms interval
        );

        // Send a large frame
        std::vector<uint8_t> lPayload(MTU * 3);
        std::iota(lPayload.begin(), lPayload.end(), 0);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 42, 1);

        REQUIRE(lSentFragments.size() >= 3);

        // Simulate network: deliver all except middle fragment
        (void)lReceiver.receive(std::span<const uint8_t>(lSentFragments[0]), 0);
        // Skip fragment 1
        for (size_t lI = 2; lI < lSentFragments.size(); lI++) {
            (void)lReceiver.receive(std::span<const uint8_t>(lSentFragments[lI]), 0);
        }

        // Wait for NACK to be sent
        REQUIRE(waitFor([&]() {
            std::lock_guard<std::mutex> lLock(lNackMutex);
            return !lReceivedNack.empty();
        }, std::chrono::milliseconds(100)));

        // Now deliver the missing fragment (simulating retransmit)
        (void)lReceiver.receive(std::span<const uint8_t>(lSentFragments[1]), 0);

        // Frame should now be complete
        REQUIRE(waitFor([&]() { return lCompleteReceived.load(); }, std::chrono::milliseconds(200)));
        CHECK(lCompleteReceived.load());
    }

    // =========================================================================
    // Test processRetransmits invokes callback with byte-identical data
    // =========================================================================
    TEST_CASE("processRetransmits invokes callback with byte-identical data and updates statistics") {
        std::vector<std::vector<uint8_t>> lSentFragments;
        std::vector<std::vector<uint8_t>> lRetransmittedFragments;

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            // Track all sent fragments
            lSentFragments.emplace_back(aData.begin(), aData.end());
        }, efp::SubFragmentMode::SINGLE, 1000);  // 1 second retention

        // Send a frame that requires multiple fragments
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 4;
        std::vector<uint8_t> lData(FRAME_SIZE);
        std::iota(lData.begin(), lData.end(), 0);

        auto lResult = lSender.send(lData, 0x01, 1000, 900, 42, 1);
        CHECK(lResult == efp::Result::OK);
        REQUIRE(lSentFragments.size() >= 4);

        // Store original fragments for comparison
        auto lOriginalFragments = lSentFragments;

        // Build a NACK for fragment 1
        std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack) + sizeof(efp::NackEntry));

        efp::FrameType0Nack lNackHeader;
        lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);
        lNackHeader.mSubtype = (uint8_t)(efp::Type0Subtype::NACK);
        lNackHeader.mNackCount = 1;

        efp::NackEntry lNackEntry;
        lNackEntry.mStreamId = 1;
        lNackEntry.mSuperFrameNo = 0;
        lNackEntry.mFragmentNo = 1;
        lNackEntry.mFragmentCount = 0;

        std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));
        std::memcpy(lNackData.data() + sizeof(lNackHeader), &lNackEntry, sizeof(lNackEntry));

        // Process the NACK
        lResult = lSender.receiveNack(std::span<const uint8_t>(lNackData));
        CHECK(lResult == efp::Result::OK);

        auto lStatsBefore = lSender.getStatistics();
        CHECK(lStatsBefore.mRetransmitQueueSize == 1);
        CHECK(lStatsBefore.mRetransmittedFragments == 0);

        // Clear sent fragments to track only retransmissions
        lSentFragments.clear();

        // Process retransmits
        auto lRetransmitCount = lSender.processRetransmits();

        // Verify return value
        CHECK(lRetransmitCount == 1);

        // Verify callback was invoked
        REQUIRE(lSentFragments.size() == 1);

        // Verify retransmitted data is byte-identical to original fragment 1
        CHECK(lSentFragments[0] == lOriginalFragments[1]);

        // Verify statistics updated
        auto lStatsAfter = lSender.getStatistics();
        CHECK(lStatsAfter.mRetransmittedFragments == 1);
        CHECK(lStatsAfter.mRetransmitQueueSize == 0);
    }

    // =========================================================================
    // Test processRetransmits respects aMaxCount limit
    // =========================================================================
    TEST_CASE("processRetransmits respects aMaxCount limit") {
        std::vector<std::vector<uint8_t>> lSentFragments;

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lSentFragments.emplace_back(aData.begin(), aData.end());
        }, efp::SubFragmentMode::SINGLE, 1000);

        // Send a frame that requires multiple fragments
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 6;
        std::vector<uint8_t> lData(FRAME_SIZE);
        (void)lSender.send(lData, 0x01, 1000, 900, 42, 1);

        REQUIRE(lSentFragments.size() >= 5);

        // Build NACK for fragments 1, 2, 3
        std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack) + sizeof(efp::NackEntry));

        efp::FrameType0Nack lNackHeader;
        lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);
        lNackHeader.mSubtype = (uint8_t)(efp::Type0Subtype::NACK);
        lNackHeader.mNackCount = 1;

        efp::NackEntry lNackEntry;
        lNackEntry.mStreamId = 1;
        lNackEntry.mSuperFrameNo = 0;
        lNackEntry.mFragmentNo = 1;
        lNackEntry.mFragmentCount = 2;  // Fragments 1, 2, 3

        std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));
        std::memcpy(lNackData.data() + sizeof(lNackHeader), &lNackEntry, sizeof(lNackEntry));

        (void)lSender.receiveNack(std::span<const uint8_t>(lNackData));

        auto lStatsBefore = lSender.getStatistics();
        CHECK(lStatsBefore.mRetransmitQueueSize == 3);

        // Clear to track only retransmissions
        lSentFragments.clear();

        // Process only 1 retransmit
        auto lRetransmitCount = lSender.processRetransmits(1);

        CHECK(lRetransmitCount == 1);
        CHECK(lSentFragments.size() == 1);

        // Check remaining in queue
        auto lStatsAfter = lSender.getStatistics();
        CHECK(lStatsAfter.mRetransmitQueueSize == 2);
        CHECK(lStatsAfter.mRetransmittedFragments == 1);

        // Process remaining
        lSentFragments.clear();
        lRetransmitCount = lSender.processRetransmits();

        CHECK(lRetransmitCount == 2);
        CHECK(lSentFragments.size() == 2);

        auto lStatsFinal = lSender.getStatistics();
        CHECK(lStatsFinal.mRetransmitQueueSize == 0);
        CHECK(lStatsFinal.mRetransmittedFragments == 3);
    }

    // =========================================================================
    // Test processRetransmits on empty queue returns zero
    // =========================================================================
    TEST_CASE("processRetransmits on empty queue returns zero") {
        size_t lCallbackCount = 0;

        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t>, uint8_t) {
            lCallbackCount++;
        }, efp::SubFragmentMode::SINGLE, 1000);

        // Send a frame (callback will be invoked)
        std::vector<uint8_t> lData(100);
        (void)lSender.send(lData, 0x01, 1000, 900, 42, 1);

        auto lInitialCallbackCount = lCallbackCount;

        // Don't send any NACKs, just call processRetransmits
        auto lStatsBefore = lSender.getStatistics();
        CHECK(lStatsBefore.mRetransmitQueueSize == 0);

        auto lRetransmitCount = lSender.processRetransmits();

        // Should return 0
        CHECK(lRetransmitCount == 0);

        // Callback should not have been invoked again
        CHECK(lCallbackCount == lInitialCallbackCount);

        // Statistics should be unchanged
        auto lStatsAfter = lSender.getStatistics();
        CHECK(lStatsAfter.mRetransmittedFragments == 0);
        CHECK(lStatsAfter.mRetransmitQueueSize == 0);
    }

    // =========================================================================
    // Test processRetransmits skips evicted fragments gracefully
    // =========================================================================
    TEST_CASE("processRetransmits skips evicted fragments gracefully") {
        std::vector<std::vector<uint8_t>> lSentFragments;

        // Very short retention (1ms)
        auto lSender = efp::makeSender(MTU, [&](std::span<const uint8_t> aData, uint8_t) {
            lSentFragments.emplace_back(aData.begin(), aData.end());
        }, efp::SubFragmentMode::SINGLE, 1);  // 1ms retention

        // Send a frame
        const size_t FRAME_SIZE = (MTU - sizeof(efp::FrameType1)) * 3;
        std::vector<uint8_t> lData(FRAME_SIZE);
        (void)lSender.send(lData, 0x01, 1000, 900, 42, 1);

        REQUIRE(lSentFragments.size() >= 3);

        // Wait for retention to expire
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Send another frame to trigger eviction
        std::vector<uint8_t> lData2(100);
        (void)lSender.send(lData2, 0x01, 2000, 1900, 42, 1);

        // Build NACK for evicted fragment
        std::vector<uint8_t> lNackData(sizeof(efp::FrameType0Nack) + sizeof(efp::NackEntry));

        efp::FrameType0Nack lNackHeader;
        lNackHeader.mFrameType = efp::makeFrameTypeByte(efp::FrameType::TYPE0, 0);
        lNackHeader.mSubtype = (uint8_t)(efp::Type0Subtype::NACK);
        lNackHeader.mNackCount = 1;

        efp::NackEntry lNackEntry;
        lNackEntry.mStreamId = 1;
        lNackEntry.mSuperFrameNo = 0;  // First frame (should be evicted)
        lNackEntry.mFragmentNo = 1;
        lNackEntry.mFragmentCount = 0;

        std::memcpy(lNackData.data(), &lNackHeader, sizeof(lNackHeader));
        std::memcpy(lNackData.data() + sizeof(lNackHeader), &lNackEntry, sizeof(lNackEntry));

        // receiveNack should not add to queue since fragment is evicted
        auto lResult = lSender.receiveNack(std::span<const uint8_t>(lNackData));
        CHECK(lResult == efp::Result::OK);

        auto lStats = lSender.getStatistics();
        CHECK(lStats.mNacksReceived == 1);
        // Queue should be empty since fragment was not found in retention buffer
        CHECK(lStats.mRetransmitQueueSize == 0);

        // Clear to track only potential retransmissions
        lSentFragments.clear();

        // processRetransmits should handle empty queue gracefully
        auto lRetransmitCount = lSender.processRetransmits();
        CHECK(lRetransmitCount == 0);
        CHECK(lSentFragments.empty());
    }

}
