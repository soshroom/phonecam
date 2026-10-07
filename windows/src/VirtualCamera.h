#pragma once

#include <mfvirtualcamera.h>
#include <wrl/client.h>
#include <string>

class VirtualCamera {
public:
    HRESULT Start();
    void Stop();
    HRESULT Remove();
    bool IsStarted() const noexcept { return camera_ != nullptr; }
    const std::wstring& LastStage() const noexcept { return lastStage_; }

    static constexpr wchar_t SourceClsid[] = L"{B6E2A98D-6F02-4C98-86F8-5AE30A2D17C2}";

private:
    Microsoft::WRL::ComPtr<IMFVirtualCamera> camera_;
    std::wstring lastStage_;
};
