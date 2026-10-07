#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <WinSock2.h>

class FrameBridge {
public:
    FrameBridge() = default;
    ~FrameBridge();

    bool Start(std::uint16_t port = 8765);
    void Stop();
    void Publish(const std::vector<std::uint8_t>& frame);
    std::size_t ClientCount() const;

private:
    void AcceptLoop();

    mutable std::mutex mutex_;
    std::vector<SOCKET> clients_;
    SOCKET listener_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic_bool running_{false};
    std::uint16_t port_ = 8765;
};
