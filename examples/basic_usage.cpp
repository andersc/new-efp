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

int main() {
    std::cout << "EFP Version: " << efp::VERSION_MAJOR << "." << efp::VERSION_MINOR << "\n\n";

    // -------------------------------------------------------------------------
    // Example 1: Simple loopback - send and receive small data
    // -------------------------------------------------------------------------
    std::cout << "=== Example 1: Simple Loopback ===\n";
    {
        constexpr uint16_t MTU = 1400;

        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);  // 100ms timeout, no HOL blocking

        // Set up receiver callback
        receiver.setCallback([](efp::SuperFramePtr frame) {
            std::cout << "Received frame: "
                      << "size=" << frame->size
                      << ", pts=" << frame->pts
                      << ", broken=" << frame->broken
                      << "\n";
        });

        // Connect sender output to receiver input
        sender.setCallback([&receiver](const uint8_t* data, size_t size, uint8_t streamId) {
            receiver.receive(data, size, 0);
        });

        // Send some data
        std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04, 0x05};
        sender.send(payload, 0x01, 1000, 1000, 0, 1);

        // Give receiver thread time to process
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // -------------------------------------------------------------------------
    // Example 2: Large fragmented data
    // -------------------------------------------------------------------------
    std::cout << "\n=== Example 2: Large Fragmented Data ===\n";
    {
        constexpr uint16_t MTU = 1400;

        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        int fragmentCount = 0;

        receiver.setCallback([](efp::SuperFramePtr frame) {
            std::cout << "Received large frame: "
                      << "size=" << frame->size
                      << ", broken=" << frame->broken
                      << "\n";
        });

        sender.setCallback([&](const uint8_t* data, size_t size, uint8_t) {
            fragmentCount++;
            receiver.receive(data, size, 0);
        });

        // Send 10KB of data (will be fragmented)
        std::vector<uint8_t> largePayload(10000, 0xAB);
        sender.send(largePayload, 0x01, 2000, 2000, 0, 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::cout << "Data was split into " << fragmentCount << " fragments\n";
    }

    // -------------------------------------------------------------------------
    // Example 3: Using media types for H.264 video
    // -------------------------------------------------------------------------
    std::cout << "\n=== Example 3: H.264 Video with Media Types ===\n";
    {
        constexpr uint16_t MTU = 1400;

        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        receiver.setCallback([](efp::SuperFramePtr frame) {
            const char* format = (frame->payloadCode == efp::media::PayloadCode::ANXB)
                                 ? "Annex B" : "AVCC";
            std::cout << "Received H.264 NAL unit: "
                      << "size=" << frame->size
                      << ", format=" << format
                      << ", pts=" << frame->pts
                      << "\n";
        });

        sender.setCallback([&receiver](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        // Simulated H.264 NAL unit (Annex B format with start code)
        std::vector<uint8_t> nalUnit = {0x00, 0x00, 0x00, 0x01, 0x67, /* SPS data... */};
        nalUnit.resize(500);  // Simulate larger NAL

        sender.send(nalUnit,
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

        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0);

        receiver.setCallback([](efp::SuperFramePtr frame) {
            std::cout << "Stream " << (int)frame->streamId << ": "
                      << "size=" << frame->size
                      << ", type=0x" << std::hex << (int)frame->payloadType << std::dec
                      << "\n";
        });

        sender.setCallback([&receiver](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        // Video on stream 1
        std::vector<uint8_t> video(1000);
        sender.send(video, efp::media::PayloadType::H264, 90000, 90000,
                    efp::media::PayloadCode::ANXB, 1);

        // Audio on stream 2
        std::vector<uint8_t> audio(200);
        sender.send(audio, efp::media::PayloadType::AAC, 48000, 48000,
                    efp::media::PayloadCode::ADTS, 2);

        // Metadata on stream 3
        std::vector<uint8_t> metadata(50);
        sender.send(metadata, efp::media::PayloadType::JSON, 0, 0, 0, 3);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // -------------------------------------------------------------------------
    // Example 5: Run-to-completion mode (no internal threads)
    // -------------------------------------------------------------------------
    std::cout << "\n=== Example 5: Run-to-Completion Mode ===\n";
    {
        constexpr uint16_t MTU = 1400;

        efp::Sender sender(MTU);
        efp::Receiver receiver(100, 0, efp::ReceiverMode::RunToCompletion);

        bool received = false;

        receiver.setCallback([&received](efp::SuperFramePtr frame) {
            std::cout << "Run-to-completion received: size=" << frame->size << "\n";
            received = true;
        });

        sender.setCallback([&receiver](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);
        sender.send(payload, 0x01, 1000, 1000, 0, 1);

        // In run-to-completion mode, must call poll() to process
        receiver.poll();

        std::cout << "Frame received: " << (received ? "yes" : "no") << "\n";
    }

    // -------------------------------------------------------------------------
    // Example 6: Custom buffer size
    // -------------------------------------------------------------------------
    std::cout << "\n=== Example 6: Custom Buffer Size ===\n";
    {
        // Use smaller buffer (2^10 - 1 = 1023)
        efp::Sender<1023> sender(1400);
        efp::Receiver<1023> receiver(100, 0);

        receiver.setCallback([](efp::SuperFramePtr frame) {
            std::cout << "Custom buffer receiver got frame: size=" << frame->size << "\n";
        });

        sender.setCallback([&receiver](const uint8_t* data, size_t size, uint8_t) {
            receiver.receive(data, size, 0);
        });

        std::vector<uint8_t> payload(100);
        sender.send(payload, 0x01, 1000, 1000, 0, 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\n=== All examples completed ===\n";
    return 0;
}

