#include "NetworkReceiver.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <chrono>
#include <stdexcept>

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

NetworkReceiver::NetworkReceiver() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
}

NetworkReceiver::~NetworkReceiver() {
    Stop();
    WSACleanup();
}

void NetworkReceiver::Start(std::wstring host, std::uint16_t port, PacketHandler handler) {
    Stop();
    host_ = std::move(host);
    port_ = port;
    handler_ = std::move(handler);
    running_.store(true);
    thread_ = std::thread(&NetworkReceiver::Run, this);
}

void NetworkReceiver::Stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    connected_.store(false);
}

void NetworkReceiver::Run() {
    while (running_.load()) {
        addrinfoW hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfoW* result = nullptr;
        const auto port = std::to_wstring(port_);
        if (GetAddrInfoW(host_.c_str(), port.c_str(), &hints, &result) != 0) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        SOCKET socket = INVALID_SOCKET;
        for (auto* ptr = result; ptr; ptr = ptr->ai_next) {
            socket = WSASocketW(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol, nullptr, 0, 0);
            if (socket == INVALID_SOCKET) continue;
            BOOL noDelay = TRUE;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            if (connect(socket, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0) break;
            closesocket(socket);
            socket = INVALID_SOCKET;
        }
        FreeAddrInfoW(result);

        if (socket == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        connected_.store(true);
        while (running_.load()) {
            std::uint32_t networkLength = 0;
            if (!recvAll(socket, reinterpret_cast<char*>(&networkLength), sizeof(networkLength))) break;
            const std::uint32_t length = ntohl(networkLength);
            if (length == 0 || length > 16 * 1024 * 1024) break;

            std::vector<std::uint8_t> packet(length);
            if (!recvAll(socket, reinterpret_cast<char*>(packet.data()), static_cast<int>(length))) break;
            if (handler_) handler_(std::move(packet));
        }

        connected_.store(false);
        shutdown(socket, SD_BOTH);
        closesocket(socket);
        if (running_.load()) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
