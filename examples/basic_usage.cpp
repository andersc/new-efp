//
// EFP Basic Usage Example
//
// This example demonstrates sending and receiving data using EFP.
//

#include "efp.h"
#include "efp_media_types.h"

#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <exception>

int main() {
    try {
    std::cout << "EFP Version: " << static_cast<unsigned int>(efp::VERSION_MAJOR) << "." << static_cast<unsigned int>(efp::VERSION_MINOR) << "\n\n";

    // -------------------------------------------------------------------------
    // Example 1: Simple loopback - send and receive small data
    // -------------------------------------------------------------------------
    std::cout << "=== Example 1: Simple Loopback ===\n";
    {
        constexpr uint16_t MTU = 1400;

        efp::Sender lSender(MTU);
        efp::Receiver lReceiver(100, 0);  // 100ms timeout, no HOL blocking

        // Set up receiver callback
        lReceiver.setCallback([](efp::SuperFramePtr apFrame) {
            std::cout << "Received frame: "
                      << "size=" << apFrame->mSize
                      << ", pts=" << apFrame->mPts
                      << ", broken=" << apFrame->mBroken
                      << "\n";
        });

        // Connect sender output to receiver input
        lSender.setCallback([&lReceiver](const uint8_t* apData, size_t aSize, uint8_t aStreamId) {
            (void)lReceiver.receive(apData, aSize, 0);
        });

        // Send some data
        const std::vector<uint8_t> lPayload = {0x01, 0x02, 0x03, 0x04, 0x05};
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        // Give receiver thread time to process
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // -------------------------------------------------------------------------
    // Example 2: Large fragmented data
    // -------------------------------------------------------------------------
    std::cout << "\n=== Example 2: Large Fragmented Data ===\n";
    {
        constexpr uint16_t MTU = 1400;

        efp::Sender lSender(MTU);
        efp::Receiver lReceiver(100, 0);

        int lFragmentCount = 0;

        lReceiver.setCallback([](efp::SuperFramePtr apFrame) {
            std::cout << "Received large frame: "
                      << "size=" << apFrame->mSize
                      << ", broken=" << apFrame->mBroken
                      << "\n";
        });

        lSender.setCallback([&](const uint8_t* apData, size_t aSize, uint8_t) {
            lFragmentCount++;
            (void)lReceiver.receive(apData, aSize, 0);
        });

        // Send 10KB of data (will be fragmented)
        const std::vector<uint8_t> lLargePayload(10000, 0xAB);
        (void)lSender.send(lLargePayload, 0x01, 2000, 2000, 0, 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::cout << "Data was split into " << lFragmentCount << " fragments\n";
    }

    // -------------------------------------------------------------------------
    // Example 3: Using media types for H.264 video
    // -------------------------------------------------------------------------
    std::cout << "\n=== Example 3: H.264 Video with Media Types ===\n";
    {
        constexpr uint16_t MTU = 1400;

        efp::Sender lSender(MTU);
        efp::Receiver lReceiver(100, 0);

        lReceiver.setCallback([](efp::SuperFramePtr apFrame) {
            const char* lpFormat = (apFrame->mPayloadCode == efp::media::PayloadCode::ANXB)
                                 ? "Annex B" : "AVCC";
            std::cout << "Received H.264 NAL unit: "
                      << "size=" << apFrame->mSize
                      << ", format=" << lpFormat
                      << ", pts=" << apFrame->mPts
                      << "\n";
        });

        lSender.setCallback([&lReceiver](const uint8_t* apData, size_t aSize, uint8_t) {
            (void)lReceiver.receive(apData, aSize, 0);
        });

        // Simulated H.264 NAL unit (Annex B format with start code)
        std::vector<uint8_t> lNalUnit = {0x00, 0x00, 0x00, 0x01, 0x67, /* SPS data... */};
        lNalUnit.resize(500);  // Simulate larger NAL

        (void)lSender.send(lNalUnit,
                    efp::media::PayloadType::H264,
                    90000,  // PTS at 90kHz (1 second)
                    90000,  // DTS
                    efp::media::PayloadCode::ANXB,
                    1);  // Stream ID 1

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // -------------------------------------------------------------------------
    // Example 4: Multiple streams
    // -------------------------------------------------------------------------
    std::cout << "\n=== Example 4: Multiple Streams ===\n";
    {
        constexpr uint16_t MTU = 1400;

        efp::Sender lSender(MTU);
        efp::Receiver lReceiver(100, 0);

        lReceiver.setCallback([](efp::SuperFramePtr apFrame) {
            std::cout << "Stream " << (int)apFrame->mStreamId << ": "
                      << "size=" << apFrame->mSize
                      << ", type=0x" << std::hex << (int)apFrame->mPayloadType << std::dec
                      << "\n";
        });

        lSender.setCallback([&lReceiver](const uint8_t* apData, size_t aSize, uint8_t) {
            (void)lReceiver.receive(apData, aSize, 0);
        });

        // Video on stream 1
        const std::vector<uint8_t> lVideo(1000);
        (void)lSender.send(lVideo, efp::media::PayloadType::H264, 90000, 90000,
                    efp::media::PayloadCode::ANXB, 1);

        // Audio on stream 2
        const std::vector<uint8_t> lAudio(200);
        (void)lSender.send(lAudio, efp::media::PayloadType::AAC, 48000, 48000,
                    efp::media::PayloadCode::ADTS, 2);

        // Metadata on stream 3
        const std::vector<uint8_t> lMetadata(50);
        (void)lSender.send(lMetadata, efp::media::PayloadType::JSON, 0, 0, 0, 3);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // -------------------------------------------------------------------------
    // Example 5: Run-to-completion mode (no internal threads)
    // -------------------------------------------------------------------------
    std::cout << "\n=== Example 5: Run-to-Completion Mode ===\n";
    {
        constexpr uint16_t MTU = 1400;

        efp::Sender lSender(MTU);
        efp::Receiver lReceiver(100, 0, efp::ReceiverMode::RUN_TO_COMPLETION);

        bool lReceived = false;

        lReceiver.setCallback([&lReceived](efp::SuperFramePtr apFrame) {
            std::cout << "Run-to-completion received: size=" << apFrame->mSize << "\n";
            lReceived = true;
        });

        lSender.setCallback([&lReceiver](const uint8_t* apData, size_t aSize, uint8_t) {
            (void)lReceiver.receive(apData, aSize, 0);
        });

        const std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        // In run-to-completion mode, must call poll() to process
        lReceiver.poll();

        std::cout << "Frame received: " << (lReceived ? "yes" : "no") << "\n";
    }

    // -------------------------------------------------------------------------
    // Example 6: Custom buffer size
    // -------------------------------------------------------------------------
    std::cout << "\n=== Example 6: Custom Buffer Size ===\n";
    {
        // Use smaller buffer (2^10 - 1 = 1023)
        efp::Sender<1023> lSender(1400);
        efp::Receiver<1023> lReceiver(100, 0);

        lReceiver.setCallback([](efp::SuperFramePtr apFrame) {
            std::cout << "Custom buffer receiver got frame: size=" << apFrame->mSize << "\n";
        });

        lSender.setCallback([&lReceiver](const uint8_t* apData, size_t aSize, uint8_t) {
            (void)lReceiver.receive(apData, aSize, 0);
        });

        const std::vector<uint8_t> lPayload(100);
        (void)lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\n=== All examples completed ===\n";
    return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }
}

