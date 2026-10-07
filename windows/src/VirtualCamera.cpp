#include "VirtualCamera.h"

#include <mfapi.h>

HRESULT VirtualCamera::Start() {
    if (camera_) return S_OK;

    Microsoft::WRL::ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_CurrentUser,
        L"PhoneCam",
        SourceClsid,
        nullptr,
        0,
        &camera
    );
    if (FAILED(hr)) return hr;

    hr = camera->Start(nullptr);
    if (FAILED(hr)) return hr;

    camera_ = std::move(camera);
    return S_OK;
}

void VirtualCamera::Stop() {
    if (!camera_) return;
    camera_->Stop();
    camera_->Shutdown();
    camera_.Reset();
}

HRESULT VirtualCamera::Remove() {
    Microsoft::WRL::ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_CurrentUser,
        L"PhoneCam",
        SourceClsid,
        nullptr,
        0,
        &camera
    );
    if (FAILED(hr)) return hr;
    return camera->Remove();
}
