#pragma once

#include <atomic>
#include <condition_variable>
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
    void SendLoop();

    mutable std::mutex mutex_;
    std::condition_variable frameReady_;
    std::vector<SOCKET> clients_;
    std::vector<std::uint8_t> latestFrame_;
    std::uint64_t frameVersion_ = 0;
    SOCKET listener_ = INVALID_SOCKET;
    std::thread acceptThread_;
    std::thread sendThread_;
    std::atomic_bool running_{false};
    std::uint16_t port_ = 8765;
};
