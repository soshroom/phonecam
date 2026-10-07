#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <WinSock2.h>

class FrameClient {
public:
    FrameClient();
    ~FrameClient();

    void Start(std::uint16_t port = 8765);
    void Stop();
    bool Latest(std::vector<std::uint8_t>& frame) const;

private:
    void Run();

    mutable std::mutex mutex_;
    std::vector<std::uint8_t> latest_;
    std::atomic_bool running_{false};
    std::thread thread_;
    SOCKET socket_ = INVALID_SOCKET;
    std::uint16_t port_ = 8765;
    bool winsockStarted_ = false;
};
