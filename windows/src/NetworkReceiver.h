#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class NetworkReceiver {
public:
    using PacketHandler = std::function<void(std::vector<std::uint8_t>)>;
    using StateHandler = std::function<void(bool connected)>;

    NetworkReceiver();
    ~NetworkReceiver();

    void Start(std::wstring host, std::uint16_t port, PacketHandler handler, StateHandler stateHandler = {});
    void Stop();
    bool IsConnected() const noexcept { return connected_.load(); }

private:
    void Run();
    void SetActiveSocket(std::uintptr_t socket);
    bool CloseActiveSocket(std::uintptr_t socket);
    void CloseActiveSocket();

    std::wstring host_;
    std::uint16_t port_ = 8554;
    PacketHandler handler_;
    StateHandler stateHandler_;
    std::thread thread_;
    std::atomic_bool running_{false};
    std::atomic_bool connected_{false};
    std::mutex socketMutex_;
    std::uintptr_t activeSocket_ = static_cast<std::uintptr_t>(-1);
};
