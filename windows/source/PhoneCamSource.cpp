#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {

// Must match VirtualCamera::SourceClsid in the controller application.
// {B6E2A98D-6F02-4C98-86F8-5AE30A2D17C2}
const CLSID CLSID_PhoneCamSource =
{0xb6e2a98d, 0x6f02, 0x4c98, {0x86, 0xf8, 0x5a, 0xe3, 0x0a, 0x2d, 0x17, 0xc2}};

constexpr UINT32 kWidth = 1280;
constexpr UINT32 kHeight = 720;
constexpr UINT32 kFps = 30;
constexpr LONGLONG kFrameDuration = 10'000'000LL / kFps;
constexpr DWORD kStreamId = 0;

class PhoneCamSource;

class PhoneCamStream final :
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaStream, IMFMediaStream2> {
public:
    HRESULT RuntimeClassInitialize(PhoneCamSource* source);
    HRESULT Start(const PROPVARIANT* position);
    HRESULT Stop();
    HRESULT ShutdownStream();
    IMFAttributes* Attributes() const noexcept { return attributes_.Get(); }

    // IMFMediaEventGenerator
    IFACEMETHOD(BeginGetEvent)(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHOD(EndGetEvent)(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHOD(GetEvent)(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHOD(QueueEvent)(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    IFACEMETHOD(GetMediaSource)(IMFMediaSource** source) override;
    IFACEMETHOD(GetStreamDescriptor)(IMFStreamDescriptor** descriptor) override;
    IFACEMETHOD(RequestSample)(IUnknown* token) override;

    // IMFMediaStream2
    IFACEMETHOD(SetStreamState)(MF_STREAM_STATE state) override;
    IFACEMETHOD(GetStreamState)(MF_STREAM_STATE* state) override;

private:
    HRESULT CheckShutdown() const noexcept { return shutdown_ ? MF_E_SHUTDOWN : S_OK; }
    HRESULT CreateSample(IMFSample** sample);

    std::mutex mutex_;
    PhoneCamSource* source_ = nullptr; // Parent owns the stream.
    ComPtr<IMFMediaEventQueue> events_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFAttributes> attributes_;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    bool shutdown_ = false;
    LONGLONG nextTimestamp_ = 0;
    UINT64 frameNumber_ = 0;
};

class PhoneCamSource final :
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaSource, IMFMediaSourceEx> {
public:
    HRESULT RuntimeClassInitialize();
    HRESULT ShutdownSource();

    // IMFMediaEventGenerator
    IFACEMETHOD(BeginGetEvent)(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHOD(EndGetEvent)(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHOD(GetEvent)(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHOD(QueueEvent)(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaSource
    IFACEMETHOD(GetCharacteristics)(DWORD* characteristics) override;
    IFACEMETHOD(CreatePresentationDescriptor)(IMFPresentationDescriptor** descriptor) override;
    IFACEMETHOD(Start)(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) override;
    IFACEMETHOD(Stop)() override;
    IFACEMETHOD(Pause)() override;
    IFACEMETHOD(Shutdown)() override { return ShutdownSource(); }

    // IMFMediaSourceEx
    IFACEMETHOD(GetSourceAttributes)(IMFAttributes** attributes) override;
    IFACEMETHOD(GetStreamAttributes)(DWORD streamId, IMFAttributes** attributes) override;
    IFACEMETHOD(SetD3DManager)(IUnknown*) override { return S_OK; }

private:
    HRESULT CheckShutdown() const noexcept { return shutdown_ ? MF_E_SHUTDOWN : S_OK; }

    std::mutex mutex_;
    ComPtr<IMFMediaEventQueue> events_;
    ComPtr<IMFPresentationDescriptor> presentation_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<PhoneCamStream> stream_;
    bool shutdown_ = false;
};

HRESULT PhoneCamStream::RuntimeClassInitialize(PhoneCamSource* source) {
    if (!source) return E_INVALIDARG;
    source_ = source;

    HRESULT hr = MFCreateEventQueue(&events_);
    if (FAILED(hr)) return hr;

    hr = MFCreateAttributes(&attributes_, 8);
    if (FAILED(hr)) return hr;
    attributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, kStreamId);
    attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

    ComPtr<IMFMediaType> type;
    hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return hr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_DEFAULT_STRIDE, kWidth * 4);
    type->SetUINT32(MF_MT_AVG_BITRATE, kWidth * kHeight * 4 * 8 * kFps);

    IMFMediaType* types[] = { type.Get() };
    hr = MFCreateStreamDescriptor(kStreamId, 1, types, &descriptor_);
    if (FAILED(hr)) return hr;

    descriptor_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    descriptor_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, kStreamId);
    descriptor_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    descriptor_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

    ComPtr<IMFMediaTypeHandler> handler;
    hr = descriptor_->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    return handler->SetCurrentMediaType(type.Get());
}

HRESULT PhoneCamStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : events_->BeginGetEvent(callback, state);
}

HRESULT PhoneCamStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : events_->EndGetEvent(result, event);
}

HRESULT PhoneCamStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::scoped_lock lock(mutex_);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        queue = events_;
    }
    return queue->GetEvent(flags, event);
}

HRESULT PhoneCamStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : events_->QueueEventParamVar(type, extendedType, status, value);
}

HRESULT PhoneCamStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    *source = nullptr;
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return source_->QueryInterface(IID_PPV_ARGS(source));
}

HRESULT PhoneCamStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    *descriptor = nullptr;
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return descriptor_.CopyTo(descriptor);
}

HRESULT PhoneCamStream::CreateSample(IMFSample** sample) {
    if (!sample) return E_POINTER;
    *sample = nullptr;

    const DWORD bytes = kWidth * kHeight * 4;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
    if (FAILED(hr)) return hr;

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    hr = buffer->Lock(&data, &maxLength, nullptr);
    if (FAILED(hr)) return hr;

    const UINT32 phase = static_cast<UINT32>((frameNumber_ / 3) % kWidth);
    for (UINT32 y = 0; y < kHeight; ++y) {
        auto* row = reinterpret_cast<UINT32*>(data + y * kWidth * 4);
        for (UINT32 x = 0; x < kWidth; ++x) {
            const UINT32 band = ((x + phase) / 160) % 6;
            UINT32 color = 0x00202020;
            switch (band) {
                case 0: color = 0x00E05050; break;
                case 1: color = 0x0050E050; break;
                case 2: color = 0x005050E0; break;
                case 3: color = 0x00E0E050; break;
                case 4: color = 0x00E050E0; break;
                case 5: color = 0x0050E0E0; break;
            }
            // Darken alternating scan bands so movement is obvious.
            if (((y / 64) & 1) != 0) color = (color >> 1) & 0x007F7F7F;
            row[x] = color;
        }
    }
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);

    ComPtr<IMFSample> result;
    hr = MFCreateSample(&result);
    if (FAILED(hr)) return hr;
    hr = result->AddBuffer(buffer.Get());
    if (FAILED(hr)) return hr;
    result->SetSampleTime(nextTimestamp_);
    result->SetSampleDuration(kFrameDuration);
    nextTimestamp_ += kFrameDuration;
    ++frameNumber_;
    return result.CopyTo(sample);
}

HRESULT PhoneCamStream::RequestSample(IUnknown* token) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;

    ComPtr<IMFSample> sample;
    hr = CreateSample(&sample);
    if (FAILED(hr)) return hr;
    if (token) sample->SetUnknown(MFSampleExtension_Token, token);
    return events_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

HRESULT PhoneCamStream::Start(const PROPVARIANT* position) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    state_ = MF_STREAM_STATE_RUNNING;
    if (position && position->vt == VT_I8) nextTimestamp_ = position->hVal.QuadPart;
    else nextTimestamp_ = 0;
    return events_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, position);
}

HRESULT PhoneCamStream::Stop() {
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    state_ = MF_STREAM_STATE_STOPPED;
    return events_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT PhoneCamStream::SetStreamState(MF_STREAM_STATE state) {
    if (state == MF_STREAM_STATE_RUNNING) return Start(nullptr);
    if (state == MF_STREAM_STATE_STOPPED) return Stop();
    if (state == MF_STREAM_STATE_PAUSED) {
        std::scoped_lock lock(mutex_);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        state_ = state;
        return S_OK;
    }
    return E_INVALIDARG;
}

HRESULT PhoneCamStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    *state = state_;
    return S_OK;
}

HRESULT PhoneCamStream::ShutdownStream() {
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

HRESULT PhoneCamSource::RuntimeClassInitialize() {
    HRESULT hr = MFCreateEventQueue(&events_);
    if (FAILED(hr)) return hr;

    hr = MFCreateAttributes(&attributes_, 8);
    if (FAILED(hr)) return hr;
    attributes_->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attributes_->SetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, L"PhoneCam");

    stream_ = Make<PhoneCamStream>();
    if (!stream_) return E_OUTOFMEMORY;
    hr = stream_->RuntimeClassInitialize(this);
    if (FAILED(hr)) return hr;

    ComPtr<IMFStreamDescriptor> streamDescriptor;
    hr = stream_->GetStreamDescriptor(&streamDescriptor);
    if (FAILED(hr)) return hr;
    IMFStreamDescriptor* descriptors[] = { streamDescriptor.Get() };
    hr = MFCreatePresentationDescriptor(1, descriptors, &presentation_);
    if (FAILED(hr)) return hr;
    return presentation_->SelectStream(0);
}

HRESULT PhoneCamSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : events_->BeginGetEvent(callback, state);
}

HRESULT PhoneCamSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : events_->EndGetEvent(result, event);
}

HRESULT PhoneCamSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::scoped_lock lock(mutex_);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        queue = events_;
    }
    return queue->GetEvent(flags, event);
}

HRESULT PhoneCamSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) {
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : events_->QueueEventParamVar(type, extendedType, status, value);
}

HRESULT PhoneCamSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

HRESULT PhoneCamSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    *descriptor = nullptr;
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return presentation_->Clone(descriptor);
}

HRESULT PhoneCamSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) {
    if (!descriptor) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> streamDescriptor;
    hr = descriptor->GetStreamDescriptorByIndex(0, &selected, &streamDescriptor);
    if (FAILED(hr)) return hr;
    if (!selected) return MF_E_INVALIDREQUEST;

    hr = events_->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK, stream_.Get());
    if (FAILED(hr)) return hr;
    hr = stream_->Start(startPosition);
    if (FAILED(hr)) return hr;
    return events_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, startPosition);
}

HRESULT PhoneCamSource::Stop() {
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    hr = stream_->Stop();
    if (FAILED(hr)) return hr;
    return events_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT PhoneCamSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

HRESULT PhoneCamSource::ShutdownSource() {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return S_OK;
    shutdown_ = true;
    if (stream_) stream_->ShutdownStream();
    stream_.Reset();
    presentation_.Reset();
    attributes_.Reset();
    if (events_) events_->Shutdown();
    events_.Reset();
    return S_OK;
}

HRESULT PhoneCamSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return attributes_.CopyTo(attributes);
}

HRESULT PhoneCamSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::scoped_lock lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    if (streamId != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
    if (!stream_ || !stream_->Attributes()) return E_UNEXPECTED;
    stream_->Attributes()->AddRef();
    *attributes = stream_->Attributes();
    return S_OK;
}

class PhoneCamActivate final :
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFActivate> {
public:
    HRESULT RuntimeClassInitialize() { return MFCreateAttributes(&attributes_, 8); }

    IFACEMETHOD(ActivateObject)(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        auto source = Make<PhoneCamSource>();
        if (!source) return E_OUTOFMEMORY;
        HRESULT hr = source->RuntimeClassInitialize();
        if (FAILED(hr)) return hr;
        return source.CopyTo(riid, object);
    }
    IFACEMETHOD(ShutdownObject)() override { return S_OK; }
    IFACEMETHOD(DetachObject)() override { return S_OK; }

    // IMFAttributes forwarding
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

class PhoneCamClassFactory final :
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    IFACEMETHOD(CreateInstance)(IUnknown* outer, REFIID riid, void** object) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!object) return E_POINTER;
        *object = nullptr;
        auto activate = Make<PhoneCamActivate>();
        if (!activate) return E_OUTOFMEMORY;
        HRESULT hr = activate->RuntimeClassInitialize();
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

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    // Frame Server owns the process lifetime. Keeping the module loaded is safer for the MVP.
    return S_FALSE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (clsid != CLSID_PhoneCamSource) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = Make<PhoneCamClassFactory>();
    if (!factory) return E_OUTOFMEMORY;
    return factory.CopyTo(riid, object);
}
