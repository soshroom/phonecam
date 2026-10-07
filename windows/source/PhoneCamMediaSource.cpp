#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <cstdint>
#include <mutex>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {

const CLSID CLSID_PhoneCamSource =
{0xb6e2a98d, 0x6f02, 0x4c98, {0x86, 0xf8, 0x5a, 0xe3, 0x0a, 0x2d, 0x17, 0xc2}};

constexpr UINT32 kWidth = 640;
constexpr UINT32 kHeight = 480;
constexpr UINT32 kFps = 15;
constexpr DWORD kStreamId = 0;
constexpr LONGLONG kFrameDuration = 10'000'000LL / kFps;
constexpr DWORD kFrameBytes = kWidth * kHeight * 3 / 2;

class CameraSource;

class CameraStream final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaStream2> {
public:
    HRESULT Initialize(CameraSource* source);
    HRESULT Start(const PROPVARIANT* position);
    HRESULT StopInternal();
    HRESULT ShutdownInternal();
    IMFAttributes* Attributes() const noexcept { return attributes_.Get(); }

    IFACEMETHOD(BeginGetEvent)(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHOD(EndGetEvent)(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHOD(GetEvent)(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHOD(QueueEvent)(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) override;

    IFACEMETHOD(GetMediaSource)(IMFMediaSource** source) override;
    IFACEMETHOD(GetStreamDescriptor)(IMFStreamDescriptor** descriptor) override;
    IFACEMETHOD(RequestSample)(IUnknown* token) override;

    IFACEMETHOD(SetStreamState)(MF_STREAM_STATE state) override;
    IFACEMETHOD(GetStreamState)(MF_STREAM_STATE* state) override;

private:
    HRESULT Check() const noexcept { return shutdown_ ? MF_E_SHUTDOWN : S_OK; }
    HRESULT MakeFrame(IMFSample** sample);

    mutable std::mutex mutex_;
    CameraSource* source_ = nullptr;
    ComPtr<IMFMediaEventQueue> events_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFAttributes> attributes_;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    bool shutdown_ = false;
    LONGLONG timestamp_ = 0;
    UINT64 frame_ = 0;
};

class CameraSource final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaSourceEx> {
public:
    HRESULT Initialize();

    IFACEMETHOD(BeginGetEvent)(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHOD(EndGetEvent)(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHOD(GetEvent)(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHOD(QueueEvent)(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) override;

    IFACEMETHOD(GetCharacteristics)(DWORD* characteristics) override;
    IFACEMETHOD(CreatePresentationDescriptor)(IMFPresentationDescriptor** descriptor) override;
    IFACEMETHOD(Start)(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) override;
    IFACEMETHOD(Stop)() override;
    IFACEMETHOD(Pause)() override { return MF_E_INVALID_STATE_TRANSITION; }
    IFACEMETHOD(Shutdown)() override;

    IFACEMETHOD(GetSourceAttributes)(IMFAttributes** attributes) override;
    IFACEMETHOD(GetStreamAttributes)(DWORD streamId, IMFAttributes** attributes) override;
    IFACEMETHOD(SetD3DManager)(IUnknown*) override { return S_OK; }

private:
    HRESULT Check() const noexcept { return shutdown_ ? MF_E_SHUTDOWN : S_OK; }

    mutable std::mutex mutex_;
    ComPtr<IMFMediaEventQueue> events_;
    ComPtr<IMFPresentationDescriptor> presentation_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<CameraStream> stream_;
    bool shutdown_ = false;
    bool announcedStream_ = false;
};

HRESULT CameraStream::Initialize(CameraSource* source) {
    if (!source) return E_INVALIDARG;
    source_ = source;

    HRESULT hr = MFCreateEventQueue(&events_);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&attributes_, 8);
    if (FAILED(hr)) return hr;

    attributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, kStreamId);
    attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
        static_cast<UINT32>(MFFrameSourceTypes::MFFrameSourceTypes_Color));

    ComPtr<IMFMediaType> mediaType;
    hr = MFCreateMediaType(&mediaType);
    if (FAILED(hr)) return hr;
    mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    mediaType->SetUINT32(MF_MT_AVG_BITRATE, kFrameBytes * 8 * kFps);
    MFSetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
    MFSetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(mediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    IMFMediaType* types[] = { mediaType.Get() };
    hr = MFCreateStreamDescriptor(kStreamId, 1, types, &descriptor_);
    if (FAILED(hr)) return hr;
    descriptor_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    descriptor_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, kStreamId);
    descriptor_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    descriptor_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
        static_cast<UINT32>(MFFrameSourceTypes::MFFrameSourceTypes_Color));

    ComPtr<IMFMediaTypeHandler> handler;
    hr = descriptor_->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    return handler->SetCurrentMediaType(mediaType.Get());
}

HRESULT CameraStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    return FAILED(hr) ? hr : events_->BeginGetEvent(callback, state);
}

HRESULT CameraStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    return FAILED(hr) ? hr : events_->EndGetEvent(result, event);
}

HRESULT CameraStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::scoped_lock lock(mutex_);
        HRESULT hr = Check();
        if (FAILED(hr)) return hr;
        queue = events_;
    }
    return queue->GetEvent(flags, event);
}

HRESULT CameraStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    return FAILED(hr) ? hr : events_->QueueEventParamVar(type, extendedType, status, value);
}

HRESULT CameraStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    *source = nullptr;
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    return source_->QueryInterface(IID_PPV_ARGS(source));
}

HRESULT CameraStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    *descriptor = nullptr;
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    return descriptor_.CopyTo(descriptor);
}

HRESULT CameraStream::MakeFrame(IMFSample** sample) {
    if (!sample) return E_POINTER;
    *sample = nullptr;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(kFrameBytes, &buffer);
    if (FAILED(hr)) return hr;

    BYTE* bytes = nullptr;
    DWORD capacity = 0;
    hr = buffer->Lock(&bytes, &capacity, nullptr);
    if (FAILED(hr)) return hr;

    const UINT32 phase = static_cast<UINT32>((frame_ * 4) % 256);
    BYTE* yPlane = bytes;
    BYTE* uvPlane = bytes + kWidth * kHeight;
    for (UINT32 y = 0; y < kHeight; ++y) {
        for (UINT32 x = 0; x < kWidth; ++x) {
            yPlane[y * kWidth + x] = static_cast<BYTE>(32 + ((x + phase + y / 2) % 192));
        }
    }
    for (UINT32 i = 0; i < kWidth * kHeight / 2; i += 2) {
        uvPlane[i] = static_cast<BYTE>(96 + ((frame_ / 3) % 64));
        uvPlane[i + 1] = static_cast<BYTE>(160 - ((frame_ / 4) % 64));
    }
    buffer->Unlock();
    buffer->SetCurrentLength(kFrameBytes);

    ComPtr<IMFSample> result;
    hr = MFCreateSample(&result);
    if (FAILED(hr)) return hr;
    hr = result->AddBuffer(buffer.Get());
    if (FAILED(hr)) return hr;
    result->SetSampleTime(timestamp_);
    result->SetSampleDuration(kFrameDuration);
    timestamp_ += kFrameDuration;
    ++frame_;
    return result.CopyTo(sample);
}

HRESULT CameraStream::RequestSample(IUnknown* token) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;

    ComPtr<IMFSample> sample;
    hr = MakeFrame(&sample);
    if (FAILED(hr)) return hr;
    if (token) sample->SetUnknown(MFSampleExtension_Token, token);
    return events_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

HRESULT CameraStream::Start(const PROPVARIANT* position) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    state_ = MF_STREAM_STATE_RUNNING;
    timestamp_ = position && position->vt == VT_I8 ? position->hVal.QuadPart : 0;
    return events_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, position);
}

HRESULT CameraStream::StopInternal() {
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    state_ = MF_STREAM_STATE_STOPPED;
    return events_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT CameraStream::SetStreamState(MF_STREAM_STATE state) {
    switch (state) {
        case MF_STREAM_STATE_RUNNING: return Start(nullptr);
        case MF_STREAM_STATE_STOPPED: return StopInternal();
        case MF_STREAM_STATE_PAUSED: {
            std::scoped_lock lock(mutex_);
            HRESULT hr = Check();
            if (FAILED(hr)) return hr;
            state_ = state;
            return S_OK;
        }
        default: return E_INVALIDARG;
    }
}

HRESULT CameraStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    *state = state_;
    return S_OK;
}

HRESULT CameraStream::ShutdownInternal() {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return S_OK;
    shutdown_ = true;
    state_ = MF_STREAM_STATE_STOPPED;
    if (events_) events_->Shutdown();
    events_.Reset();
    descriptor_.Reset();
    attributes_.Reset();
    source_ = nullptr;
    return S_OK;
}

HRESULT CameraSource::Initialize() {
    HRESULT hr = MFCreateEventQueue(&events_);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&attributes_, 8);
    if (FAILED(hr)) return hr;
    attributes_->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attributes_->SetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, L"PhoneCam");

    stream_ = Make<CameraStream>();
    if (!stream_) return E_OUTOFMEMORY;
    hr = stream_->Initialize(this);
    if (FAILED(hr)) return hr;

    ComPtr<IMFStreamDescriptor> streamDescriptor;
    hr = stream_->GetStreamDescriptor(&streamDescriptor);
    if (FAILED(hr)) return hr;
    IMFStreamDescriptor* descriptors[] = { streamDescriptor.Get() };
    hr = MFCreatePresentationDescriptor(1, descriptors, &presentation_);
    if (FAILED(hr)) return hr;
    return presentation_->SelectStream(0);
}

HRESULT CameraSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    return FAILED(hr) ? hr : events_->BeginGetEvent(callback, state);
}

HRESULT CameraSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    return FAILED(hr) ? hr : events_->EndGetEvent(result, event);
}

HRESULT CameraSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::scoped_lock lock(mutex_);
        HRESULT hr = Check();
        if (FAILED(hr)) return hr;
        queue = events_;
    }
    return queue->GetEvent(flags, event);
}

HRESULT CameraSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    return FAILED(hr) ? hr : events_->QueueEventParamVar(type, extendedType, status, value);
}

HRESULT CameraSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

HRESULT CameraSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    *descriptor = nullptr;
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    return presentation_->Clone(descriptor);
}

HRESULT CameraSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) {
    if (!descriptor) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> selectedDescriptor;
    hr = descriptor->GetStreamDescriptorByIndex(0, &selected, &selectedDescriptor);
    if (FAILED(hr)) return hr;
    if (!selected) return MF_E_INVALIDREQUEST;

    ComPtr<IMFMediaStream> mediaStream;
    hr = stream_.As(&mediaStream);
    if (FAILED(hr)) return hr;
    hr = events_->QueueEventParamUnk(announcedStream_ ? MEUpdatedStream : MENewStream,
        GUID_NULL, S_OK, mediaStream.Get());
    if (FAILED(hr)) return hr;
    announcedStream_ = true;

    hr = stream_->Start(startPosition);
    if (FAILED(hr)) return hr;
    return events_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, startPosition);
}

HRESULT CameraSource::Stop() {
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    hr = stream_->StopInternal();
    if (FAILED(hr)) return hr;
    return events_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT CameraSource::Shutdown() {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return S_OK;
    shutdown_ = true;
    if (stream_) stream_->ShutdownInternal();
    stream_.Reset();
    presentation_.Reset();
    attributes_.Reset();
    if (events_) events_->Shutdown();
    events_.Reset();
    return S_OK;
}

HRESULT CameraSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    return attributes_.CopyTo(attributes);
}

HRESULT CameraSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::scoped_lock lock(mutex_);
    HRESULT hr = Check();
    if (FAILED(hr)) return hr;
    if (streamId != kStreamId || !stream_ || !stream_->Attributes()) return MF_E_INVALIDSTREAMNUMBER;
    *attributes = stream_->Attributes();
    (*attributes)->AddRef();
    return S_OK;
}

class CameraActivate final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFActivate> {
public:
    HRESULT Initialize() { return MFCreateAttributes(&attributes_, 8); }

    IFACEMETHOD(ActivateObject)(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        auto source = Make<CameraSource>();
        if (!source) return E_OUTOFMEMORY;
        HRESULT hr = source->Initialize();
        if (FAILED(hr)) return hr;
        return source.CopyTo(riid, object);
    }
    IFACEMETHOD(ShutdownObject)() override { return S_OK; }
    IFACEMETHOD(DetachObject)() override { return S_OK; }

    IFACEMETHOD(GetItem)(REFGUID k, PROPVARIANT* v) override { return attributes_->GetItem(k, v); }
    IFACEMETHOD(GetItemType)(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attributes_->GetItemType(k, t); }
    IFACEMETHOD(CompareItem)(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attributes_->CompareItem(k, v, r); }
    IFACEMETHOD(Compare)(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return attributes_->Compare(a, m, r); }
    IFACEMETHOD(GetUINT32)(REFGUID k, UINT32* v) override { return attributes_->GetUINT32(k, v); }
    IFACEMETHOD(GetUINT64)(REFGUID k, UINT64* v) override { return attributes_->GetUINT64(k, v); }
    IFACEMETHOD(GetDouble)(REFGUID k, double* v) override { return attributes_->GetDouble(k, v); }
    IFACEMETHOD(GetGUID)(REFGUID k, GUID* v) override { return attributes_->GetGUID(k, v); }
    IFACEMETHOD(GetStringLength)(REFGUID k, UINT32* v) override { return attributes_->GetStringLength(k, v); }
    IFACEMETHOD(GetString)(REFGUID k, LPWSTR v, UINT32 s, UINT32* l) override { return attributes_->GetString(k, v, s, l); }
    IFACEMETHOD(GetAllocatedString)(REFGUID k, LPWSTR* v, UINT32* l) override { return attributes_->GetAllocatedString(k, v, l); }
    IFACEMETHOD(GetBlobSize)(REFGUID k, UINT32* v) override { return attributes_->GetBlobSize(k, v); }
    IFACEMETHOD(GetBlob)(REFGUID k, UINT8* v, UINT32 s, UINT32* l) override { return attributes_->GetBlob(k, v, s, l); }
    IFACEMETHOD(GetAllocatedBlob)(REFGUID k, UINT8** v, UINT32* l) override { return attributes_->GetAllocatedBlob(k, v, l); }
    IFACEMETHOD(GetUnknown)(REFGUID k, REFIID i, LPVOID* v) override { return attributes_->GetUnknown(k, i, v); }
    IFACEMETHOD(SetItem)(REFGUID k, REFPROPVARIANT v) override { return attributes_->SetItem(k, v); }
    IFACEMETHOD(DeleteItem)(REFGUID k) override { return attributes_->DeleteItem(k); }
    IFACEMETHOD(DeleteAllItems)() override { return attributes_->DeleteAllItems(); }
    IFACEMETHOD(SetUINT32)(REFGUID k, UINT32 v) override { return attributes_->SetUINT32(k, v); }
    IFACEMETHOD(SetUINT64)(REFGUID k, UINT64 v) override { return attributes_->SetUINT64(k, v); }
    IFACEMETHOD(SetDouble)(REFGUID k, double v) override { return attributes_->SetDouble(k, v); }
    IFACEMETHOD(SetGUID)(REFGUID k, REFGUID v) override { return attributes_->SetGUID(k, v); }
    IFACEMETHOD(SetString)(REFGUID k, LPCWSTR v) override { return attributes_->SetString(k, v); }
    IFACEMETHOD(SetBlob)(REFGUID k, const UINT8* v, UINT32 s) override { return attributes_->SetBlob(k, v, s); }
    IFACEMETHOD(SetUnknown)(REFGUID k, IUnknown* v) override { return attributes_->SetUnknown(k, v); }
    IFACEMETHOD(LockStore)() override { return attributes_->LockStore(); }
    IFACEMETHOD(UnlockStore)() override { return attributes_->UnlockStore(); }
    IFACEMETHOD(GetCount)(UINT32* c) override { return attributes_->GetCount(c); }
    IFACEMETHOD(GetItemByIndex)(UINT32 i, GUID* k, PROPVARIANT* v) override { return attributes_->GetItemByIndex(i, k, v); }
    IFACEMETHOD(CopyAllItems)(IMFAttributes* d) override { return attributes_->CopyAllItems(d); }

private:
    ComPtr<IMFAttributes> attributes_;
};

class CameraClassFactory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    IFACEMETHOD(CreateInstance)(IUnknown* outer, REFIID riid, void** object) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!object) return E_POINTER;
        *object = nullptr;
        auto activate = Make<CameraActivate>();
        if (!activate) return E_OUTOFMEMORY;
        HRESULT hr = activate->Initialize();
        if (FAILED(hr)) return hr;
        return activate.CopyTo(riid, object);
    }
    IFACEMETHOD(LockServer)(BOOL) override { return S_OK; }
};

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (clsid != CLSID_PhoneCamSource) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = Make<CameraClassFactory>();
    if (!factory) return E_OUTOFMEMORY;
    return factory.CopyTo(riid, object);
}
