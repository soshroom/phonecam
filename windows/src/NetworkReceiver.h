#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class NetworkReceiver {
public:
    using PacketHandler = std::function<void(std::vector<std::uint8_t>)>;

    NetworkReceiver();
    ~NetworkReceiver();

    void Start(std::wstring host, std::uint16_t port, PacketHandler handler);
    void Stop();
    bool IsConnected() const noexcept { return connected_.load(); }

private:
    void Run();

    std::wstring host_;
    std::uint16_t port_ = 8554;
    PacketHandler handler_;
    std::thread thread_;
    std::atomic_bool running_{false};
    std::atomic_bool connected_{false};
};
