#include "FrameBridge.h"

#include <WS2tcpip.h>
#include <algorithm>

namespace {
bool sendAll(SOCKET socket, const char* data, int bytes) {
    int sent = 0;
    while (sent < bytes) {
        const int n = send(socket, data + sent, bytes - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}
}

FrameBridge::~FrameBridge() {
    Stop();
}

bool FrameBridge::Start(std::uint16_t port) {
    Stop();
    port_ = port;

    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) return false;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    BOOL reuse = TRUE;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener_, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listener_);
        listener_ = INVALID_SOCKET;
        return false;
    }

    {
        std::scoped_lock lock(mutex_);
        latestFrame_.clear();
        frameVersion_ = 0;
    }

    running_.store(true);
    acceptThread_ = std::thread(&FrameBridge::AcceptLoop, this);
    sendThread_ = std::thread(&FrameBridge::SendLoop, this);
    return true;
}

void FrameBridge::Stop() {
    running_.store(false);
    frameReady_.notify_all();

    if (listener_ != INVALID_SOCKET) {
        shutdown(listener_, SD_BOTH);
        closesocket(listener_);
        listener_ = INVALID_SOCKET;
    }
    if (acceptThread_.joinable()) acceptThread_.join();

    {
        std::scoped_lock lock(mutex_);
        for (SOCKET socket : clients_) {
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        }
        clients_.clear();
    }

    frameReady_.notify_all();
    if (sendThread_.joinable()) sendThread_.join();

    std::scoped_lock lock(mutex_);
    latestFrame_.clear();
    frameVersion_ = 0;
}

void FrameBridge::AcceptLoop() {
    while (running_.load()) {
        SOCKET client = accept(listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        BOOL noDelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        std::scoped_lock lock(mutex_);
        clients_.push_back(client);
    }
}

void FrameBridge::SendLoop() {
    std::uint64_t sentVersion = 0;

    while (running_.load()) {
        std::vector<std::uint8_t> frame;
        std::vector<SOCKET> clients;
        std::uint64_t version = 0;

        {
            std::unique_lock lock(mutex_);
            frameReady_.wait(lock, [&] {
                return !running_.load() || frameVersion_ != sentVersion;
            });
            if (!running_.load()) break;

            version = frameVersion_;
            frame = latestFrame_;
            clients = clients_;
        }

        if (frame.empty()) {
            sentVersion = version;
            continue;
        }

        const std::uint32_t size = htonl(static_cast<std::uint32_t>(frame.size()));
        std::vector<SOCKET> failed;
        failed.reserve(clients.size());
        for (SOCKET socket : clients) {
            if (!sendAll(socket, reinterpret_cast<const char*>(&size), sizeof(size)) ||
                !sendAll(socket, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()))) {
                failed.push_back(socket);
            }
        }

        if (!failed.empty()) {
            std::scoped_lock lock(mutex_);
            for (SOCKET socket : failed) {
                auto it = std::find(clients_.begin(), clients_.end(), socket);
                if (it == clients_.end()) continue;
                shutdown(socket, SD_BOTH);
                closesocket(socket);
                clients_.erase(it);
            }
        }

        sentVersion = version;
    }
}

void FrameBridge::Publish(const std::vector<std::uint8_t>& frame) {
    if (frame.empty() || !running_.load()) return;

    {
        std::scoped_lock lock(mutex_);
        latestFrame_ = frame;
        ++frameVersion_;
    }
    frameReady_.notify_one();
}

std::size_t FrameBridge::ClientCount() const {
    std::scoped_lock lock(mutex_);
    return clients_.size();
}
