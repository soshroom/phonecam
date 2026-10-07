#include "VirtualCamera.h"

#include <windows.h>
#include <ole2.h>
#include <initguid.h>
#include <ks.h>
#include <ksproxy.h>
#include <mfapi.h>
#include <mfidl.h>
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

    // Validate the whole COM activation path before registering the camera.
    // This makes E_NOINTERFACE failures actionable instead of collapsing them into Start().
    Microsoft::WRL::ComPtr<IMFActivate> activation;
    hr = CoCreateInstance(
        sourceClsid,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&activation)
    );
    if (FAILED(hr)) {
        lastStage_ = L"CoCreateInstance(IMFActivate)";
        return hr;
    }

    // Match Microsoft's VirtualCamera sample. Frame Server may populate associated
    // camera sources through this activation attribute even for a synthetic source.
    activation->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1);

    Microsoft::WRL::ComPtr<IMFMediaSource> source;
    hr = activation->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr)) {
        lastStage_ = L"IMFActivate::ActivateObject(IMFMediaSource)";
        return hr;
    }

    Microsoft::WRL::ComPtr<IMFMediaSourceEx> sourceEx;
    hr = source.As(&sourceEx);
    if (FAILED(hr)) {
        lastStage_ = L"QI(IMFMediaSourceEx)";
        return hr;
    }

    Microsoft::WRL::ComPtr<IMFGetService> getService;
    hr = source.As(&getService);
    if (FAILED(hr)) {
        lastStage_ = L"QI(IMFGetService)";
        return hr;
    }

    Microsoft::WRL::ComPtr<IKsControl> ksControl;
    hr = source.As(&ksControl);
    if (FAILED(hr)) {
        lastStage_ = L"QI(IKsControl)";
        return hr;
    }

    Microsoft::WRL::ComPtr<IMFSampleAllocatorControl> allocatorControl;
    hr = source.As(&allocatorControl);
    if (FAILED(hr)) {
        lastStage_ = L"QI(IMFSampleAllocatorControl)";
        return hr;
    }

    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> presentation;
    hr = source->CreatePresentationDescriptor(&presentation);
    if (FAILED(hr)) {
        lastStage_ = L"IMFMediaSource::CreatePresentationDescriptor";
        return hr;
    }

    source->Shutdown();
    source.Reset();
    activation->ShutdownObject();
    activation.Reset();

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

    // Keep the registration attributes aligned with Microsoft's reference sample.
    camera->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1);

    hr = camera->Start(nullptr);
    if (FAILED(hr)) {
        lastStage_ = L"IMFVirtualCamera::Start (activation self-test passed)";
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
