#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <wrl/client.h>
#include <mftransform.h>

class H264Decoder {
public:
    using FrameHandler = std::function<void(std::vector<std::uint8_t>)>;

    H264Decoder() = default;
    ~H264Decoder();

    HRESULT Initialize(std::uint32_t width, std::uint32_t height, std::uint32_t fps, FrameHandler handler);
    HRESULT Push(const std::vector<std::uint8_t>& packet);
    void Reset();

private:
    HRESULT SubmitAccessUnit(const std::vector<std::uint8_t>& accessUnit, bool keyFrame);
    HRESULT Drain();

    Microsoft::WRL::ComPtr<IMFTransform> decoder_;
    FrameHandler handler_;
    std::vector<std::uint8_t> sps_;
    std::vector<std::uint8_t> pps_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t fps_ = 15;
    LONGLONG timestamp_ = 0;
    bool started_ = false;
};
