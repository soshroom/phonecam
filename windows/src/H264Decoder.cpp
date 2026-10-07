#include "H264Decoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <algorithm>
#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;

H264Decoder::~H264Decoder() {
    Reset();
}

HRESULT H264Decoder::Initialize(std::uint32_t width, std::uint32_t height, std::uint32_t fps, FrameHandler handler) {
    Reset();
    width_ = width;
    height_ = height;
    fps_ = fps ? fps : 15;
    handler_ = std::move(handler);
    timestamp_ = 0;

    MFT_REGISTER_TYPE_INFO inputInfo{MFMediaType_Video, MFVideoFormat_H264};
    MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputInfo,
        &outputInfo,
        &activates,
        &count
    );
    if (FAILED(hr)) return hr;
    if (count == 0) {
        CoTaskMemFree(activates);
        return MF_E_TOPO_CODEC_NOT_FOUND;
    }

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&decoder_));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return hr;
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps_, 1);
    hr = decoder_->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) return hr;
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = decoder_->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr)) return hr;

    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return S_OK;
}

HRESULT H264Decoder::Push(const std::vector<std::uint8_t>& packet) {
    if (!decoder_) return MF_E_NOT_INITIALIZED;
    if (packet.empty()) return S_OK;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(packet.size()), &buffer);
    if (FAILED(hr)) return hr;

    BYTE* dst = nullptr;
    DWORD capacity = 0;
    hr = buffer->Lock(&dst, &capacity, nullptr);
    if (FAILED(hr)) return hr;
    memcpy(dst, packet.data(), packet.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(packet.size()));

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) return hr;

    const LONGLONG duration = 10'000'000LL / std::max<std::uint32_t>(1, fps_);
    sample->SetSampleTime(timestamp_);
    sample->SetSampleDuration(duration);
    timestamp_ += duration;

    hr = decoder_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        const HRESULT drainHr = Drain();
        if (FAILED(drainHr)) return drainHr;
        hr = decoder_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return hr;
    return Drain();
}

HRESULT H264Decoder::Drain() {
    if (!decoder_) return MF_E_NOT_INITIALIZED;

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    HRESULT hr = decoder_->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) return hr;

    const DWORD expected = width_ * height_ * 3 / 2;
    for (;;) {
        ComPtr<IMFSample> sample;
        if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            hr = MFCreateSample(&sample);
            if (FAILED(hr)) return hr;
            ComPtr<IMFMediaBuffer> outputBuffer;
            hr = MFCreateMemoryBuffer(std::max<DWORD>(streamInfo.cbSize, expected), &outputBuffer);
            if (FAILED(hr)) return hr;
            hr = sample->AddBuffer(outputBuffer.Get());
            if (FAILED(hr)) return hr;
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = sample.Get();
        DWORD status = 0;
        hr = decoder_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> newType;
            for (DWORD index = 0; SUCCEEDED(decoder_->GetOutputAvailableType(0, index, &newType)); ++index) {
                GUID subtype{};
                if (SUCCEEDED(newType->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
                    decoder_->SetOutputType(0, newType.Get(), 0);
                    break;
                }
                newType.Reset();
            }
            continue;
        }
        if (FAILED(hr)) return hr;

        ComPtr<IMFSample> decoded = output.pSample ? output.pSample : sample;
        if (!decoded) continue;
        ComPtr<IMFMediaBuffer> contiguous;
        hr = decoded->ConvertToContiguousBuffer(&contiguous);
        if (FAILED(hr)) return hr;

        BYTE* data = nullptr;
        DWORD maxLength = 0;
        DWORD currentLength = 0;
        hr = contiguous->Lock(&data, &maxLength, &currentLength);
        if (FAILED(hr)) return hr;
        if (currentLength > 0 && handler_) {
            std::vector<std::uint8_t> frame(data, data + currentLength);
            contiguous->Unlock();
            handler_(std::move(frame));
        } else {
            contiguous->Unlock();
        }
    }
}

void H264Decoder::Reset() {
    if (decoder_) {
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    decoder_.Reset();
    handler_ = nullptr;
    width_ = 0;
    height_ = 0;
    fps_ = 15;
    timestamp_ = 0;
}
