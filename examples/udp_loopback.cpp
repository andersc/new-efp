// Real UDP loopback example using net-tools v1.0.1.
// One UDP endpoint carries EFP data; the reverse endpoint carries NACKs.

#include "NetworkIF.h"
#include "efp.h"

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace {

bool check(NetworkError aResult, const char* apOperation) {
    if (aResult == NetworkError::SUCCESS) {
        return true;
    }
    std::cerr << apOperation << " failed: " << networkErrorToString(aResult) << '\n';
    return false;
}

} // namespace

int main() {
    constexpr uint16_t MTU = 1200;

    UdpServer lSenderEndpoint;
    UdpServer lReceiverEndpoint;
    if (!check(lSenderEndpoint.create(IpVersion::V4), "create sender endpoint") ||
        !check(lSenderEndpoint.bind(0, "127.0.0.1"), "bind sender endpoint") ||
        !check(lReceiverEndpoint.create(IpVersion::V4), "create receiver endpoint") ||
        !check(lReceiverEndpoint.bind(0, "127.0.0.1"), "bind receiver endpoint")) {
        return 1;
    }

    lSenderEndpoint.setOnNewConnection(
        [](const ClientInfo&) -> std::optional<std::any> {
            return std::optional<std::any>{std::any{true}};
        });
    lReceiverEndpoint.setOnNewConnection(
        [](const ClientInfo&) -> std::optional<std::any> {
            return std::optional<std::any>{std::any{true}};
        });

    std::atomic<bool> lDelivered = false;
    std::atomic<bool> lNetworkError = false;
    const std::vector<uint8_t> lPayload(16 * 1024, 0x5a);

    auto lReceiver = efp::makeReceiver(
        [&lDelivered, &lPayload](efp::SuperFramePtr apFrame) {
            const auto lData = std::span<const uint8_t>(apFrame->mpData, apFrame->mSize);
            lDelivered = !apFrame->mBroken &&
                         lData.size() == lPayload.size() &&
                         std::equal(lData.begin(), lData.end(), lPayload.begin());
        },
        [&lReceiverEndpoint, &lSenderEndpoint, &lNetworkError](std::span<const uint8_t> aNack) {
            int64_t lBytesSent = 0;
            const auto lResult = lReceiverEndpoint.sendTo(
                aNack, "127.0.0.1", lSenderEndpoint.getLocalPort(), lBytesSent);
            if (lResult != NetworkError::SUCCESS ||
                lBytesSent != static_cast<int64_t>(aNack.size())) {
                lNetworkError = true;
            }
        },
        500, 0, 3, 20);

    auto lSender = efp::makeSender(
        MTU,
        [&lSenderEndpoint, &lReceiverEndpoint, &lNetworkError](std::span<const uint8_t> aPacket, uint8_t) {
            int64_t lBytesSent = 0;
            const auto lResult = lSenderEndpoint.sendTo(
                aPacket, "127.0.0.1", lReceiverEndpoint.getLocalPort(), lBytesSent);
            if (lResult != NetworkError::SUCCESS ||
                lBytesSent != static_cast<int64_t>(aPacket.size())) {
                lNetworkError = true;
            }
        },
        efp::SubFragmentMode::SINGLE, 1000);

    lReceiverEndpoint.setOnDataReceived(
        [&lReceiver](const ClientInfo&, std::span<const uint8_t> aData, std::any&) {
            (void)lReceiver.receive(aData, 0);
        });
    lSenderEndpoint.setOnDataReceived(
        [&lSender](const ClientInfo&, std::span<const uint8_t> aData, std::any&) {
            (void)lSender.receiveNack(aData);
        });

    const auto lSendResult = lSender.send(lPayload, 0x01, 1000, 1000, 0, 1);
    if (lSendResult != efp::Result::OK) {
        std::cerr << "EFP send failed: " << static_cast<int>(lSendResult) << '\n';
        return 1;
    }

    const auto lDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!lDelivered && !lNetworkError && std::chrono::steady_clock::now() < lDeadline) {
        const auto lReceiverPoll = lReceiverEndpoint.poll(10);
        if (lReceiverPoll != NetworkError::SUCCESS &&
            lReceiverPoll != NetworkError::TIMEOUT_ERROR) {
            lNetworkError = true;
        }
        const auto lSenderPoll = lSenderEndpoint.poll(0);
        if (lSenderPoll != NetworkError::SUCCESS &&
            lSenderPoll != NetworkError::TIMEOUT_ERROR) {
            lNetworkError = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!lDelivered || lNetworkError) {
        std::cerr << "UDP loopback delivery failed\n";
        return 1;
    }

    std::cout << "Delivered " << lPayload.size()
              << " bytes as EFP fragments over UDP ports "
              << lSenderEndpoint.getLocalPort() << " -> "
              << lReceiverEndpoint.getLocalPort() << '\n';
    return 0;
}
