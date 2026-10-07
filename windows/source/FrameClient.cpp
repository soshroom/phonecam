#include "FrameClient.h"

#include <WS2tcpip.h>
#include <chrono>

namespace {
bool recvAll(SOCKET socket, char* data, int bytes) {
    int received = 0;
    while (received < bytes) {
        const int n = recv(socket, data + received, bytes - received, 0);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}
}

FrameClient::FrameClient() {
    WSADATA data{};
    winsockStarted_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

FrameClient::~FrameClient() {
    Stop();
    if (winsockStarted_) WSACleanup();
}

void FrameClient::Start(std::uint16_t port) {
    if (!winsockStarted_ || running_.exchange(true)) return;
    port_ = port;
    thread_ = std::thread(&FrameClient::Run, this);
}

void FrameClient::Stop() {
    running_.store(false);
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    if (thread_.joinable()) thread_.join();
}

bool FrameClient::Latest(std::vector<std::uint8_t>& frame) const {
    std::scoped_lock lock(mutex_);
    if (latest_.empty()) return false;
    frame = latest_;
    return true;
}

void FrameClient::Run() {
    while (running_.load()) {
        SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port_);
        InetPtonW(AF_INET, L"127.0.0.1", &address.sin_addr);
        if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            closesocket(socket);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        socket_ = socket;

        while (running_.load()) {
            std::uint32_t networkLength = 0;
            if (!recvAll(socket, reinterpret_cast<char*>(&networkLength), sizeof(networkLength))) break;
            const std::uint32_t length = ntohl(networkLength);
            if (length == 0 || length > 32 * 1024 * 1024) break;

            std::vector<std::uint8_t> frame(length);
            if (!recvAll(socket, reinterpret_cast<char*>(frame.data()), static_cast<int>(length))) break;
            {
                std::scoped_lock lock(mutex_);
                latest_ = std::move(frame);
            }
        }

        shutdown(socket, SD_BOTH);
        closesocket(socket);
        if (socket_ == socket) socket_ = INVALID_SOCKET;
        if (running_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}
