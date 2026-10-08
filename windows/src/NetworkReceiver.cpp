#include "NetworkReceiver.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <chrono>
#include <stdexcept>

namespace {
constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(-1);

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

void NetworkReceiver::Start(
    std::wstring host,
    std::uint16_t port,
    PacketHandler handler,
    StateHandler stateHandler) {
    Stop();
    host_ = std::move(host);
    port_ = port;
    handler_ = std::move(handler);
    stateHandler_ = std::move(stateHandler);
    running_.store(true);
    thread_ = std::thread(&NetworkReceiver::Run, this);
}

void NetworkReceiver::SetActiveSocket(std::uintptr_t socket) {
    std::scoped_lock lock(socketMutex_);
    activeSocket_ = socket;
}

bool NetworkReceiver::CloseActiveSocket(std::uintptr_t socket) {
    std::scoped_lock lock(socketMutex_);
    if (activeSocket_ != socket || activeSocket_ == kInvalidSocket) return false;
    const SOCKET native = static_cast<SOCKET>(activeSocket_);
    activeSocket_ = kInvalidSocket;
    shutdown(native, SD_BOTH);
    closesocket(native);
    return true;
}

void NetworkReceiver::CloseActiveSocket() {
    std::scoped_lock lock(socketMutex_);
    if (activeSocket_ == kInvalidSocket) return;
    const SOCKET native = static_cast<SOCKET>(activeSocket_);
    activeSocket_ = kInvalidSocket;
    shutdown(native, SD_BOTH);
    closesocket(native);
}

void NetworkReceiver::Stop() {
    running_.store(false);
    CloseActiveSocket();
    if (thread_.joinable()) thread_.join();
    const bool wasConnected = connected_.exchange(false);
    if (wasConnected && stateHandler_) stateHandler_(false);
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
            std::this_thread::sleep_for(std::chrono::milliseconds(750));
            continue;
        }

        SOCKET socket = INVALID_SOCKET;
        for (auto* ptr = result; ptr && running_.load(); ptr = ptr->ai_next) {
            socket = WSASocketW(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol, nullptr, 0, 0);
            if (socket == INVALID_SOCKET) continue;

            SetActiveSocket(static_cast<std::uintptr_t>(socket));

            BOOL noDelay = TRUE;
            BOOL keepAlive = TRUE;
            DWORD receiveTimeoutMs = 3000;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepAlive), sizeof(keepAlive));
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeoutMs), sizeof(receiveTimeoutMs));

            if (connect(socket, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0 && running_.load()) break;
            CloseActiveSocket(static_cast<std::uintptr_t>(socket));
            socket = INVALID_SOCKET;
        }
        FreeAddrInfoW(result);

        if (!running_.load()) break;
        if (socket == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(750));
            continue;
        }

        connected_.store(true);
        if (stateHandler_) stateHandler_(true);

        while (running_.load()) {
            std::uint32_t networkLength = 0;
            if (!recvAll(socket, reinterpret_cast<char*>(&networkLength), sizeof(networkLength))) break;
            const std::uint32_t length = ntohl(networkLength);
            if (length == 0 || length > 16 * 1024 * 1024) break;

            std::vector<std::uint8_t> packet(length);
            if (!recvAll(socket, reinterpret_cast<char*>(packet.data()), static_cast<int>(length))) break;
            if (handler_) handler_(std::move(packet));
        }

        const bool wasConnected = connected_.exchange(false);
        if (wasConnected && stateHandler_) stateHandler_(false);
        CloseActiveSocket(static_cast<std::uintptr_t>(socket));

        if (running_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
