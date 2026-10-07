#include "H264Decoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <algorithm>
#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

struct NalUnit {
    std::size_t start = 0;
    std::size_t end = 0;
    int type = 0;
};

std::size_t StartCodeLength(const std::vector<std::uint8_t>& data, std::size_t pos) {
    if (pos + 4 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1) {
        return 4;
    }
    if (pos + 3 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
        return 3;
    }
    return 0;
}

bool StartsWithAnnexB(const std::vector<std::uint8_t>& data) {
    return StartCodeLength(data, 0) != 0;
}

std::vector<std::uint8_t> NormalizeAnnexB(const std::vector<std::uint8_t>& packet) {
    if (packet.empty() || StartsWithAnnexB(packet)) return packet;

    // Some encoders expose AVC/avcC style packets: [4-byte big-endian length][NAL]...
    // Convert them to the Annex B representation expected by the Windows H.264 decoder.
    std::vector<std::uint8_t> converted;
    std::size_t pos = 0;
    bool parsedLengthPrefixed = false;
    while (pos + 4 <= packet.size()) {
        const std::uint32_t length =
            (static_cast<std::uint32_t>(packet[pos]) << 24) |
            (static_cast<std::uint32_t>(packet[pos + 1]) << 16) |
            (static_cast<std::uint32_t>(packet[pos + 2]) << 8) |
            static_cast<std::uint32_t>(packet[pos + 3]);
        pos += 4;
        if (length == 0 || pos + length > packet.size()) {
            parsedLengthPrefixed = false;
            break;
        }
        parsedLengthPrefixed = true;
        converted.insert(converted.end(), {0, 0, 0, 1});
        converted.insert(converted.end(), packet.begin() + pos, packet.begin() + pos + length);
        pos += length;
    }

    if (parsedLengthPrefixed && pos == packet.size() && !converted.empty()) {
        return converted;
    }

    // Android AVC CSD is defined as Annex B, but tolerate devices that return a raw NAL.
    converted.clear();
    converted.insert(converted.end(), {0, 0, 0, 1});
    converted.insert(converted.end(), packet.begin(), packet.end());
    return converted;
}

std::vector<NalUnit> ParseNals(const std::vector<std::uint8_t>& data) {
    std::vector<NalUnit> result;
    std::size_t pos = 0;
    while (pos < data.size()) {
        std::size_t code = StartCodeLength(data, pos);
        if (code == 0) {
            ++pos;
            continue;
        }

        const std::size_t nalStart = pos;
        const std::size_t payload = pos + code;
        if (payload >= data.size()) break;

        std::size_t next = payload + 1;
        while (next < data.size() && StartCodeLength(data, next) == 0) ++next;

        result.push_back({nalStart, next, data[payload] & 0x1f});
        pos = next;
    }
    return result;
}

std::vector<std::uint8_t> CopyNal(const std::vector<std::uint8_t>& data, const NalUnit& nal) {
    return std::vector<std::uint8_t>(data.begin() + nal.start, data.begin() + nal.end);
}

} // namespace

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
    started_ = false;

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
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
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

    const auto annexB = NormalizeAnnexB(packet);
    const auto nals = ParseNals(annexB);
    if (nals.empty()) return S_OK;

    bool hasVcl = false;
    bool hasIdr = false;
    bool containsSps = false;
    bool containsPps = false;

    for (const auto& nal : nals) {
        if (nal.type == 7) {
            sps_ = CopyNal(annexB, nal);
            containsSps = true;
        } else if (nal.type == 8) {
            pps_ = CopyNal(annexB, nal);
            containsPps = true;
        } else if (nal.type >= 1 && nal.type <= 5) {
            hasVcl = true;
            hasIdr = hasIdr || nal.type == 5;
        }
    }

    // CSD packets have no frame timestamp and must not be submitted as stand-alone frames.
    if (!hasVcl) return S_OK;

    // Start decoding from an IDR only. This also avoids presenting an arbitrary delta frame
    // when the Windows client connects in the middle of an already running phone stream.
    if (!started_) {
        if (!hasIdr || sps_.empty() || pps_.empty()) return S_OK;
        started_ = true;
    }

    std::vector<std::uint8_t> accessUnit;
    if (hasIdr) {
        // The Microsoft H.264 decoder scans the byte stream for SPS/PPS. Put them directly
        // in front of every IDR so reconnects and decoder resets are deterministic.
        if (!containsSps && !sps_.empty()) accessUnit.insert(accessUnit.end(), sps_.begin(), sps_.end());
        if (!containsPps && !pps_.empty()) accessUnit.insert(accessUnit.end(), pps_.begin(), pps_.end());
    }
    accessUnit.insert(accessUnit.end(), annexB.begin(), annexB.end());

    return SubmitAccessUnit(accessUnit, hasIdr);
}

HRESULT H264Decoder::SubmitAccessUnit(const std::vector<std::uint8_t>& accessUnit, bool keyFrame) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(accessUnit.size()), &buffer);
    if (FAILED(hr)) return hr;

    BYTE* dst = nullptr;
    DWORD capacity = 0;
    hr = buffer->Lock(&dst, &capacity, nullptr);
    if (FAILED(hr)) return hr;
    std::memcpy(dst, accessUnit.data(), accessUnit.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(accessUnit.size()));

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) return hr;

    const LONGLONG duration = 10'000'000LL / std::max<std::uint32_t>(1, fps_);
    sample->SetSampleTime(timestamp_);
    sample->SetSampleDuration(duration);
    sample->SetUINT32(MFSampleExtension_CleanPoint, keyFrame ? TRUE : FALSE);
    if (timestamp_ == 0) sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
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
            bool typeSet = false;
            for (DWORD index = 0; SUCCEEDED(decoder_->GetOutputAvailableType(0, index, &newType)); ++index) {
                GUID subtype{};
                if (SUCCEEDED(newType->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
                    hr = decoder_->SetOutputType(0, newType.Get(), 0);
                    if (FAILED(hr)) return hr;
                    typeSet = true;
                    break;
                }
                newType.Reset();
            }
            if (!typeSet) return MF_E_INVALIDMEDIATYPE;
            hr = decoder_->GetOutputStreamInfo(0, &streamInfo);
            if (FAILED(hr)) return hr;
            continue;
        }
        if (FAILED(hr)) return hr;

        ComPtr<IMFSample> decoded;
        if (output.pSample) {
            if (output.pSample == sample.Get()) {
                decoded = sample;
            } else {
                decoded.Attach(output.pSample);
            }
        }
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
    sps_.clear();
    pps_.clear();
    width_ = 0;
    height_ = 0;
    fps_ = 15;
    timestamp_ = 0;
    started_ = false;
}
