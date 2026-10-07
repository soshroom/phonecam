#include "VirtualCamera.h"

#include <mfapi.h>
#include <objbase.h>

HRESULT VirtualCamera::Start() {
    if (camera_) return S_OK;
    lastStage_.clear();

    CLSID sourceClsid{};
    HRESULT hr = CLSIDFromString(SourceClsid, &sourceClsid);
    if (FAILED(hr)) {
        lastStage_ = L"CLSIDFromString";
        return hr;
    }

    // Validate that the installer actually registered the custom IMFActivate COM class.
    Microsoft::WRL::ComPtr<IUnknown> sourceActivation;
    hr = CoCreateInstance(
        sourceClsid,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&sourceActivation)
    );
    if (FAILED(hr)) {
        lastStage_ = L"CoCreateInstance(PhoneCamSource)";
        return hr;
    }
    sourceActivation.Reset();

    Microsoft::WRL::ComPtr<IMFVirtualCamera> camera;
    hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_CurrentUser,
        L"PhoneCam",
        SourceClsid,
        nullptr,
        0,
        &camera
    );
    if (FAILED(hr)) {
        lastStage_ = L"MFCreateVirtualCamera";
        return hr;
    }

    hr = camera->Start(nullptr);
    if (FAILED(hr)) {
        lastStage_ = L"IMFVirtualCamera::Start";
        return hr;
    }

    camera_ = std::move(camera);
    lastStage_ = L"started";
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
